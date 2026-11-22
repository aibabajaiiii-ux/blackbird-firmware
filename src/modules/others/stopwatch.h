#pragma once
// Stopwatch -- not stock Bruce (stock only has a countdown Timer). Simple
// count-up with start/pause, lap recording, and reset. No hardware
// dependency (just millis()), so this works regardless of HAS_RTC.
void stopwatch_setup();
