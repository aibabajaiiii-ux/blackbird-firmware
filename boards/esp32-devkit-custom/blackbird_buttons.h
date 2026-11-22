#pragma once
// Live (non-edge) button state, read straight off the PCF8574T's last poll.
// Bruce's global UpPress/DownPress/SelPress/etc. flags are one-shot "it was
// just pressed" edges (cleared the moment something calls check() on them)
// -- they can't tell you whether a button is STILL held down a moment
// later, which is what a press-and-hold gesture (e.g. FM Radio's hold-to-
// save-preset, hold-Select-to-mute) actually needs. These three functions
// expose that instead, reading interface.cpp's own cached PCF8574T state so
// the I2C bit-to-button mapping stays defined in exactly one place.
bool blackbirdBtnHeldUp();
bool blackbirdBtnHeldDown();
bool blackbirdBtnHeldSelect();

// True if ANY of the 6 buttons is currently held, regardless of which. Used
// by the Alarm background task (see alarm.cpp) to let a ringing alarm be
// silenced by any button press from ANY screen -- since the alarm can fire
// while you're nested inside some other app's own blocking loop, there's no
// safe way to steal its global UpPress/DownPress/etc. edge flags out from
// under it (same reasoning as the Menu-button burst), so this reads the
// live PCF8574T state directly instead, which doesn't consume anything the
// foreground screen is also reading.
bool blackbirdAnyButtonHeld();
