#pragma once

#include <windows.h>
#include <string>
#include <vector>

// The power supply at one moment: AC or battery, and what the battery is doing.
struct PowerStatus {
    std::wstring summary;       // e.g. "AC power · battery 91 %, charging at 25.3 W, 16.8 V · 86.1 Wh now, 95.2 Wh full, 99.9 Wh design"
    bool onAc = false;
    bool drainingOnAc = false;  // on AC power but the battery discharges: the power supply (often a dock) delivers too little
};

PowerStatus CapturePowerStatus();

// Machine and power configuration that often explains dock and USB dropouts. Fields are empty when
// Windows does not report them.
struct SystemInfo {
    std::wstring manufacturer;        // from the BIOS / SMBIOS
    std::wstring model;
    std::wstring biosVersion;
    PowerStatus power;
    std::wstring powerPlan;
    std::wstring sleepModel;          // "Modern Standby (S0 low power idle)" or "S3 sleep"
    std::wstring usbSelectiveSuspendAc;  // "on" / "off", for the active power plan on AC and battery power
    std::wstring usbSelectiveSuspendDc;
    std::wstring pcieLinkPowerAc;     // "off", "moderate power savings", "maximum power savings"
    std::wstring pcieLinkPowerDc;
    bool usbSelectiveSuspendOn = false;  // on for AC or DC
};

SystemInfo CaptureSystemInfo();

// One entry from the Windows event logs that matter for docks, USB and displays.
struct WindowsEvent {
    ULONGLONG time = 0;          // UTC, FILETIME units
    std::wstring timeText;       // local time YYYY-MM-DD HH:MM:SS.mmm
    std::wstring channel;        // e.g. "System"
    std::wstring provider;       // e.g. "Microsoft-Windows-Kernel-PnP"
    unsigned id = 0;
    std::wstring level;          // Critical, Error, Warning, Information, Verbose
    ULONGLONG recordId = 0;      // unique within its channel
    std::wstring message;        // localized, on one line
};

struct WindowsEventList {
    std::vector<WindowsEvent> events;  // oldest first
    size_t omitted = 0;                // matching events left out because of maxEvents
    std::wstring error;                // why a log could not be read, if any
};

// Current time as UTC FILETIME units, the unit of WindowsEvent::time.
ULONGLONG UtcNow();
constexpr ULONGLONG UtcSecond = 10000000ULL;

// Relevant events in (fromUtc, toUtc]. Keeps the newest maxEvents when there are more.
// Sources: the Kernel-PnP device logs; from the System log only power, display, USB, Thunderbolt, PCIe
// (WHEA), driver install, Windows Update and crash related providers; warnings and errors of the
// DeviceSetupManager log (driver updates); and blue screen / kernel live dump reports from the Application log.
WindowsEventList ReadWindowsEvents(ULONGLONG fromUtc, ULONGLONG toUtc, size_t maxEvents);

// One kind of reliability event over a period, e.g. all "0x116 VIDEO_TDR_FAILURE" blue screen reports.
struct HistoryEntry {
    std::wstring category;  // "Blue screen", "Kernel live dump", "Unexpected shutdown", "Driver installed", ...
    std::wstring title;     // "0x116 VIDEO_TDR_FAILURE", or the event message
    size_t count = 0;       // reports; Windows may report the same crash more than once
    size_t days = 0;        // distinct days with such a report
    std::wstring first;     // local time of the oldest and newest report
    std::wstring last;
    std::wstring detail;    // from the newest report, e.g. the dump file
};

struct ReliabilityHistory {
    std::vector<HistoryEntry> entries;  // grouped by category, newest first within a category
    std::wstring error;
};

// Filters and names used by ReadWindowsEvents / ReadReliabilityHistory (pure functions, unit tested).

// System log providers worth reporting, matched as case-insensitive substrings of the provider name.
bool IsRelevantSystemProvider(const std::wstring& provider);
// Frequent event messages that are never about hardware: shadow copy volumes, Microsoft Defender
// signature updates, Microsoft Store app updates.
bool IsNoise(const std::wstring& message);
// "0x116 VIDEO_TDR_FAILURE (...)" from a hex stop code such as "116", "00000116" or "0x00000116".
std::wstring StopCodeTitle(std::wstring code);

// Crashes, kernel live dumps, unexpected shutdowns, driver installs and Windows Update results of the
// last `days` days, from the System and Application event logs (the same events Reliability Monitor shows).
ReliabilityHistory ReadReliabilityHistory(unsigned days);
