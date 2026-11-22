// Blackbird custom boot sequence: falling 0/1 "matrix rain" animation, a
// BLACKBIRD splash, then a 5-button lock screen, in place of Bruce's stock
// boot animation. Runs from _late_setup_gpio() (see interface.cpp), which
// main.cpp calls right after the input-handler task has started -- so the
// PCF8574T button task is already live and `check(UpPress)` etc. work here
// exactly like they do in any menu/module.
//
// Not stock Bruce: this entire file is specific to the Blackbird build.

#include "blackbird_boot.h"
#include "core/display.h"
#include <Arduino.h>
#include <globals.h>
#include <vector>

// ---------------------------------------------------------------------------
// Matrix rain: a handful of independent falling columns of random 0/1
// glyphs. Deliberately simple/cheap (no per-glyph trail fade, no PSRAM
// buffer) so it costs negligible RAM and runs fine on a plain ESP32.
static void blackbirdMatrixRain(uint32_t durationMs) {
    const int glyphH = 8 * FP;
    const int glyphW = 6 * FP;
    const int cols = tftWidth / glyphW;
    if (cols <= 0) return;

    // One falling glyph per column, independent random speed/restart so the
    // columns don't all scroll in lockstep.
    std::vector<int16_t> colY(cols);
    std::vector<uint8_t> colSpeed(cols);
    for (int i = 0; i < cols; i++) {
        colY[i] = random(-tftHeight, 0);
        colSpeed[i] = 1 + random(0, 3);
    }

    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(FP);

    uint32_t start = millis();
    while (millis() - start < durationMs) {
        if (check(AnyKeyPress)) break; // let an impatient user skip ahead

        for (int i = 0; i < cols; i++) {
            int x = i * glyphW;

            // Erase the glyph that's about to scroll off the bottom of its trail.
            // (tft_display only exposes drawString/drawCentreString publicly --
            // TFT_eSPI::drawChar is a private base-class method -- so single
            // glyphs are drawn as one-character strings instead.)
            tft.setTextColor(TFT_BLACK, TFT_BLACK);
            tft.drawString(random(0, 2) ? "1" : "0", x, colY[i] - glyphH, 1);

            // Draw a fresh glyph for this frame; brighter near the head of
            // the column to fake the "leading bright pixel" matrix look.
            bool isHead = true;
            tft.setTextColor(isHead ? TFT_GREEN : TFT_DARKGREEN, TFT_BLACK);
            tft.drawString(random(0, 2) ? "1" : "0", x, colY[i], 1);

            colY[i] += colSpeed[i];
            if (colY[i] > tftHeight + glyphH) {
                colY[i] = random(-tftHeight, -glyphH);
                colSpeed[i] = 1 + random(0, 3);
            }
        }
        delay(30);
    }
}

// ---------------------------------------------------------------------------
static void blackbirdSplash(uint32_t durationMs) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(FG);
    tft.drawCentreString("BLACKBIRD", tftWidth / 2, tftHeight / 2 - (FG * LH) / 2, SMOOTH_FONT);

    uint32_t start = millis();
    while (millis() - start < durationMs) {
        if (check(AnyKeyPress)) break;
        delay(20);
    }
}

// ---------------------------------------------------------------------------
// 5-step fixed-sequence lock screen: Up, Down, Left(=Prev), Right(=Next),
// Select, in that exact order. Any wrong button resets progress to zero.
// There is no real cryptographic intent here -- it's a "don't let it turn on
// in your bag and start pressing things" gate, not device security -- so it
// blocks forever until the correct sequence is entered (no lockout, no
// bypass needed for a forgotten code beyond remembering five arrow keys).
static void blackbirdLockScreen() {
    int step = 0;
    bool blinkOn = true;
    long lastBlink = millis();
    const int BLINK_MS = 400;

    // Title is drawn once per screen-reset (wrong entry) rather than every
    // frame, since it never changes otherwise -- keeps the blink animation
    // from needlessly repainting the whole display.
    auto drawTitle = [&]() {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.setTextSize(FM);
        tft.drawCentreString("ENTER PASSWORD", tftWidth / 2, 30, SMOOTH_FONT);
    };

    // Phone-style PIN row: entered slots show '*', the slot you're currently
    // on blinks an underscore cursor, slots you haven't reached yet sit as
    // static underscores.
    auto drawSlots = [&]() {
        int rowY = tftHeight / 2;
        int rowH = (int)FG * LH + 6;
        tft.fillRect(0, rowY - rowH / 2, tftWidth, rowH, TFT_BLACK);

        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.setTextSize(FG);

        String row = "";
        for (int i = 0; i < 5; i++) {
            if (i < step) row += "*";
            else if (i == step) row += (blinkOn ? "_" : " ");
            else row += "_";
            if (i < 4) row += " ";
        }
        tft.drawCentreString(row, tftWidth / 2, rowY, SMOOTH_FONT);
    };

    drawTitle();
    drawSlots();

    while (step < 5) {
        // Blink the current slot's cursor on a timer, independent of button
        // presses, so it animates even while nothing is being pressed.
        if (millis() - lastBlink > BLINK_MS) {
            blinkOn = !blinkOn;
            lastBlink = millis();
            drawSlots();
        }

        bool wrong = false;
        bool pressed = false;

        if (check(UpPress)) {
            pressed = true;
            wrong = (step != 0);
        } else if (check(DownPress)) {
            pressed = true;
            wrong = (step != 1);
        } else if (check(PrevPress)) { // Left
            pressed = true;
            wrong = (step != 2);
        } else if (check(NextPress)) { // Right
            pressed = true;
            wrong = (step != 3);
        } else if (check(SelPress)) {
            pressed = true;
            wrong = (step != 4);
        } else if (check(EscPress)) {
            pressed = true;
            wrong = true; // Esc is never a valid digit -- always resets
        }

        if (pressed) {
            if (wrong) {
                step = 0;
                tft.fillScreen(TFT_RED);
                delay(150); // brief red flash = "wrong", then back to step 0
                drawTitle();
            } else {
                step++;
            }
            blinkOn = true;
            lastBlink = millis();
            drawSlots();
        }

        delay(10);
    }

    // Brief confirmation before handing off to the normal menu.
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(FM);
    tft.drawCentreString("UNLOCKED", tftWidth / 2, tftHeight / 2, SMOOTH_FONT);
    delay(500);
}

void blackbird_boot_sequence() {
    blackbirdMatrixRain(2500);
    blackbirdSplash(1500);
    blackbirdLockScreen();
    tft.fillScreen(bruceConfig.bgColor);
}
