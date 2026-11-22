// Stopwatch -- not stock Bruce. See stopwatch.h.
//
// Controls: Select = start/pause, Up = record a lap (while running), Down =
// reset to zero (only while paused, to avoid an accidental wipe mid-run),
// Menu/Esc = exit (elapsed time is NOT persisted -- exiting resets it; this
// is a stopwatch, not a saved-state timer).

#include "stopwatch.h"

#include "core/display.h"
#include <Arduino.h>
#include <globals.h>
#include <vector>

#define MAX_LAPS_SHOWN 5

static String fmtElapsed(uint32_t ms) {
    uint32_t tenths = (ms / 100) % 10;
    uint32_t secs = (ms / 1000) % 60;
    uint32_t mins = (ms / 60000) % 60;
    uint32_t hours = (ms / 3600000);
    char buf[16];
    if (hours > 0) {
        snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", (unsigned long)hours, (unsigned long)mins, (unsigned long)secs);
    } else {
        snprintf(buf, sizeof(buf), "%02lu:%02lu.%lu", (unsigned long)mins, (unsigned long)secs, (unsigned long)tenths);
    }
    return String(buf);
}

static void drawStopwatch(bool running, uint32_t elapsed, const std::vector<uint32_t> &laps) {
    drawMainBorderWithTitle("Stopwatch");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);

    tft.setTextSize(FM);
    padprintln("");
    padprintln(fmtElapsed(elapsed));

    tft.setTextSize(FP);
    padprintln("");
    padprintln(running ? "[Running]" : "[Paused]");
    padprintln("");
    padprintln("Sel: start/pause");
    padprintln("Up: lap   Down: reset");
    padprintln("");

    int shown = 0;
    for (int i = (int)laps.size() - 1; i >= 0 && shown < MAX_LAPS_SHOWN; i--, shown++) {
        padprintln(String("Lap ") + String(i + 1) + ": " + fmtElapsed(laps[i]));
    }
}

void stopwatch_setup() {
    bool running = false;
    uint32_t accumulated = 0; // ms banked across previous start/pause cycles
    uint32_t runStart = 0;
    std::vector<uint32_t> laps;

    auto elapsedNow = [&]() -> uint32_t { return accumulated + (running ? (millis() - runStart) : 0); };

    drawStopwatch(running, elapsedNow(), laps);
    uint32_t lastDraw = millis();

    for (;;) {
        if (check(EscPress)) break;

        bool changed = false;

        if (check(SelPress)) {
            if (running) {
                accumulated += millis() - runStart;
                running = false;
            } else {
                runStart = millis();
                running = true;
            }
            changed = true;
        }
        if (check(UpPress)) {
            if (running) laps.push_back(elapsedNow());
            changed = true;
        }
        if (check(DownPress)) {
            if (!running) {
                accumulated = 0;
                laps.clear();
            }
            changed = true;
        }

        // Keep the running display ticking even with no button activity.
        if (running && (millis() - lastDraw) >= 100) {
            changed = true;
            lastDraw = millis();
        }

        if (changed) drawStopwatch(running, elapsedNow(), laps);
        delay(20);
    }
}
