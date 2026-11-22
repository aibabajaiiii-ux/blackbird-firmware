#include "blackbird_boot.h"
#include "blackbird_buttons.h"
#include "core/bus_HAL.h"
#if defined(HAS_RTC)
#include "modules/others/alarm.h"
#endif
#include "core/display.h"
#include "core/powerSave.h"
#include "core/utils.h"
#include "hal/bright/bright.h"
#include "hal/device.h"
#include <Arduino.h>
#include <Wire.h>
#include <globals.h>
#include <interface.h>

// ---------------------------------------------------------------------------
// PCF8574T I/O expander (6 tactile buttons) -- shares the I2C bus with the
// RDA5807M FM radio chip. The PCF8574T has no direction register: writing a
// 1 to a pin makes it a weak-pulled-up input, writing 0 drives it low. We
// write all-1s once at boot so every pin behaves as an input with the
// internal ~100uA pull-up, then just read the 8-bit input port each poll.
//
// ASSUMPTION: A0/A1/A2 on the PCF8574T are tied to GND -> I2C address 0x20.
// If you wired them differently, change PCF8574_ADDR to match
// (0x20 + (A2<<2 | A1<<1 | A0)).
//
// Your 6 physical buttons: Up, Down, Left, Right, Select, Menu.
// ASSUMPTION: wired P0..P5 = Left, Right, Up, Down, Sel, Menu, each button
// between the PCF8574T pin and GND. Change the BTN_BIT_* defines below if
// your wiring differs -- nothing else needs to change. (Left/Right reuse
// Bruce's existing Prev/Next press events -- that's what list navigation
// already listens for, so no core code needed changing for them.)
//
// The Menu button is a single press, not a hold: press it once from any
// screen and it takes you straight back to the home menu. Bruce's menu
// system has no real "jump to home" primitive to call into -- every screen
// is reached by nesting one menu inside another, and the only way out of
// any of them is each one's own Esc/back handling. So a single Menu press
// here automatically fires a short burst of synthetic EscPress events
// (MENU_BACK_BURST_COUNT of them, spaced MENU_BACK_BURST_INTERVAL_MS apart)
// rather than requiring you to press Esc once per level yourself. One click
// walks back out through however many levels you're nested in -- 8 presses
// is comfortably more than this firmware ever nests -- and firing a few
// extra once you're already at the home menu is harmless (Esc there just
// redraws the home menu, nothing else).
#define PCF8574_ADDR 0x20
#define BTN_BIT_PREV 0 // Left
#define BTN_BIT_NEXT 1 // Right
#define BTN_BIT_UP   2
#define BTN_BIT_DOWN 3
#define BTN_BIT_SEL  4
#define BTN_BIT_MENU 5

#define MENU_BACK_BURST_COUNT    8
#define MENU_BACK_BURST_INTERVAL_MS 150

static uint8_t pcf8574_read() {
    Wire.requestFrom((uint8_t)PCF8574_ADDR, (uint8_t)1);
    if (Wire.available()) return Wire.read();
    return 0xFF; // bus error -- report "nothing pressed" rather than phantom presses
}

// Cached raw (active-LOW) PCF8574T state from the most recent poll, exposed
// read-only via blackbird_buttons.h for anything that needs to know whether
// a button is STILL held right now rather than just "was it pressed" --
// see that header for why this exists separately from the UpPress/DownPress/
// SelPress edge flags.
static volatile uint8_t currentRawState = 0xFF;

bool blackbirdBtnHeldUp() { return !(currentRawState & (1 << BTN_BIT_UP)); }
bool blackbirdBtnHeldDown() { return !(currentRawState & (1 << BTN_BIT_DOWN)); }
bool blackbirdBtnHeldSelect() { return !(currentRawState & (1 << BTN_BIT_SEL)); }
bool blackbirdAnyButtonHeld() { return currentRawState != 0xFF; }

/***************************************************************************************
** Function name: _setup_gpio()
** Location: main.cpp
** Description:   initial setup for the device
***************************************************************************************/
void _setup_gpio() {
    bruceConfigPins.rotation = 0;

    // Keep shared-bus chip-selects idle (HIGH) before each driver's own init runs.
    pinMode(13, OUTPUT);
    digitalWrite(13, HIGH); // microSD CS
    pinMode(32, OUTPUT);
    digitalWrite(32, HIGH); // PN532 SS (SPI mode)
    // PN532's RSTPDN pin isn't driven by Bruce's PN532 driver (it only talks
    // SPI + resets the chip in software over the bus) -- on most PN532
    // breakouts RSTPDN already has its own onboard pull-up and needs nothing
    // wired to it at all. GPIO14 is left spare here (previously held the
    // RC522's RST pin high; not needed for PN532). If your specific PN532
    // board's RSTPDN floats low/unreliable, wire it to GPIO14 and uncomment
    // the two lines below.
    // pinMode(14, OUTPUT);
    // digitalWrite(14, HIGH);

    // TFT/SD/PN532 share the VSPI-style bus wired above. The PN532-SPI
    // RFID2 driver (src/modules/rfid/RFID2.cpp) reads its pins from
    // PN532_bus regardless of which RFID chip is selected in rfidModule.
    //
    // IMPORTANT: most PN532 breakout boards have a 3-way switch or solder
    // jumpers selecting HSU (UART) / I2C / SPI mode -- set it to SPI before
    // wiring this in, or the module won't respond on this bus at all.
    bruceConfigPins.PN532_bus = {
        (gpio_num_t)18, (gpio_num_t)39, (gpio_num_t)23, (gpio_num_t)32
    }; // sck,miso,mosi,cs
    bruceConfigPins.rfidModule = PN532_SPI_MODULE;
    bruceConfigPins.SDCARD_bus = {
        (gpio_num_t)18, (gpio_num_t)39, (gpio_num_t)23, (gpio_num_t)13
    }; // sck,miso,mosi,cs

    // IR
    bruceConfigPins.irTx = 16; // through 2N2222 transistor
    bruceConfigPins.irRx = 35; // TSOP1838 OUT

    // Buzzer
    bruceConfigPins.buzzer = 19;

    // No microphone on this build -- GPIO27/26/36 (originally wired for a
    // PCM1808) are free for future use.

    // I2C bus shared by the PCF8574T (buttons), RDA5807M (FM radio), and now
    // the DS3231 RTC. This is a "fixed system driver" bus per
    // src/core/bus_HAL.h -- always on, never torn down -- so it goes
    // through sys_i2c rather than the user-configurable i2c_bus.
    bruceConfigPins.sys_i2c = {(gpio_num_t)33, (gpio_num_t)15}; // sda, scl
    Wire.begin((int)bruceConfigPins.sys_i2c.sda, (int)bruceConfigPins.sys_i2c.scl);
    setSysI2CBus(&Wire);

    // PCF8574T: write all-1s so every pin is a pulled-up input.
    Wire.beginTransmission(PCF8574_ADDR);
    Wire.write(0xFF);
    Wire.endTransmission();

#if defined(HAS_RTC_DS3231)
    // ds3231_RTC::begin() is a no-op by design (bus is already up above);
    // point it at the shared bus before init_clock() calls _rtc.begin() /
    // _rtc.GetTime() / _rtc.GetDate() in main.cpp.
    _rtc.setWire(getSysI2CBus());
#endif
}

/***************************************************************************************
** Function name: _post_setup_gpio()
** Location: main.cpp
** Description:   second stage gpio setup -- runs AFTER tft.init()
***************************************************************************************/
void _post_setup_gpio() {
    hal_bright_attach(TFT_BL);
    hal_bright_set(TFT_BL, 100);
}

/*********************************************************************
** Function: setBrightness
** location: settings.cpp
** set brightness value
**********************************************************************/
void _setBrightness(uint8_t brightval) { hal_bright_set(TFT_BL, brightval); }

/*********************************************************************
** Function: InputHandler
** Polls the PCF8574T over I2C (buttons are not on raw ESP32 GPIOs, so the
** shared button HAL in src/hal/inputs/buttons.cpp can't be used here) and
** sets PrevPress / NextPress / UpPress / DownPress / SelPress / EscPress on
** the falling edge of each bit (active LOW -- button pulls the pin to GND).
**********************************************************************/
void InputHandler(void) {
    static uint8_t lastState = 0xFF;
    static long lastPoll = 0;

    // Menu-button "walk back to home" burst, running independently of the
    // edge-detect logic below so it keeps firing across polls where nothing
    // else changed.
    static int menuBackRemaining = 0;
    static long menuBackLastFire = 0;

    checkPowerSaveTime();

    // The input task already calls this roughly every 10ms; an extra guard
    // here just protects against back-to-back calls hammering the I2C bus.
    if (millis() - lastPoll < 8) return;
    lastPoll = millis();

    uint8_t state = pcf8574_read();
    uint8_t pressedEdges = (~state) & (lastState); // bits that just went LOW
    lastState = state;
    currentRawState = state;

    if (pressedEdges & (1 << BTN_BIT_MENU)) {
        // Single press -- kick off the back-to-home burst (see header
        // comment above for why this is a burst rather than one EscPress).
        menuBackRemaining = MENU_BACK_BURST_COUNT;
        menuBackLastFire = millis();
        if (!wakeUpScreen()) AnyKeyPress = true;
    }

    if (menuBackRemaining > 0 && (millis() - menuBackLastFire) >= MENU_BACK_BURST_INTERVAL_MS) {
        EscPress = true;
        menuBackRemaining--;
        menuBackLastFire = millis();
    }

    // Everything else is ordinary edge-triggered (press-on-press-down).
    uint8_t otherEdges = pressedEdges & ~(1 << BTN_BIT_MENU);
    if (otherEdges == 0) return;

    if (!wakeUpScreen()) AnyKeyPress = true;

    if (otherEdges & (1 << BTN_BIT_PREV)) PrevPress = true;
    if (otherEdges & (1 << BTN_BIT_NEXT)) NextPress = true;
    if (otherEdges & (1 << BTN_BIT_UP)) UpPress = true;
    if (otherEdges & (1 << BTN_BIT_DOWN)) DownPress = true;
    if (otherEdges & (1 << BTN_BIT_SEL)) SelPress = true;
}

/***************************************************************************************
** Function name: _late_setup_gpio()
** Location: main.cpp (weak no-op there; this board overrides it)
** Description:   Runs once at boot, right after the button InputHandler task
**                has started (so the lock screen can read real button
**                presses) and right before main.cpp would otherwise run
**                Bruce's stock boot animation. We replace that stock
**                animation outright: run our own matrix-rain + splash +
**                lock screen, then set instantBoot so main.cpp skips its
**                own boot_screen_anim()/startup_sound() right after this
**                returns.
***************************************************************************************/
void _late_setup_gpio() {
    blackbird_boot_sequence();
    bruceConfig.instantBoot = 1;

#if defined(HAS_RTC)
    // Starts the Alarm's background watch task (see
    // src/modules/others/alarm.cpp) -- independent of the menu system, so
    // the alarm still rings even if you're not sitting in the Alarm screen.
    alarm_background_start();
#endif
}

/*********************************************************************
** Function: powerOff
** location: mykeyboard.cpp
** Turns off the device (or tries to)
**********************************************************************/
void powerOff() { esp_deep_sleep_start(); }

/*********************************************************************
** Function: goToDeepSleep
** location: mykeyboard.cpp
** Puts the device into DeepSleep
** NOTE: no DEEPSLEEP_WAKEUP_PIN is set because all buttons sit behind the
** PCF8574T, not a raw RTC-capable GPIO, so there is no wake source wired up.
** The device will need a reset/power-cycle to wake. The "Deep Sleep" menu
** entry will show as unavailable until a real GPIO button is added.
**********************************************************************/
void goToDeepSleep() { esp_deep_sleep_start(); }

/*********************************************************************
** Function: checkReboot
** location: mykeyboard.cpp
** Btn logic to turn off the device
**********************************************************************/
void checkReboot() {}
