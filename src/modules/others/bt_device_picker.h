#pragma once
// Shared "scan, list, connect" screen used by both FM Radio and Play Music,
// so the two apps don't duplicate the same BT picker UI. 6-button control:
// Up/Down move the highlight, Select connects to the highlighted entry (or
// re-scans / skips, for those two special rows), Esc/Menu backs out (handled
// globally -- see interface.cpp's Menu-button burst).
//
// Returns true if connected (name available via btAudioConnectedName()),
// false if the user chose "Play without Bluetooth" or backed out.
bool btDevicePickerRun(const char *titleWhileScanning = "Scanning for speakers...");
