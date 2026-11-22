#pragma once
// "Play Music": pick a Bluetooth speaker, then play .wav files off the
// microSD card to it. Not stock Bruce -- new module, reuses the A2DP engine
// in bt_audio_source.cpp (the same one FM Radio streams through) and the
// shared bt_device_picker.h screen.
void sd_music_player_setup();
