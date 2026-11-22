#pragma once
// Classic-Bluetooth A2DP SOURCE: Blackbird connects itself OUT to a Bluetooth
// speaker/headset (the sink) and streams the FM tuner's audio to it,
// entirely under the firmware's own menu -- no second "BT audio transmitter"
// box, no pairing done outside the device.
//
// *** THIS IS THE RISKIEST PIECE OF THE WHOLE BUILD. READ THIS BEFORE WIRING. ***
// A plain ESP32-WROOM-32 has one radio and ~520KB of RAM shared between
// WiFi, classic Bluetooth (BR/EDR, needed for A2DP), and BLE (used elsewhere
// in Bruce for scanning/spoofing). Running all three stacks in the same
// firmware image is the single most memory- and stability-sensitive thing in
// this project -- more so than anything else we've built. Expect this to
// need real debugging on your actual hardware: possible symptoms are boot
// crashes ("Guru Meditation" / brownout resets) if RAM is too tight, or BLE
// and classic-BT features becoming flaky when both are live at once. If that
// happens, the fallback is Option A from before (a standalone BT audio
// transmitter module wired to the RDA5807M's analog output) -- wave this
// file goodbye and tell me, and I'll wire that route instead.
//
// Not stock Bruce: Bruce has no existing A2DP/classic-BT code at all
// (confirmed by searching the whole repo). Everything here is new.

#include <stdint.h>
#include <string>
#include <vector>

struct BtAudioDevice {
    uint8_t bda[6];
    std::string name;
};

// Brings the classic-BT + BLE "dual mode" controller, Bluedroid, and A2DP
// source up from cold. Call this when entering FM Radio or Play Music, and
// call btAudioShutdown() when leaving -- this stack is NOT meant to stay
// resident for the whole session. See btAudioShutdown() for why: this is
// deliberately an on/off lifecycle, not "initialize once and leave running",
// specifically so classic Bluetooth is never live at the same time as WiFi.
// Returns false if the classic-BT controller fails to start (e.g. out of
// memory) -- in that case don't call anything else in this file.
bool btAudioInit();

// Full teardown: disconnects if still connected, stops streaming/playback,
// tears the Bluedroid + BT controller stacks all the way down, and turns
// WiFi back on if btAudioInit() had turned it off. Call this once when
// leaving FM Radio or Play Music (after your own btAudioDisconnect() /
// btAudioStopStreaming() / btAudioStopFile() calls, or instead of them --
// this calls those too). Safe to call even if btAudioInit() was never
// called or already shut down.
//
// WHY THIS EXISTS: classic Bluetooth (needed for A2DP) and WiFi share the
// same physical radio and a tight RAM budget on a plain ESP32. Rather than
// trying to make all three (WiFi + BLE + classic BT) coexist simultaneously
// for the whole time the device is powered on -- the riskiest, least-tested
// combination -- Blackbird only brings classic BT up for as long as you're
// actually inside FM Radio or Play Music, and takes WiFi down for that
// window. The rest of the time (every other screen), it's WiFi + BLE only,
// same as stock Bruce. You lose WiFi connectivity while actively using
// Bluetooth audio; you get it back the moment you back out.
void btAudioShutdown();

// Starts a ~10 second classic-BT device discovery. Clears any previous scan
// results first. Devices found trickle in -- poll btAudioScanComplete() and
// then read btAudioGetScanResults().
void btAudioStartScan();
bool btAudioScanComplete();
std::vector<BtAudioDevice> btAudioGetScanResults();

// Connect/disconnect as an A2DP source to a discovered device's address.
// btAudioConnect() is async -- poll btAudioIsConnected() /
// btAudioConnectFailed() afterward.
void btAudioConnect(const uint8_t *bda);
void btAudioDisconnect();
bool btAudioIsConnected();
bool btAudioConnectFailed();
std::string btAudioConnectedName();

// Starts/stops pulling samples from the ADC (GPIO36, see connections.md for
// the analog mixer/bias circuit feeding it) and encoding them out over A2DP.
// Call btAudioStartStreaming() once connected. This is the FM Radio path.
void btAudioStartStreaming();
void btAudioStopStreaming();

// SD-card music playback path (used by the "Play Music" app): streams a
// 16-bit PCM .wav file (mono or stereo, any sample rate -- it's resampled
// the same crude nearest-neighbour way as the ADC/FM path) out over the
// same A2DP connection. Only uncompressed PCM .wav is supported -- no MP3/
// AAC/etc. decoding (that needs a codec library this environment can't
// fetch). Switches the data source away from the ADC automatically; only
// one of the two (FM streaming or file playback) is ever active at once.
// Returns false immediately if the file can't be opened or isn't a PCM .wav
// this parser recognises.
bool btAudioPlayWavFile(const char *path);
void btAudioStopFile();
bool btAudioFilePlaybackActive();
// True once the currently-playing file has been fully consumed (so the UI
// can advance to the next track in a playlist, if it's driving one).
bool btAudioFileFinished();

// Software gain (0-100) applied to the captured audio before it's sent.
// The RDA5807M's own VOLUME register already controls the analog level this
// circuit taps, so under normal use leave this near 100 and control volume
// via the FM chip's register (what the UI's Up/Down buttons do) -- this is
// here mainly as a mute (0) and a bit of extra headroom trim.
void btAudioSetGain(uint8_t gainPct);
