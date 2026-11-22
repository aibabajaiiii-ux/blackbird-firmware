#include "bt_device_picker.h"

#ifdef HAS_BT_AUDIO

#include "bt_audio_source.h"
#include "core/display.h"
#include <Arduino.h>
#include <globals.h>

// Hand-rolled draw loop (same pattern as the boot lock screen in
// blackbird_boot.cpp) rather than the generic loopOptions() helper, because
// this list has to redraw itself as devices trickle in asynchronously from
// the BT scan callback -- loopOptions() only redraws in response to a button
// press, with nothing to prod it externally when new data just arrived.

static void drawPicker(const std::vector<BtAudioDevice> &results, int selected, bool scanning,
                        const char *titleWhileScanning) {
    drawMainBorderWithTitle(scanning ? titleWhileScanning : "Bluetooth Speakers");
    tft.setTextSize(FP);

    int row = 0;
    auto drawRow = [&](const String &text, bool isSel) {
        tft.setTextColor(isSel ? bruceConfig.bgColor : bruceConfig.priColor,
                          isSel ? bruceConfig.priColor : bruceConfig.bgColor);
        padprintln(text);
        row++;
    };

    drawRow(scanning ? "[Scanning...]" : "Rescan", selected == 0);
    drawRow("Skip (no Bluetooth)", selected == 1);
    for (size_t i = 0; i < results.size(); i++) {
        String label = String(results[i].name.c_str());
        if (label.length() == 0) label = "(unknown)";
        drawRow(label, selected == (int)(i + 2));
    }
}

static void drawConnecting(const String &name) {
    tft.fillScreen(bruceConfig.bgColor);
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(FM);
    tft.drawCentreString("Connecting to", tftWidth / 2, tftHeight / 2 - 20, SMOOTH_FONT);
    tft.drawCentreString(name, tftWidth / 2, tftHeight / 2 + 10, SMOOTH_FONT);
}

static void drawFailed() {
    tft.fillScreen(bruceConfig.bgColor);
    tft.setTextColor(TFT_RED, bruceConfig.bgColor);
    tft.setTextSize(FM);
    tft.drawCentreString("Connect failed", tftWidth / 2, tftHeight / 2, SMOOTH_FONT);
    delay(1200);
}

bool btDevicePickerRun(const char *titleWhileScanning) {
    if (!btAudioInit()) {
        tft.fillScreen(bruceConfig.bgColor);
        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        tft.setTextSize(FP);
        tft.drawCentreString("BT stack failed to start", tftWidth / 2, tftHeight / 2, SMOOTH_FONT);
        delay(1500);
        return false;
    }

    btAudioStartScan();

    int selected = 0;
    size_t lastCount = (size_t)-1;
    bool lastScanDone = false;
    std::vector<BtAudioDevice> results;

    drawPicker(results, selected, true, titleWhileScanning);

    for (;;) {
        results = btAudioGetScanResults();
        bool scanDone = btAudioScanComplete();
        if (results.size() != lastCount || scanDone != lastScanDone) {
            lastCount = results.size();
            lastScanDone = scanDone;
            drawPicker(results, selected, !scanDone, titleWhileScanning);
        }

        int maxIdx = (int)results.size() + 1; // 0=Rescan, 1=Skip, 2..=devices

        if (check(UpPress)) {
            selected = (selected == 0) ? maxIdx : selected - 1;
            drawPicker(results, selected, !scanDone, titleWhileScanning);
        }
        if (check(DownPress)) {
            selected = (selected >= maxIdx) ? 0 : selected + 1;
            drawPicker(results, selected, !scanDone, titleWhileScanning);
        }

        if (check(SelPress)) {
            if (selected == 0) {
                btAudioStartScan();
                lastCount = (size_t)-1;
            } else if (selected == 1) {
                return false; // user chose to skip Bluetooth
            } else {
                const BtAudioDevice &dev = results[selected - 2];
                String name = String(dev.name.c_str());
                if (name.length() == 0) name = "(unknown)";
                drawConnecting(name);
                btAudioConnect(dev.bda);

                uint32_t t0 = millis();
                while (!btAudioIsConnected() && !btAudioConnectFailed() && millis() - t0 < 8000) {
                    delay(50);
                }

                if (btAudioIsConnected()) {
                    btAudioStartStreaming();
                    return true;
                }
                drawFailed();
                drawPicker(results, selected, !scanDone, titleWhileScanning);
            }
        }

        if (check(EscPress)) return false; // backed out (Menu button) -- play/tune locally only

        delay(10);
    }
}

#endif // HAS_BT_AUDIO
