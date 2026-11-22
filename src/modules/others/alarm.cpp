// Alarm clock -- not stock Bruce. See alarm.h for the summary.
//
// Rings using a background FreeRTOS task that polls the DS3231 RTC every
// 15 seconds, independent of whatever screen/app is currently in the
// foreground -- unlike Bruce's stock Timer (which only counts down while
// you're sitting in that one screen), this is a real "goes off even if
// you're not looking" alarm.
//
// ONE REAL LIMITATION, stated plainly: there's no on-screen pop-up when it
// rings, only the buzzer siren. Bruce's menu system is a single cooperative
// loop with no way to preempt whatever screen is currently blocking (the
// same reason the Menu button needed its burst-of-Esc trick rather than a
// real "jump to this screen now" primitive) -- there's no safe way to
// interrupt an arbitrary nested screen and throw up an alarm UI on top of
// it. So this rings audibly and is silenced by pressing ANY button from
// ANY screen (via blackbirdAnyButtonHeld(), which reads the PCF8574T
// directly rather than consuming whatever button flags the foreground
// screen is reading), but you won't see a visual banner unless you happen
// to already be looking at the home menu or the Alarm screen itself.

#include "alarm.h"

#ifdef HAS_RTC

#include "blackbird_buttons.h"
#include "core/display.h"
#include "modules/others/audio.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <globals.h>

#define ALARM_NS "bb_alarm"

static uint8_t alarmHour = 7, alarmMinute = 0;
static bool alarmEnabled = false;

static void alarmLoad() {
    Preferences p;
    p.begin(ALARM_NS, true);
    alarmHour = p.getUChar("hour", 7);
    alarmMinute = p.getUChar("min", 0);
    alarmEnabled = p.getBool("en", false);
    p.end();
}

static void alarmSave() {
    Preferences p;
    p.begin(ALARM_NS, false);
    p.putUChar("hour", alarmHour);
    p.putUChar("min", alarmMinute);
    p.putBool("en", alarmEnabled);
    p.end();
}

static void ringSiren() {
    // Same two-tone pattern stock Timer::playAlarmPattern() uses. Hard-
    // capped iteration count (not just "until dismissed") so a wiring fault
    // or missed button read can't leave it buzzing forever unattended.
    for (int i = 0; i < 200; i++) {
        if (blackbirdAnyButtonHeld()) return;
        _tone(2000, 400);
        if (blackbirdAnyButtonHeld()) return;
        delay(100);
        _tone(2600, 400);
        delay(300);
    }
}

static void alarmTask(void *arg) {
    int lastFiredMinuteOfDay = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000)); // 15s granularity is plenty for a minute-resolution alarm
        if (!alarmEnabled) continue;

        RTC_TimeTypeDef t;
        _rtc.GetTime(&t);
        int minuteOfDay = t.Hours * 60 + t.Minutes;
        int targetMinuteOfDay = alarmHour * 60 + alarmMinute;

        if (minuteOfDay == targetMinuteOfDay && lastFiredMinuteOfDay != minuteOfDay) {
            lastFiredMinuteOfDay = minuteOfDay;
            ringSiren();
        } else if (minuteOfDay != targetMinuteOfDay && lastFiredMinuteOfDay == targetMinuteOfDay) {
            // Re-arm once the clock has moved past the trigger minute, so
            // tomorrow's alarm at the same HH:MM fires again.
            lastFiredMinuteOfDay = -1;
        }
    }
}

void alarm_background_start() {
    alarmLoad();
    xTaskCreatePinnedToCore(alarmTask, "bb_alarm", 3072, nullptr, 1, nullptr, 0);
}

// ---------------------------------------------------------------------------
static void drawAlarmScreen(int field, uint8_t h, uint8_t m, bool en) {
    drawMainBorderWithTitle("Alarm");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);

    tft.setTextSize(FM);
    padprintln("");
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    padprintln(buf);

    tft.setTextSize(FP);
    padprintln("");
    padprintln(en ? "Status: ON" : "Status: OFF");
    padprintln("");
    padprintln(String(field == 0 ? "> " : "  ") + "Hour");
    padprintln(String(field == 1 ? "> " : "  ") + "Minute");
    padprintln(String(field == 2 ? "> " : "  ") + "Toggle ON/OFF");
    padprintln("");
    padprintln("L/R: field  Up/Dn: change");
    padprintln("Sel: save+exit  Esc: cancel");
}

void alarm_setup() {
    // Work on local copies so Esc can discard edits without touching the
    // saved/running alarm.
    uint8_t h = alarmHour, m = alarmMinute;
    bool en = alarmEnabled;
    int field = 0; // 0=hour, 1=minute, 2=enable toggle

    drawAlarmScreen(field, h, m, en);

    for (;;) {
        bool changed = false;

        if (check(EscPress)) return; // discard edits

        if (check(SelPress)) {
            alarmHour = h;
            alarmMinute = m;
            alarmEnabled = en;
            alarmSave();
            return;
        }

        if (check(NextPress)) { // Right -- next field
            field = (field + 1) % 3;
            changed = true;
        }
        if (check(PrevPress)) { // Left -- previous field
            field = (field + 2) % 3;
            changed = true;
        }

        // Capture both edges into locals BEFORE branching -- check() clears
        // the flag the moment it's read, so (check(UpPress) || check(Down
        // Press)) would short-circuit and then re-reading the raw UpPress
        // afterward would always see it already cleared.
        bool upPressed = check(UpPress);
        bool downPressed = check(DownPress);
        if (upPressed || downPressed) {
            switch (field) {
                case 0: h = upPressed ? (h + 1) % 24 : (h + 23) % 24; break;
                case 1: m = upPressed ? (m + 1) % 60 : (m + 59) % 60; break;
                case 2: en = !en; break;
            }
            changed = true;
        }

        if (changed) drawAlarmScreen(field, h, m, en);
        delay(10);
    }
}

#endif // HAS_RTC
