#pragma once

#include "monitor.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

// UI-independent logic: device classification, change signatures and the tracking log files.

bool IsThunderbolt(const DeviceRecord& device);
bool IsUsbFamily(const DeviceRecord& device);
std::wstring DeviceLabel(const DeviceRecord& device);
std::wstring DisplayLabel(const DisplayRecord& display);
std::wstring DisplayMode(const DisplayRecord& display);  // "3840 x 2160 @ 59.94 Hz"
// Estimated uncompressed data rate of the display signal (pixel clock x bits per pixel on the link), in
// Gbit/s; 0 when unknown. DSC compression, if active, lowers the real rate; Windows does not report it.
double DisplayBandwidthGbps(const DisplayRecord& display);
std::wstring DisplayBandwidthText(const DisplayRecord& display);  // "≈ 12.8 Gbit/s (533.25 MHz × 24 bit)"
std::wstring FormatGbps(double gbps);  // "12.8 Gbit/s", "480 Mbit/s"

std::wstring DisplaySignature(const HardwareSnapshot& snapshot);
std::wstring UsbSignature(const HardwareSnapshot& snapshot);

std::wstring TrackingDirectory();

enum class Tracker { Monitor, UsbTree };
enum class TrackingEvent { Started, Change, Stopped };

// A tracking session runs from switching on the first tracker until both are off again; all files written
// during it share its number.
struct TrackingSession {
    int number = 0;
    // Windows events up to this time (UTC FILETIME units) are already in a file of this session.
    unsigned long long eventsCoveredUntil = 0;
    std::set<std::pair<std::wstring, unsigned long long>> writtenEvents;  // (channel, record ID)
};

// One more than the highest session number among log file names like "s12_...log" (1 if there is none).
int NextSessionNumber(const std::vector<std::wstring>& fileNames);

// The WHAT CHANGED section body: displays, devices, USB port problems and power, `before` -> `after`.
std::wstring FormatChanges(const HardwareSnapshot& before, const HardwareSnapshot& after);

// Starts a new session numbered one higher than the highest s<N>_ prefix in TrackingDirectory(), and
// writes its s<N>_<time>_session_info.log (machine, power settings, Windows events of the last 30 minutes).
TrackingSession StartTrackingSession();

// The file a tracker wrote last and the snapshot in it: the baseline its next change is compared against.
struct TrackerBaseline {
    std::wstring fileName;
    HardwareSnapshot snapshot;
};

// Writes one self-contained UTF-8 file per tracking event into TrackingDirectory(), named
// s<session>_<local time>_<tracker>_<event>.log, and refreshes the about-dock-debug-logs.md file that
// explains the folder. Every file gets the relevant Windows events since the previous file of the session;
// a Change file also gets a summary of what differs from `previous`. `snapshot` is null for Stopped.
// Returns the name of the file written, or an empty string on failure.
std::wstring WriteTrackingLog(TrackingSession& session, Tracker tracker, TrackingEvent event,
                              const HardwareSnapshot* snapshot, const TrackerBaseline* previous);

// Writes s<session>_<local time>_marker.log: the user noticed a problem now. Contains the user's note (may
// be empty), the Windows events since the previous file of the session and the current snapshot.
// Returns the name of the file written, or an empty string on failure.
std::wstring WriteMarker(TrackingSession& session, const std::wstring& note, const HardwareSnapshot& snapshot);
