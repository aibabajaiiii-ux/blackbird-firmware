#pragma once
// Alarm clock -- not stock Bruce. Rings at a saved HH:MM every day it's
// enabled, using the DS3231 RTC we wired in (see connections.md). Unlike
// Bruce's stock Timer (which only counts down while you're sitting in that
// screen), this runs as an independent background task so it actually goes
// off like a real alarm clock even while you're using some other app --
// see alarm.cpp for how, and its one real limitation (no on-screen pop-up,
// audio only).
#include <stdint.h>

// Call once at boot (see interface.cpp's _late_setup_gpio()) to start the
// background watch task. Safe to call only once.
void alarm_background_start();

// The menu screen: view/set the alarm time, toggle it on/off.
void alarm_setup();
