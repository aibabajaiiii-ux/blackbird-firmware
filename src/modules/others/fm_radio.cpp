// RDA5807M FM radio module -- custom addition, not part of stock Bruce.
//
// Minimal self-contained driver written directly against the RDA5807M/MP
// register map (no external Arduino library dependency, since PlatformIO
// library fetches may not always be available when building this board).
// Uses the chip's sequential-write I2C mode: every update sends registers
// 0x02 through 0x07 as 12 bytes starting at I2C address 0x10. Register
// values for 0x04/0x06/0x07 are the manufacturer-recommended defaults from
// public RDA5807M reference code; only 0x02 (power/mute/seek), 0x03
// (channel/tune) and the volume nibble of 0x05 are computed here. The seek
// and RSSI/status bit positions (register 0x0A) follow the same widely-used
// public reference layout -- flagging this the same way the original driver
// note does, since there's no way to check them against a live chip here.
//
// Tuning range: 87.0-108.0 MHz (BAND=00), 100kHz steps (SPACE=00).
//
// On opening this app, it now first offers to connect to a Bluetooth
// speaker/headset (see bt_device_picker.h / bt_audio_source.h) so tuned FM
// audio streams there; choosing "Skip" just leaves you with the always-on
// 3.5mm headphone jack wired straight to the chip's analog output.
//
// 6-button "car radio" control scheme:
//   Left / Right   -- fine-tune 0.1MHz
//   Up / Down      -- volume; HOLD = save/recall the one saved preset
//   Select (tap)   -- seek up to the next station
//   Select (hold)  -- mute / unmute
//   Menu           -- exit (handled globally; disconnects Bluetooth on the
//                     way out, see fm_radio_setup()'s cleanup below)

#include "fm_radio.h"

#ifdef HAS_FM_RADIO

// blackbird_buttons.h resolves via the -Iboards/esp32-devkit-custom build
// flag already set in esp32-devkit-custom.ini (same reason interface.cpp's
// own includes of "blackbird_boot.h" need no relative path).
#include "blackbird_buttons.h"
#include "core/display.h"
#include <Preferences.h>
#include <Wire.h>
#include <globals.h>
#include <string>

#ifdef HAS_BT_AUDIO
#include "bt_audio_source.h"
#include "bt_device_picker.h"
#else
// This board always sets HAS_BT_AUDIO alongside HAS_FM_RADIO, but keep this
// file buildable on its own terms: without it, FM Radio just never offers
// the Bluetooth step and plays only out of the always-on headphone jack.
static inline bool btDevicePickerRun(const char *) { return false; }
static inline void btAudioStopStreaming() {}
static inline void btAudioDisconnect() {}
static inline void btAudioShutdown() {}
static inline std::string btAudioConnectedName() { return ""; }
#endif

#define FM_I2C_ADDR 0x10

#define FM_BASE_KHZ   87000
#define FM_STEP_KHZ   100
#define FM_MIN_CH     0                                        // 87.0 MHz
#define FM_MAX_CH     ((108000 - FM_BASE_KHZ) / FM_STEP_KHZ)   // 108.0 MHz

#define SELECT_HOLD_MS 600 // Select held this long = mute toggle instead of seek

static uint16_t fmChannel = (100000 - FM_BASE_KHZ) / FM_STEP_KHZ; // default 100.0 MHz
static uint8_t fmVolume = 10;                                      // 0-15
static bool fmMuted = false;
static uint8_t fmRssi = 0;      // 0-63 raw, scaled to 0-4 bars for display
static bool fmStereo = false;
static bool btConnected = false;

static void fmWriteState(bool seeking = false, bool seekUp = true) {
    // 0x02: DHIZ=1 (normal audio out), DMUTE=!fmMuted, ENABLE=1, plus the
    // seek bits when a seek is being kicked off.
    uint16_t reg02 = (1 << 15) | (1 << 0) | (fmMuted ? 0 : (1 << 14));
    if (seeking) {
        reg02 |= (1 << 10);            // SEEK=1
        if (seekUp) reg02 |= (1 << 11); // SEEKUP
        // SKMODE left 0 -> wraps around band edge instead of stopping there.
    }

    // 0x03: CHAN<<6 | TUNE=1 (only meaningful outside a seek) | BAND=00 | SPACE=00
    uint16_t reg03 = (uint16_t)((fmChannel & 0x3FF) << 6) | (seeking ? 0 : (1 << 4));

    uint16_t reg04 = 0x0400; // de-emphasis default
    uint16_t reg05 = (uint16_t)(0xC3A0 | (fmVolume & 0x0F));
    uint16_t reg06 = 0x6000; // reference default
    uint16_t reg07 = 0x4212; // reference default (soft-blend etc.)

    uint16_t regs[6] = {reg02, reg03, reg04, reg05, reg06, reg07};

    Wire.beginTransmission(FM_I2C_ADDR);
    for (int i = 0; i < 6; i++) {
        Wire.write((uint8_t)(regs[i] >> 8));
        Wire.write((uint8_t)(regs[i] & 0xFF));
    }
    Wire.endTransmission();
}

// Reads back status registers 0x0A/0x0B in the chip's sequential read mode
// (a plain I2C read starting at the fixed status-register address). Updates
// fmRssi/fmStereo, and if a seek was in progress, waits (briefly) for the
// chip to report STC (seek/tune complete) and returns the channel it landed
// on. This bit layout is the common public-reference one for this chip
// family -- not independently verified against a datasheet copy here.
static bool fmWaitSeekComplete(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        Wire.requestFrom((uint8_t)FM_I2C_ADDR, (uint8_t)4);
        if (Wire.available() >= 4) {
            uint16_t regA = ((uint16_t)Wire.read() << 8) | Wire.read();
            uint16_t regB = ((uint16_t)Wire.read() << 8) | Wire.read();
            bool stc = regA & (1 << 14);
            if (stc) {
                fmChannel = regA & 0x03FF;
                fmRssi = (uint8_t)((regB >> 9) & 0x7F);
                fmStereo = regA & (1 << 10);
                return true;
            }
        }
        delay(20);
    }
    return false;
}

static void fmRefreshStatus() {
    Wire.requestFrom((uint8_t)FM_I2C_ADDR, (uint8_t)4);
    if (Wire.available() >= 4) {
        uint16_t regA = ((uint16_t)Wire.read() << 8) | Wire.read();
        uint16_t regB = ((uint16_t)Wire.read() << 8) | Wire.read();
        fmStereo = regA & (1 << 10);
        fmRssi = (uint8_t)((regB >> 9) & 0x7F);
    }
}

static void fmInit() {
    // Soft reset pulse, then power up in the normal (non-reset) state.
    Wire.beginTransmission(FM_I2C_ADDR);
    Wire.write(0xC0);
    Wire.write(0x02); // DHIZ=1, DMUTE=1, SOFT_RESET=1, ENABLE=0
    for (int i = 0; i < 10; i++) Wire.write((uint8_t)0x00);
    Wire.endTransmission();
    delay(50);

    fmWriteState();
    delay(100); // let the PLL settle on the initial channel
}

static float fmFrequencyMHz() { return (FM_BASE_KHZ + (uint32_t)fmChannel * FM_STEP_KHZ) / 1000.0f; }

// ---------------------------------------------------------------------------
// Single-slot station preset, persisted in NVS (survives power-off).
static void presetSave() {
    Preferences prefs;
    prefs.begin("bb_fm", false);
    prefs.putUShort("chan", fmChannel);
    prefs.end();
}
static bool presetRecall() {
    Preferences prefs;
    prefs.begin("bb_fm", true);
    bool has = prefs.isKey("chan");
    if (has) fmChannel = prefs.getUShort("chan", fmChannel);
    prefs.end();
    return has;
}

// ---------------------------------------------------------------------------
static void fmDraw() {
    drawMainBorderWithTitle("FM Radio");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);

    tft.setTextSize(FM);
    padprintln("");
    padprintln(String(fmFrequencyMHz(), 1) + " MHz" + (fmStereo ? " (st)" : " (mono)"));

    tft.setTextSize(FP);
    // Signal bars: scale the 0-63 raw RSSI down to 0-4 bars.
    int bars = fmRssi * 4 / 63;
    String sig = "Signal: [";
    for (int i = 0; i < 4; i++) sig += (i < bars) ? "#" : "-";
    sig += "]";
    padprintln(sig);

    padprintln(String("Vol: ") + String(fmVolume) + (fmMuted ? "  [MUTED]" : ""));
    padprintln(btConnected ? (String("BT: ") + String(btAudioConnectedName().c_str()))
                            : "BT: off (headphone jack only)");
    padprintln("");
    padprintln("L/R: fine tune   Up/Dn: volume");
    padprintln("Sel tap: seek    Sel hold: mute");
    padprintln("Hold Up/Dn: save/recall preset");
}

void fm_radio_setup() {
    fmInit();
    presetRecall();

    // --- Bluetooth: offer to connect to a speaker before showing the main
    // screen. See bt_device_picker.h / bt_audio_source.h for the full
    // explanation of why this is the riskiest subsystem in this build.
    btConnected = btDevicePickerRun("Scanning for speakers...");

    fmWriteState();
    fmRefreshStatus();
    fmDraw();

    // Tap-vs-hold state for Up/Down/Select. Bruce's UpPress/DownPress/
    // SelPress are one-shot edges with no "is it still held" signal, so
    // hold detection here polls the PCF8574T's live state directly via
    // blackbird_buttons.h instead -- the same workaround the Menu button's
    // walk-to-home burst already needed for the same underlying reason.
    uint32_t upHeldSince = 0, downHeldSince = 0, selHeldSince = 0;
    bool upHoldFired = false, downHoldFired = false, selHoldFired = false;

    uint32_t lastStatusPoll = millis();

    for (;;) {
        bool changed = false;

        if (check(EscPress)) break;
        check(UpPress);   // drain these edge flags -- this screen's volume/
        check(DownPress); // seek actions below are driven entirely by the
        check(SelPress);  // held-state polling, not these one-shot edges.

        if (check(NextPress)) { // Right
            if (fmChannel < FM_MAX_CH) fmChannel++;
            changed = true;
        }
        if (check(PrevPress)) { // Left
            if (fmChannel > FM_MIN_CH) fmChannel--;
            changed = true;
        }

        // --- Up: tap = volume+1, hold = save preset ---
        bool upHeld = blackbirdBtnHeldUp();
        if (upHeld && upHeldSince == 0) { upHeldSince = millis(); upHoldFired = false; }
        if (upHeld && upHeldSince && !upHoldFired && (millis() - upHeldSince) >= SELECT_HOLD_MS) {
            presetSave();
            upHoldFired = true;
            tft.fillRect(0, tftHeight - 12, tftWidth, 12, bruceConfig.bgColor);
            tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
            tft.drawCentreString("Preset saved", tftWidth / 2, tftHeight - 12, SMOOTH_FONT);
            delay(400);
            changed = true;
        }
        if (!upHeld && upHeldSince) {
            if (!upHoldFired && fmVolume < 15) { fmVolume++; changed = true; }
            upHeldSince = 0;
        }

        // --- Down: tap = volume-1, hold = recall preset ---
        bool downHeld = blackbirdBtnHeldDown();
        if (downHeld && downHeldSince == 0) { downHeldSince = millis(); downHoldFired = false; }
        if (downHeld && downHeldSince && !downHoldFired && (millis() - downHeldSince) >= SELECT_HOLD_MS) {
            downHoldFired = true;
            if (presetRecall()) {
                tft.fillRect(0, tftHeight - 12, tftWidth, 12, bruceConfig.bgColor);
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
                tft.drawCentreString("Preset recalled", tftWidth / 2, tftHeight - 12, SMOOTH_FONT);
                delay(400);
                changed = true;
            }
        }
        if (!downHeld && downHeldSince) {
            if (!downHoldFired && fmVolume > 0) { fmVolume--; changed = true; }
            downHeldSince = 0;
        }

        // --- Select: tap = seek up, hold = mute toggle ---
        bool selHeld = blackbirdBtnHeldSelect();
        if (selHeld && selHeldSince == 0) { selHeldSince = millis(); selHoldFired = false; }
        if (selHeld && selHeldSince && !selHoldFired && (millis() - selHeldSince) >= SELECT_HOLD_MS) {
            fmMuted = !fmMuted;
            selHoldFired = true;
            changed = true;
        }
        if (!selHeld && selHeldSince) {
            if (!selHoldFired) {
                fmDraw();
                tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
                tft.drawCentreString("Seeking...", tftWidth / 2, tftHeight / 2, SMOOTH_FONT);
                fmWriteState(/*seeking=*/true, /*seekUp=*/true);
                fmWaitSeekComplete(4000);
                fmWriteState(); // clear SEEK bit, settle on the found channel
                changed = true;
            }
            selHeldSince = 0;
        }

        // Slow periodic refresh of signal/stereo indicator even with no
        // button activity, so the bars aren't frozen from init time.
        if (millis() - lastStatusPoll > 1500) {
            fmRefreshStatus();
            lastStatusPoll = millis();
            changed = true;
        }

        if (changed) {
            fmWriteState();
            fmDraw();
        }

        delay(10);
    }

    if (btConnected) {
        // Full teardown, not just disconnect -- this also switches WiFi
        // back on. See bt_audio_source.h's btAudioShutdown() for why the
        // Bluetooth stack isn't left resident between visits to this app.
        btAudioShutdown();
    }
}

#endif // HAS_FM_RADIO
