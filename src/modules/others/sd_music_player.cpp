// "Play Music" -- browse /music on the microSD card, connect to a Bluetooth
// speaker, and stream .wav files to it over the same A2DP engine FM Radio
// uses (src/modules/others/bt_audio_source.cpp). Not stock Bruce.
//
// Only uncompressed PCM .wav is supported, not MP3/AAC -- decoding those
// needs a codec library this environment can't fetch over the network, so
// it's out of scope here. Convert files to .wav ahead of time if needed.
//
// Files are expected in /music on the SD card (flat folder, no subfolders
// scanned). Controls:
//   Up/Down   -- volume (software gain, since .wav playback has no
//                hardware volume register the way the FM path does)
//   Left/Right-- previous / next track
//   Select    -- play/stop the highlighted track (browse screen) or
//                stop playback and return to the browse list (now-playing
//                screen)
//   Menu      -- exit (handled globally; disconnects Bluetooth)

#include "sd_music_player.h"

#ifdef HAS_BT_AUDIO

#include "bt_audio_source.h"
#include "bt_device_picker.h"
#include "core/display.h"
#include "core/sd_functions.h"
#include <Arduino.h>
#include <SD.h>
#include <globals.h>
#include <vector>

#define MUSIC_DIR "/music"

static std::vector<String> trackList;
static int selectedIdx = 0;
static uint8_t gainPct = 80;

static void scanTracks() {
    trackList.clear();
    File dir = SD.open(MUSIC_DIR);
    if (!dir || !dir.isDirectory()) return;
    File entry = dir.openNextFile();
    while (entry) {
        String name = String(entry.name());
        String lower = name;
        lower.toLowerCase();
        if (!entry.isDirectory() && lower.endsWith(".wav")) trackList.push_back(name);
        entry = dir.openNextFile();
    }
    dir.close();
}

static void drawBrowseList() {
    drawMainBorderWithTitle("Play Music");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(FP);

    if (trackList.empty()) {
        padprintln("");
        padprintln("No .wav files found in");
        padprintln(MUSIC_DIR);
        return;
    }

    padprintln("");
    for (size_t i = 0; i < trackList.size(); i++) {
        bool sel = ((int)i == selectedIdx);
        tft.setTextColor(sel ? bruceConfig.bgColor : bruceConfig.priColor,
                          sel ? bruceConfig.priColor : bruceConfig.bgColor);
        padprintln(trackList[i]);
    }
}

static void drawNowPlaying(const String &name, bool finished) {
    drawMainBorderWithTitle("Now Playing");
    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(FM);
    padprintln("");
    padprintln(name);
    tft.setTextSize(FP);
    padprintln("");
    padprintln(finished ? "[Finished]" : "[Playing]");
    padprintln(String("Vol: ") + String(gainPct) + "%");
    padprintln(String("BT: ") + String(btAudioConnectedName().c_str()));
    padprintln("");
    padprintln("L/R: prev/next  Up/Dn: vol");
    padprintln("Sel: stop");
}

static bool playTrack(int idx) {
    if (idx < 0 || idx >= (int)trackList.size()) return false;
    String path = String(MUSIC_DIR) + "/" + trackList[idx];
    return btAudioPlayWavFile(path.c_str());
}

void sd_music_player_setup() {
    if (!setupSdCard()) {
        tft.fillScreen(bruceConfig.bgColor);
        tft.setTextColor(TFT_RED, bruceConfig.bgColor);
        tft.setTextSize(FM);
        tft.drawCentreString("No SD card", tftWidth / 2, tftHeight / 2, SMOOTH_FONT);
        delay(1200);
        return;
    }

    scanTracks();
    selectedIdx = 0;
    drawBrowseList();

    // --- Browse screen ---
    bool playing = false;
    for (;;) {
        if (check(EscPress)) return;

        if (!trackList.empty()) {
            if (check(DownPress)) {
                selectedIdx = (selectedIdx + 1) % trackList.size();
                drawBrowseList();
            }
            if (check(UpPress)) {
                selectedIdx = (selectedIdx - 1 + trackList.size()) % trackList.size();
                drawBrowseList();
            }
            if (check(SelPress)) break; // -> connect + play
        }
        delay(10);
    }

    if (trackList.empty()) return;

    bool btConnected = btDevicePickerRun("Scanning for speakers...");
    if (!btConnected) {
        tft.fillScreen(bruceConfig.bgColor);
        tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
        tft.setTextSize(FP);
        tft.drawCentreString("No Bluetooth speaker --", tftWidth / 2, tftHeight / 2 - 10, SMOOTH_FONT);
        tft.drawCentreString("nothing to play audio out of.", tftWidth / 2, tftHeight / 2 + 10, SMOOTH_FONT);
        delay(1800);
        return;
    }

    btAudioSetGain(gainPct);
    playTrack(selectedIdx);
    drawNowPlaying(trackList[selectedIdx], false);

    // --- Now-playing screen ---
    for (;;) {
        bool changed = false;

        if (check(EscPress)) break;

        if (check(SelPress)) {
            btAudioStopFile();
            break;
        }
        if (check(NextPress)) { // Right -- next track
            selectedIdx = (selectedIdx + 1) % trackList.size();
            playTrack(selectedIdx);
            changed = true;
        }
        if (check(PrevPress)) { // Left -- previous track
            selectedIdx = (selectedIdx - 1 + trackList.size()) % trackList.size();
            playTrack(selectedIdx);
            changed = true;
        }
        if (check(UpPress)) {
            if (gainPct <= 90) gainPct += 10;
            btAudioSetGain(gainPct);
            changed = true;
        }
        if (check(DownPress)) {
            if (gainPct >= 10) gainPct -= 10;
            btAudioSetGain(gainPct);
            changed = true;
        }

        bool finished = btAudioFileFinished();
        if (finished && !btAudioFilePlaybackActive()) {
            // Auto-advance to the next track.
            selectedIdx = (selectedIdx + 1) % trackList.size();
            playTrack(selectedIdx);
            changed = true;
        }

        if (changed) drawNowPlaying(trackList[selectedIdx], finished);

        delay(20);
    }

    // Full teardown, not just disconnect -- this also switches WiFi back
    // on. See bt_audio_source.h's btAudioShutdown() for why the Bluetooth
    // stack isn't left resident between visits to this app: classic BT and
    // WiFi are never run at the same time on this build.
    btAudioShutdown();
}

#endif // HAS_BT_AUDIO
