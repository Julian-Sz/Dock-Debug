#pragma once

#include "monitor.h"

#include <string>

// UI-independent helpers shared by the Win32 dashboard and the WinUI app.

std::wstring Lower(std::wstring value);
bool IsThunderbolt(const DeviceRecord& device);
bool IsUsbFamily(const DeviceRecord& device);
std::wstring DeviceLabel(const DeviceRecord& device);
std::wstring DisplayMode(const DisplayRecord& display);
std::wstring DeviceDetails(const DeviceRecord& device);

std::wstring DisplaySignature(const HardwareSnapshot& snapshot);
std::wstring UsbSignature(const HardwareSnapshot& snapshot);

std::wstring TrackingDirectory();
std::wstring FormatLive(const HardwareSnapshot& snapshot);

enum class Tracker { Monitor, UsbTree };
enum class TrackingEvent { Started, Change, Stopped };

// Writes one self-contained UTF-8 file per tracking event into TrackingDirectory(), named
// <local time>_<tracker>_<event>.log so the files sort chronologically, and refreshes the
// about-dock-debug-logs.md file that explains the folder. `snapshot` may be null (e.g. for Stopped).
// Returns the name of the file written, or an empty string on failure.
std::wstring WriteTrackingLog(Tracker tracker, TrackingEvent event, const HardwareSnapshot* snapshot);
