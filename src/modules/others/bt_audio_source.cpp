// Classic-Bluetooth A2DP source -- see bt_audio_source.h for the full risk
// disclosure. Written directly against the ESP-IDF classic-BT/Bluedroid
// headers bundled inside the arduino-esp32 core (esp_bt.h, esp_bt_main.h,
// esp_gap_bt_api.h, esp_a2dp_api.h) rather than a third-party library --
// these ship with the framework already, so this builds without any new
// `lib_deps` fetch.
//
// *** THIS IS THE RISKIEST PIECE OF THE WHOLE BUILD -- see bt_audio_source.h
// for why (classic BT + BLE + WiFi all live on one plain ESP32's RAM). ***
//
// Two audio sources feed the same A2DP connection, one at a time:
//  - FM path: RDA5807M analog L/R -> mixer/bias circuit -> GPIO36 (the one
//    remaining free ADC1-capable pin) -> ESP32 ADC continuous/DMA driver,
//    16kHz mono.
//  - File path (Play Music): a 16-bit PCM .wav read off the SD card.
// Both get converted into a shared int16 mono ring buffer at a known
// sample rate; one resampler (nearest-neighbour, not a proper filtered
// interpolator -- a known, disclosed quality compromise) upsamples whichever
// is active to the 44.1kHz stereo frames A2DP expects.
//
// NOT implemented / known rough edges:
//  - No AVRCP (so the connected speaker's own volume/skip buttons do
//    nothing -- control only from Blackbird).
//  - SSP/pairing auto-accepts "Just Works" -- no PIN-entry UI.
//  - Only uncompressed PCM .wav for file playback, not MP3/AAC/etc.
//  - No tested hardware exists for any of this -- best-effort draft only.

#include "bt_audio_source.h"

#ifdef HAS_BT_AUDIO

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

extern "C" {
#include "esp_a2dp_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
}

// This is the ESP-IDF 5.x continuous/DMA ADC API (adc_continuous_new_handle,
// etc.) -- confirmed correct for this project's pinned platform
// (platformio.ini uses Arduino-ESP32 core 3.3.x, which is built on IDF 5.x).
// An older Arduino-ESP32 2.x core would need the different/older
// driver/adc.h continuous API instead.
#include "esp_adc/adc_continuous.h"

// ---------------------------------------------------------------------------
// State
static bool btReady = false;
static bool wifiWasOnBeforeBt = false; // so btAudioShutdown() knows whether to bring it back
static std::vector<BtAudioDevice> scanResults;
static volatile bool scanDone = true;
static volatile bool connected = false;
static volatile bool connectFailed = false;
static uint8_t connectedBda[6] = {0};
static std::string connectedName;
static volatile uint8_t softwareGainPct = 100;

enum AudioSourceMode { SRC_NONE, SRC_ADC, SRC_FILE };
static volatile AudioSourceMode activeSource = SRC_NONE;

// ---------------------------------------------------------------------------
// Shared PCM ring buffer: signed, already-centered 16-bit mono samples at
// `ringSampleRate` Hz, however they got there (ADC or file decode).
#define PCM_RING_SIZE 4096
static int16_t pcmRing[PCM_RING_SIZE];
static volatile uint32_t ringWriteIdx = 0;
static volatile uint32_t ringReadIdx = 0;
static volatile uint32_t ringSampleRate = 16000;

static inline void ringPush(int16_t sample) {
    pcmRing[ringWriteIdx % PCM_RING_SIZE] = sample;
    ringWriteIdx++;
}

// ---------------------------------------------------------------------------
// --- ADC capture path (FM) ---
#define ADC_SAMPLE_RATE_HZ 16000
static adc_continuous_handle_t adcHandle = nullptr;
static bool adcRunning = false;

static bool IRAM_ATTR adcConvDoneCb(adc_continuous_handle_t handle,
                                     const adc_continuous_evt_data_t *edata, void *user_data) {
    uint32_t n = edata->size / sizeof(adc_digi_output_data_t);
    adc_digi_output_data_t *rec = (adc_digi_output_data_t *)edata->conv_frame_buffer;
    for (uint32_t i = 0; i < n; i++) {
        int32_t raw = rec[i].type2.data & 0x0FFF; // 12-bit, centered ~2048 by the bias network
        int32_t centered = (raw - 2048) * 8;      // scale toward 16-bit headroom
        if (centered > 32767) centered = 32767;
        if (centered < -32768) centered = -32768;
        ringPush((int16_t)centered);
    }
    return false;
}

static void adcCaptureStart() {
    if (adcRunning) return;

    adc_continuous_handle_cfg_t handleCfg = {
        .max_store_buf_size = 1024 * sizeof(adc_digi_output_data_t),
        .conv_frame_size = 256 * sizeof(adc_digi_output_data_t),
    };
    if (adc_continuous_new_handle(&handleCfg, &adcHandle) != ESP_OK) return;

    adc_digi_pattern_config_t pattern = {
        .atten = ADC_ATTEN_DB_11,
        // ADC_CHANNEL_0 on ADC_UNIT_1 = GPIO36, matching FM_AUDIO_ADC_PIN in
        // esp32-devkit-custom.ini. Hardcoded rather than read from that flag
        // since the ESP-IDF adc_continuous API wants a channel enum, not a
        // raw GPIO number.
        .channel = ADC_CHANNEL_0,
        .unit = ADC_UNIT_1,
        .bit_width = ADC_BITWIDTH_12,
    };
    adc_continuous_config_t digCfg = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = ADC_SAMPLE_RATE_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    if (adc_continuous_config(adcHandle, &digCfg) != ESP_OK) return;

    adc_continuous_evt_cbs_t cbs = {.on_conv_done = adcConvDoneCb};
    adc_continuous_register_event_callbacks(adcHandle, &cbs);

    if (adc_continuous_start(adcHandle) == ESP_OK) adcRunning = true;
}

static void adcCaptureStop() {
    if (!adcRunning) return;
    adc_continuous_stop(adcHandle);
    adc_continuous_deinit(adcHandle);
    adcHandle = nullptr;
    adcRunning = false;
}

// ---------------------------------------------------------------------------
// --- SD-card .wav file playback path (Play Music) ---
static File wavFile;
static uint32_t wavDataStart = 0, wavDataEnd = 0, wavPos = 0;
static uint16_t wavChannels = 1, wavBitsPerSample = 16;
static uint32_t wavSampleRate = 44100;
static TaskHandle_t fileTaskHandle = nullptr;
static volatile bool fileTaskRunning = false;
static volatile bool fileFinished = false;

// Minimal RIFF/WAVE 'fmt '+'data' chunk scan -- handles the common case
// (no odd extra chunks before 'data') but isn't a general WAV parser.
static bool wavParseHeader(File &f) {
    uint8_t hdr[12];
    if (f.read(hdr, 12) != 12) return false;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

    while (f.available()) {
        uint8_t chunkId[4];
        uint8_t chunkSizeBytes[4];
        if (f.read(chunkId, 4) != 4) return false;
        if (f.read(chunkSizeBytes, 4) != 4) return false;
        uint32_t chunkSize = chunkSizeBytes[0] | (chunkSizeBytes[1] << 8) | (chunkSizeBytes[2] << 16) |
                             ((uint32_t)chunkSizeBytes[3] << 24);

        if (memcmp(chunkId, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunkSize < 16 || f.read(fmt, 16) != 16) return false;
            wavChannels = fmt[2] | (fmt[3] << 8);
            wavSampleRate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            wavBitsPerSample = fmt[14] | (fmt[15] << 8);
            if (chunkSize > 16) f.seek(f.position() + (chunkSize - 16));
        } else if (memcmp(chunkId, "data", 4) == 0) {
            wavDataStart = f.position();
            wavDataEnd = wavDataStart + chunkSize;
            return true;
        } else {
            f.seek(f.position() + chunkSize); // skip unknown chunk (LIST, etc.)
        }
    }
    return false;
}

static void fileTask(void *arg) {
    const int CHUNK_SAMPLES = 256;
    uint8_t raw[CHUNK_SAMPLES * 4]; // worst case: stereo 16-bit

    while (fileTaskRunning) {
        // Don't get too far ahead of the consumer -- keep ~half the ring
        // buffer as headroom so we don't overwrite unread samples.
        if ((ringWriteIdx - ringReadIdx) > (PCM_RING_SIZE / 2)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (wavPos >= wavDataEnd) {
            fileFinished = true;
            break;
        }

        int bytesPerFrame = (wavBitsPerSample / 8) * wavChannels;
        int framesToRead = CHUNK_SAMPLES;
        int bytesToRead = framesToRead * bytesPerFrame;
        int available = wavDataEnd - wavPos;
        if (bytesToRead > available) bytesToRead = available;
        if (bytesToRead <= 0) { fileFinished = true; break; }

        int got = wavFile.read(raw, bytesToRead);
        if (got <= 0) { fileFinished = true; break; }
        wavPos += got;

        int frames = got / bytesPerFrame;
        for (int i = 0; i < frames; i++) {
            int16_t left, right = 0;
            uint8_t *p = raw + i * bytesPerFrame;
            if (wavBitsPerSample == 16) {
                left = (int16_t)(p[0] | (p[1] << 8));
                if (wavChannels == 2) right = (int16_t)(p[2] | (p[3] << 8));
            } else { // 8-bit unsigned PCM
                left = (int16_t)(((int32_t)p[0] - 128) * 256);
                if (wavChannels == 2) right = (int16_t)(((int32_t)p[1] - 128) * 256);
            }
            int16_t mono = (wavChannels == 2) ? (int16_t)(((int32_t)left + right) / 2) : left;
            ringPush(mono);
        }
    }

    fileTaskRunning = false;
    vTaskDelete(nullptr);
}

bool btAudioPlayWavFile(const char *path) {
    btAudioStopFile();
    adcCaptureStop();

    wavFile = SD.open(path, FILE_READ);
    if (!wavFile) return false;
    if (!wavParseHeader(wavFile)) {
        wavFile.close();
        return false;
    }
    if (wavBitsPerSample != 16 && wavBitsPerSample != 8) {
        wavFile.close();
        return false; // only 8/16-bit PCM supported
    }

    wavPos = wavDataStart;
    wavFile.seek(wavDataStart);
    ringWriteIdx = 0;
    ringReadIdx = 0;
    ringSampleRate = wavSampleRate;
    fileFinished = false;
    activeSource = SRC_FILE;
    fileTaskRunning = true;
    xTaskCreatePinnedToCore(fileTask, "bb_wav", 4096, nullptr, 1, &fileTaskHandle, 0);
    return true;
}

void btAudioStopFile() {
    if (fileTaskRunning) {
        fileTaskRunning = false;
        delay(20); // let the task notice and self-delete
    }
    if (wavFile) wavFile.close();
    if (activeSource == SRC_FILE) activeSource = SRC_NONE;
}

bool btAudioFilePlaybackActive() { return activeSource == SRC_FILE && fileTaskRunning; }
bool btAudioFileFinished() { return fileFinished; }

// ---------------------------------------------------------------------------
// A2DP data callback: Bluedroid calls this whenever it needs more PCM.
// `len` is in bytes; the profile default is 44.1kHz 16-bit stereo (4 bytes/
// frame). Nearest-neighbour-resamples whichever source is active up to
// that rate and duplicates mono into both channels.
static uint32_t resamplePhaseQ16 = 0;

static int32_t a2dpDataCb(uint8_t *data, int32_t len) {
    if (activeSource == SRC_NONE) {
        memset(data, 0, len);
        return len;
    }

    uint32_t stepQ16 = (uint32_t)(((uint64_t)ringSampleRate << 16) / 44100);
    int32_t framesNeeded = len / 4;

    for (int32_t i = 0; i < framesNeeded; i++) {
        resamplePhaseQ16 += stepQ16;
        while (resamplePhaseQ16 >= (1u << 16)) {
            resamplePhaseQ16 -= (1u << 16);
            if (ringReadIdx + 1 <= ringWriteIdx) ringReadIdx++;
        }

        int16_t sample = pcmRing[ringReadIdx % PCM_RING_SIZE];
        int32_t scaled = ((int32_t)sample * (int32_t)softwareGainPct) / 100;
        if (scaled > 32767) scaled = 32767;
        if (scaled < -32768) scaled = -32768;

        int16_t *out = (int16_t *)(data + i * 4);
        out[0] = (int16_t)scaled; // L
        out[1] = (int16_t)scaled; // R
    }
    return framesNeeded * 4;
}

// ---------------------------------------------------------------------------
// GAP (device discovery) callback
static void gapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            BtAudioDevice dev;
            memcpy(dev.bda, param->disc_res.bda, 6);
            dev.name = "(unknown)";
            for (int i = 0; i < param->disc_res.num_prop; i++) {
                esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
                if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                    dev.name = std::string((char *)p->val, p->len);
                    break;
                }
            }
            for (auto &existing : scanResults) {
                if (memcmp(existing.bda, dev.bda, 6) == 0) return;
            }
            scanResults.push_back(dev);
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) scanDone = true;
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true); // "Just Works", no PIN UI
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// A2DP connection/audio-state callback
static void a2dCb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                connected = true;
                connectFailed = false;
                memcpy(connectedBda, param->conn_stat.remote_bda, 6);
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                connected = false;
                adcCaptureStop();
                btAudioStopFile();
                activeSource = SRC_NONE;
            }
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
bool btAudioInit() {
    if (btReady) return true;

    // Take WiFi down first. Classic Bluetooth (BR/EDR, required for A2DP)
    // shares the same physical radio as WiFi, and the two coexisting is the
    // least-tested, highest-risk combination on this chip -- see
    // bt_audio_source.h for the full reasoning. Rather than hoping the
    // coexistence logic handles it gracefully, we just don't run them at
    // the same time: WiFi goes off for as long as this feature is active,
    // and btAudioShutdown() brings it back when you leave.
    wifiWasOnBeforeBt = (WiFi.getMode() != WIFI_OFF);
    if (wifiWasOnBeforeBt) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(100); // let the radio actually release before handing it to BT
    }

    esp_bt_controller_config_t btCfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    // BTDM = classic BT + BLE dual mode -- Bruce's existing BLE features
    // (scanning/spoofing elsewhere in the menu) still need BLE alongside
    // this, just not WiFi at the same time too.
    if (esp_bt_controller_init(&btCfg) != ESP_OK) {
        if (wifiWasOnBeforeBt) WiFi.mode(WIFI_STA);
        return false;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_BTDM) != ESP_OK) {
        esp_bt_controller_deinit();
        if (wifiWasOnBeforeBt) WiFi.mode(WIFI_STA);
        return false;
    }

    if (esp_bluedroid_init() != ESP_OK) return false;
    if (esp_bluedroid_enable() != ESP_OK) return false;

    esp_bt_dev_set_device_name("Blackbird");
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    esp_bt_gap_register_callback(gapCb);
    // esp_bt_gap_set_security_param wants a POINTER to the value, not the
    // value cast to a pointer -- a static here keeps that pointer valid.
    static esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));

    esp_a2d_register_callback(a2dCb);
    esp_a2d_source_register_data_callback(a2dpDataCb);
    esp_a2d_source_init();

    btReady = true;
    return true;
}

void btAudioShutdown() {
    if (!btReady) return;

    btAudioDisconnect(); // also stops ADC capture + file playback
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    btReady = false;
    connected = false;
    activeSource = SRC_NONE;

    if (wifiWasOnBeforeBt) {
        // Minimal restore: puts the radio back in station mode. This does
        // NOT automatically reconnect to your saved network -- if Bruce's
        // own WiFi menu has auto-reconnect logic, that's separate from this
        // file and isn't triggered here. You may need to reconnect manually
        // from the WiFi menu after leaving FM Radio / Play Music.
        WiFi.mode(WIFI_STA);
        wifiWasOnBeforeBt = false;
    }
}

void btAudioStartScan() {
    if (!btReady) return;
    scanResults.clear();
    scanDone = false;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 8, 0); // ~10s
}

bool btAudioScanComplete() { return scanDone; }

std::vector<BtAudioDevice> btAudioGetScanResults() { return scanResults; }

void btAudioConnect(const uint8_t *bda) {
    if (!btReady) return;
    connectFailed = false;
    memcpy(connectedBda, bda, 6);
    if (esp_a2d_source_connect((uint8_t *)bda) != ESP_OK) connectFailed = true;
}

void btAudioDisconnect() {
    if (!btReady || !connected) return;
    esp_a2d_source_disconnect(connectedBda);
    adcCaptureStop();
    btAudioStopFile();
}

bool btAudioIsConnected() { return connected; }
bool btAudioConnectFailed() { return connectFailed; }

std::string btAudioConnectedName() {
    for (auto &d : scanResults) {
        if (memcmp(d.bda, connectedBda, 6) == 0) return d.name;
    }
    return "(connected)";
}

void btAudioStartStreaming() {
    if (!connected) return;
    btAudioStopFile();
    ringWriteIdx = 0;
    ringReadIdx = 0;
    ringSampleRate = ADC_SAMPLE_RATE_HZ;
    resamplePhaseQ16 = 0;
    activeSource = SRC_ADC;
    adcCaptureStart();
}

void btAudioStopStreaming() {
    adcCaptureStop();
    if (activeSource == SRC_ADC) activeSource = SRC_NONE;
}

void btAudioSetGain(uint8_t gainPct) { softwareGainPct = gainPct > 100 ? 100 : gainPct; }

#endif // HAS_BT_AUDIO
