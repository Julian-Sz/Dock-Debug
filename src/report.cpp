#include "report.h"
#include "system.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
    });
    return value;
}

std::wstring OrNone(const std::wstring& value) {
    return value.empty() ? L"none" : value;
}

} // namespace

bool IsThunderbolt(const DeviceRecord& device) {
    const std::wstring searchable = Lower(device.friendlyName + L" " + device.instanceId + L" " + device.hardwareId);
    return searchable.find(L"thunderbolt") != std::wstring::npos || searchable.find(L"usb4") != std::wstring::npos;
}

bool IsUsbFamily(const DeviceRecord& device) {
    const std::wstring className = Lower(device.className);
    const std::wstring instance = Lower(device.instanceId);
    return className == L"usb" || instance.find(L"usb\\") == 0 || instance.find(L"usb4\\") == 0 ||
           IsThunderbolt(device);
}

// "CORSAIR K55 RGB PRO Gaming Keyboard (USB Composite Device)" when the device reports its own name,
// "HID Keyboard Device · CORSAIR K55 RGB PRO Gaming Keyboard" for a part of it, otherwise the Windows name.
std::wstring DeviceLabel(const DeviceRecord& device) {
    std::wstring windowsName = device.friendlyName;
    if (windowsName.empty()) {
        windowsName = device.className.empty() ? L"(unnamed device)" : L"(unnamed " + device.className + L" device)";
    }
    if (!device.reportedName.empty() && Lower(device.reportedName) != Lower(device.friendlyName)) {
        return device.reportedName + L" (" + windowsName + L")";
    }
    if (!device.nameHint.empty() && Lower(device.nameHint) != Lower(device.friendlyName)) {
        return windowsName + L" · " + device.nameHint;
    }
    return windowsName;
}

std::wstring FormatGbps(double gbps) {
    wchar_t text[32]{};
    if (gbps >= 1.0) {
        swprintf_s(text, gbps >= 10.0 ? L"%.0f Gbit/s" : L"%.1f Gbit/s", gbps);
    } else {
        swprintf_s(text, gbps >= 0.01 ? L"%.0f Mbit/s" : L"%.1f Mbit/s", gbps * 1000.0);
    }
    return text;
}

double DisplayBandwidthGbps(const DisplayRecord& display) {
    const UINT32 bitsPerChannel = display.bitsPerColorChannel ? display.bitsPerColorChannel : 8;
    return display.active ? static_cast<double>(display.pixelRate) * bitsPerChannel * 3 / 1e9 : 0.0;
}

std::wstring DisplayBandwidthText(const DisplayRecord& display) {
    const double gbps = DisplayBandwidthGbps(display);
    if (gbps <= 0) {
        return {};
    }
    wchar_t text[96]{};
    swprintf_s(text, L"≈ %.1f Gbit/s (%.2f MHz × %u bit)", gbps, display.pixelRate / 1e6,
               (display.bitsPerColorChannel ? display.bitsPerColorChannel : 8) * 3);
    return text;
}

std::wstring DisplayLabel(const DisplayRecord& display) {
    if (!display.monitorName.empty()) {
        return display.monitorName;
    }
    if (display.connection.rfind(L"Internal", 0) == 0) {
        return L"Built-in display";
    }
    return display.edidId.empty() ? L"Unknown display" : L"Unknown display (" + display.edidId + L")";
}

namespace {

// "59.94", from the exact fraction.
std::wstring RefreshRate(const DisplayRecord& display) {
    if (display.refreshDenominator == 0) {
        return L"0";
    }
    wchar_t text[32]{};
    swprintf_s(text, L"%.3f", static_cast<double>(display.refreshNumerator) / display.refreshDenominator);
    std::wstring value = text;
    value.erase(value.find_last_not_of(L'0') + 1);
    if (!value.empty() && value.back() == L'.') {
        value.pop_back();
    }
    return value;
}

} // namespace

std::wstring DisplayMode(const DisplayRecord& display) {
    if (!display.active) {
        return L"not active";
    }
    return std::to_wstring(display.pixelWidth) + L" x " + std::to_wstring(display.pixelHeight) + L" @ " +
           RefreshRate(display) + L" Hz";
}

std::wstring DisplaySignature(const HardwareSnapshot& snapshot) {
    std::wstringstream output;
    for (const DisplayRecord& display : snapshot.displays) {
        output << display.sourceName << L'|' << display.monitorInstanceId << L'|' << display.monitorName << L'|'
               << display.connection << L'|' << display.pixelWidth << L'x' << display.pixelHeight << L'@'
               << display.refreshNumerator << L'/' << display.refreshDenominator << L'|' << display.bitsPerPixel
               << L'|' << display.rotation << L'|' << display.scalePercent << L'|' << display.active << L'|'
               << display.primary << L'|' << display.advancedColorEnabled << L'\n';
    }
    return output.str();
}

std::wstring UsbSignature(const HardwareSnapshot& snapshot) {
    std::wstringstream output;
    for (const DeviceRecord& device : snapshot.devices) {
        output << device.className << L'|' << device.friendlyName << L'|' << device.instanceId << L'|'
               << device.hardwareId << L'|' << device.parentInstanceId << L'|' << device.status << L'|'
               << device.driverVersion << L'|' << device.lastArrival << L'|' << device.usbSpeed << L'|'
               << device.usbPortStatus << L'\n';
    }
    for (const UsbPortProblem& problem : snapshot.usbPortProblems) {
        output << L"port problem|" << problem.hubInstanceId << L'|' << problem.port << L'|' << problem.status << L'\n';
    }
    return output.str();
}

// Documents\Dock-Debug\tracking. Not %LOCALAPPDATA%: in the MSIX package, writes there are redirected to a
// per-package folder, so Explorer ("Open folder") would show the user an empty directory.
std::wstring TrackingDirectory() {
    std::wstring directory = L"C:\\ProgramData";
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documents))) {
        directory = documents;
    }
    CoTaskMemFree(documents);
    directory += L"\\Dock-Debug";
    CreateDirectoryW(directory.c_str(), nullptr);
    directory += L"\\tracking";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

namespace {

// The snapshot part of a log file (LIVE VIEW ... THUNDERBOLT / USB4 DETAIL).
std::wstring FormatLive(const HardwareSnapshot& snapshot) {
    std::wstringstream output;
    output << L"LIVE VIEW  /  " << snapshot.displays.size() << L" displays  /  "
           << snapshot.devices.size() << L" tracked devices\r\n\r\n";
    output << L"POWER: " << OrNone(snapshot.power.summary) << L"\r\n\r\n";
    output << L"MONITORS\r\n";
    if (snapshot.displays.empty()) {
        output << L"  Windows did not report any displays.\r\n";
    }
    for (const DisplayRecord& display : snapshot.displays) {
        output << L"  " << (display.active ? L"[ACTIVE]  " : L"[INACTIVE] ") << DisplayLabel(display)
               << (display.primary ? L"  [PRIMARY]" : L"") << L"\r\n";
        if (display.active) {
            output << L"      source: " << display.sourceName << L"  |  mode: " << display.pixelWidth << L" x "
                   << display.pixelHeight << L" @ " << RefreshRate(display) << L" Hz (" << display.refreshNumerator
                   << L"/" << display.refreshDenominator << L"), " << display.bitsPerPixel << L" bpp, rotation "
                   << display.rotation << L", scaling "
                   << (display.scalePercent ? std::to_wstring(display.scalePercent) + L" %" : std::wstring(L"unknown"))
                   << L"\r\n";
            if (display.pixelRate != 0) {
                output << L"      signal: pixel clock " << display.pixelRate / 1000 << L" kHz  |  estimated data rate "
                       << DisplayBandwidthText(display) << L", uncompressed\r\n";
            }
        } else {
            output << L"      connected, but not part of the desktop (no source, no mode)\r\n";
        }
        output << L"      connection: " << OrNone(display.connection) << L"  |  advanced color (HDR): "
               << (display.advancedColorSupported ? L"supported, " : L"not supported, ")
               << (display.advancedColorEnabled ? L"on" : L"off");
        if (display.bitsPerColorChannel != 0) {
            output << L", " << display.bitsPerColorChannel << L" bits per channel";
        }
        output << L"\r\n"
               << L"      monitor: " << OrNone(display.monitorInstanceId) << L"\r\n"
               << L"      EDID: " << OrNone(display.edidId) << L"  |  serial: " << OrNone(display.edidSerial)
               << L"  |  manufactured: " << OrNone(display.edidManufactured) << L"\r\n"
               << L"      adapter: " << OrNone(display.adapterName) << L"\r\n";
        if (!display.edid.empty()) {
            output << L"      EDID data (" << display.edid.size() << L" bytes, hex):\r\n";
            for (size_t offset = 0; offset < display.edid.size(); offset += 32) {
                output << L"        ";
                for (size_t i = offset; i < std::min(offset + 32, display.edid.size()); ++i) {
                    wchar_t hex[4]{};
                    swprintf_s(hex, L"%02X", display.edid[i]);
                    output << hex;
                }
                output << L"\r\n";
            }
        }
    }

    // Print a real tree: every device directly followed by its children (siblings keep Windows
    // enumeration order). Devices whose parent is not in the list start at the left margin.
    std::map<std::wstring, const DeviceRecord*> devicesById;
    for (const DeviceRecord& device : snapshot.devices) {
        devicesById[device.instanceId] = &device;
    }
    std::map<std::wstring, std::vector<const DeviceRecord*>> children;
    std::vector<const DeviceRecord*> roots;
    for (const DeviceRecord& device : snapshot.devices) {
        const auto parent = devicesById.find(device.parentInstanceId);
        if (parent != devicesById.end() && parent->second != &device) {
            children[device.parentInstanceId].push_back(&device);
        } else {
            roots.push_back(&device);
        }
    }

    std::set<const DeviceRecord*> printed;
    std::function<void(const DeviceRecord&, size_t)> print = [&](const DeviceRecord& device, size_t depth) {
        if (!printed.insert(&device).second) {
            return;
        }
        std::wstring tag = L"[" + (device.className.empty() ? std::wstring(L"Other") : device.className) + L"] ";
        if (IsThunderbolt(device)) {
            tag = L"[TB/USB4] ";
        } else if (IsUsbFamily(device)) {
            tag = L"[USB] ";
        }
        const std::wstring indent(depth * 2, L' ');
        output << indent << tag << DeviceLabel(device) << L"\r\n"
               << indent << L"  class: " << device.className << L"  status: " << device.status << L"\r\n"
               << indent << L"  instance: " << device.instanceId << L"  parent: " << device.parentInstanceId << L"\r\n";
        if (!device.hardwareId.empty() || !device.manufacturer.empty() || !device.location.empty()) {
            output << indent << L"  hardware: " << device.hardwareId << L"  |  maker: " << device.manufacturer
                   << L"  |  location: " << device.location
                   << (device.containerId.empty() ? L"" : L"  |  container: " + device.containerId) << L"\r\n";
        }
        if (!device.driverVersion.empty() || !device.firmwareRevision.empty() || !device.firmwareVersion.empty()) {
            output << indent << L"  driver: " << OrNone(device.driverProvider) << L" " << OrNone(device.driverVersion)
                   << L" (" << OrNone(device.driverDate) << L")  |  firmware revision: "
                   << OrNone(device.firmwareRevision)
                   << (device.firmwareVersion.empty() ? L"" : L"  |  firmware version: " + device.firmwareVersion) << L"\r\n";
        }
        if (!device.lastArrival.empty() || !device.lastRemoval.empty()) {
            output << indent << L"  last arrival: " << OrNone(device.lastArrival) << L"  |  last removal: "
                   << OrNone(device.lastRemoval) << L"\r\n";
        }
        if (!device.usbPort.empty()) {
            output << indent << L"  usb: " << device.usbPort << L" of parent  |  " << OrNone(device.usbSpeed) << L"  |  "
                   << OrNone(device.usbCapability) << L"  |  port status: " << OrNone(device.usbPortStatus) << L"\r\n";
        }
        if (!device.powerState.empty() || !device.powerSaving.empty()) {
            output << indent << L"  power: " << OrNone(device.powerState) << L"  |  turn off to save power: "
                   << OrNone(device.powerSaving) << L"\r\n";
        }
        const auto found = children.find(device.instanceId);
        if (found != children.end()) {
            for (const DeviceRecord* child : found->second) {
                print(*child, depth + 1);
            }
        }
    };

    output << L"\r\nUSB DEVICE TREE\r\n";
    for (const DeviceRecord* root : roots) {
        print(*root, 0);
    }
    // Only reachable through a parent cycle, which Windows should never report; never drop a device.
    for (const DeviceRecord& device : snapshot.devices) {
        print(device, 0);
    }

    output << L"\r\nUSB PORT PROBLEMS\r\n";
    if (snapshot.usbPortProblems.empty()) {
        output << L"  none\r\n";
    }
    for (const UsbPortProblem& problem : snapshot.usbPortProblems) {
        output << L"  hub " << problem.hubInstanceId << L", port " << problem.port << L": " << problem.status << L"\r\n";
    }

    output << L"\r\nTHUNDERBOLT / USB4 DETAIL\r\n";
    bool foundThunderbolt = false;
    for (const DeviceRecord& device : snapshot.devices) {
        if (IsThunderbolt(device)) {
            foundThunderbolt = true;
            output << L"  " << DeviceLabel(device) << L"\r\n"
                   << L"    instance: " << device.instanceId << L"\r\n"
                   << L"    parent: " << device.parentInstanceId << L"\r\n"
                   << L"    service: " << device.service << L"  |  status: " << device.status << L"\r\n"
                   << L"    hardware: " << device.hardwareId << L"\r\n";
        }
    }
    if (!foundThunderbolt) {
        output << L"  No Thunderbolt or USB4 devices matched in the current snapshot.\r\n";
    }
    return output.str();
}

constexpr wchar_t AboutFileName[] = L"about-dock-debug-logs.md";
constexpr size_t MaxEventsPerFile = 300;
constexpr unsigned long long SessionInfoEventWindow = 30 * 60 * UtcSecond;
constexpr unsigned HistoryDays = 30;
// Event log entries can be written slightly after the moment they describe, so every query reaches back
// this far; entries already written in this session are skipped.
constexpr unsigned long long EventOverlap = 60 * UtcSecond;

constexpr wchar_t AboutIntro[] = LR"md(# About these logs

This folder was written by **Dock-Debug**, a Windows diagnostic tool for docks, Thunderbolt / USB4 / USB
devices and external displays. Its tracking feature saves a snapshot of the connected hardware every time
something changes, together with the relevant Windows event log entries. Users typically zip the whole
folder when a problem happens (monitors blanking or dropping out, dock devices disconnecting or failing to
start, wrong resolution or refresh rate) and hand it over for analysis.

Dock-Debug runs on arbitrary Windows 10 / 11 PCs with arbitrary docks, GPUs and monitors. Do not assume any
specific hardware; everything below describes the format, and the logs themselves describe the machine.

## Machine that wrote this folder

)md";

constexpr wchar_t AboutBody[] = LR"md(
Every log file repeats its capture time and the Windows version in its header.

## Files

- `about-dock-debug-logs.md`: this file. It is rewritten every time a log file is written.
- `s<session>_<date>_<time>_session_info.log`: written once when a session starts. Computer model, BIOS,
  power source and power settings, the reliability history of the last 30 days (blue screens, driver
  hangs, unexpected shutdowns, driver installs, Windows Update results) and the relevant Windows events of
  the 30 minutes before tracking started (see "Session info file").
- `s<session>_<date>_<time>_marker.log`: the user pressed "Mark problem" at this moment, for example when a
  monitor went black. Contains the user's note (`NOTE:` line, may be "(no note)"), the Windows events since
  the previous file and a full snapshot. **Markers are the most direct pointer to what the user
  experienced; start the analysis at them.** The problem itself may have started a little earlier.
- `s<session>_<date>_<time>_<tracker>_<event>.log`: one file per tracking event, for example
  `s3_2026-01-31_14-05-09-250_usb-tree_change.log`.
  - `s<session>` is the tracking session number (see below): `s1`, `s2`, ..., `s10`, without leading
    zeros. Sort files by the session number numerically, not as text (`s10` comes after `s9`).
  - `<date>_<time>` is the local capture time as `YYYY-MM-DD_HH-MM-SS-mmm`, so within one session sorting
    by file name sorts chronologically. (If two events share a millisecond, the second gets a `-2` suffix.)
  - `<tracker>` is `monitor` (display configuration) or `usb-tree` (USB / USB4 / Thunderbolt devices).
    The user switches the two trackers on and off independently.
  - `<event>` is one of:
    - `started`: the tracker was switched on. Contains the baseline snapshot that later changes are
      compared against.
    - `change`: the part of the hardware state this tracker watches differs from the previous file of the
      same tracker. Starts with a WHAT CHANGED summary, followed by the complete new snapshot.
    - `stopped`: the tracker was switched off. Contains no snapshot.

### Sessions

A session is one recording by the user: it begins when a tracker is switched on while no tracker is
running, and ends when both trackers are off again. Files of both trackers written in between share the
session number. Each new session gets the next number after the highest one already in the folder, so
numbers grow over time; a gap means the user deleted files. Users often record one session per
occurrence of a problem, so treat different sessions as separate attempts, not as one continuous timeline.

Within a session, each tracker has `started`, then zero or more `change`, then `stopped`. A tracker can be
switched off and on again while the other keeps running, which gives it another `started` ... `stopped`
run in the same session. A run without a `stopped` file means the app was closed or crashed, or was still
running when the folder was zipped; Dock-Debug does not write a file when it exits. Nothing is recorded
between sessions.

## How snapshots are captured

A snapshot is taken:

- immediately when Windows sends a device-change notification (a USB device or monitor interface arriving
  or being removed, or the generic "device tree changed" broadcast) or a display-change notification
  (`WM_DISPLAYCHANGE`: resolution, refresh rate or the set of active monitors changed); and
- every 10 seconds by polling, which catches changes without a notification (for example a device's
  status changing).

A snapshot takes roughly 0.1 to 0.2 seconds. Notifications that arrive while one is being taken are merged
into one follow-up snapshot. As a result:

- The capture time is usually well under a second after the event, but can be up to about 10 seconds later
  for changes without a notification. The WINDOWS EVENTS section often has the exact time.
- A device that drops out and comes back before the next snapshot does not disappear from any snapshot.
  Its `last arrival` time still changes, which the usb-tree tracker detects: WHAT CHANGED then reports it
  as "re-arrived", and the event log usually has the matching surprise removal (Kernel-PnP 1010).
- Several quick changes can end up in a single `change` file.
- A `change` file is only written when the watched state actually differs; an unchanged system produces
  no files.

What each tracker compares:

- `monitor`: for every display entry, the source name, monitor instance ID, monitor name, connection,
  resolution, exact refresh rate, color depth, rotation, scaling, active and primary flags and whether
  advanced color (HDR) is on.
- `usb-tree`: for every entry in the device list (which also contains monitor and display-adapter
  devices, see below), its class, name, instance ID, first hardware ID, parent instance ID, status,
  driver version, last arrival time, USB speed and USB port status; and the list of USB port problems.
  A monitor or GPU driver restart can therefore also cause a `usb-tree` change. Power states are not
  compared (idle USB devices go in and out of low power all the time).

Both trackers write the same full snapshot (displays and devices), whichever part changed.

Data sources (Win32 APIs, all readable without administrator rights):

- Displays: `QueryDisplayConfig` / `DisplayConfigGetDeviceInfo` for active monitors and for monitors that
  are connected but not part of the desktop; the EDID stored by Windows for each monitor;
  `GetDpiForMonitor` for the scaling.
- Devices: SetupAPI enumeration of all currently *present* devices (`SetupDiGetClassDevs` with
  `DIGCF_PRESENT | DIGCF_ALLCLASSES`), properties from `SetupDiGetDeviceRegistryProperty` and
  `SetupDiGetDeviceProperty` (driver, firmware version, arrival and removal times, power state, the
  product name a USB device reports, container ID), the parent from
  `CM_Get_Parent`, the status from `CM_Get_DevNode_Status`, power-saving settings from the device's
  `Device Parameters` registry key.
- USB connections: every port of every USB hub via the hub IOCTLs (`IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX`
  and `_EX_V2`, as Microsoft's USBView tool does), matched to devices by driver key.
- Events: the Windows event logs, see "WINDOWS EVENTS" and "Session info file".
- Session info: SMBIOS values from the registry, `GetSystemPowerStatus`, `GetPwrCapabilities` and the
  active power plan.

## Log file format

Text is UTF-8 with CRLF line endings. Device and class names and event messages are localized to the
Windows display language of the machine (for example German "USB-Stammrouter" for "USB Root Router"), so
match devices by their IDs rather than their names.

### Header

```
Dock-Debug tracking log
Session:  3
Tracker:  USB tree
Event:    change (snapshot differs from the previous one logged by this tracker)
Captured: 2026-01-31T14:05:09.250+01:00
Windows:  Windows 11 Professional 24H2 (build 26100.4061, x64)
Format:   see about-dock-debug-logs.md in this folder
```

`Session` repeats the number from the file name. `Captured` is local time with its UTC offset. `Windows`
shows the registry EditionID (`Core` means Home, `Professional` means Pro), the feature version and the
exact build.

Marker files start with `Dock-Debug problem marker` and `Event: marker`, followed by the `NOTE:` line.

After the header come, in this order: WHAT CHANGED (only in `change` files), WINDOWS EVENTS (in every
file), and the snapshot (not in `stopped` files), starting with a summary line
`LIVE VIEW  /  <n> displays  /  <m> tracked devices` and a POWER line:

```
POWER: AC power · battery 91 %, discharging at 12.4 W, 16.9 V · 73.3 Wh now, 80.4 Wh full, 87.4 Wh design · DRAINING ON AC POWER: ...
```

The power source, the battery charge, what the battery is doing at that moment (charging / discharging
with the rate in watts, voltage) and its capacity now, at full charge and by design. "DRAINING ON AC
POWER" means the computer is on external power but still uses its battery: the power supply, often a
USB-C / Thunderbolt dock, delivers less than the computer needs. That is a common cause of docks
disconnecting under load, and it can come and go with the workload (for example when the GPU is busy).

### WHAT CHANGED

The differences between this snapshot and the one in the previous file of the same tracker (named in the
section title), for displays and for devices:

```
WHAT CHANGED since s3_2026-01-31_14-04-51-020_usb-tree_started.log
  Displays: no change
  Devices:
    removed: CORSAIR IRONCLAW RGB Gaming Mouse (USB Composite Device)  [USB\VID_1B1C&PID_1B5D\0701C0...]  parent USB\VID_0BDA&PID_5485\...
    changed: USB Root Hub (USB 3.0)  [USB\ROOT_HUB30\...]
      status: 0x180200A -> 0x1802400 (problem 43, status 0xC0000001)
```

- `removed` / `added`: the device or display is missing from, or new in, this snapshot. Displays are
  matched by monitor instance ID, devices by instance ID.
- `changed`: the same device or display with different values, listed as `field: old -> new`.
- `re-arrived`: a device whose `last arrival` time changed although it is in both snapshots, meaning it was
  removed and added again between the two snapshots.
- `USB port problems`: `new` / `gone` entries of the USB PORT PROBLEMS list.
- `Power`: a switch between AC power and battery, or the battery starting / stopping to drain on AC power
  (listed when a file is written for another reason; power changes alone do not create a `change` file,
  but appear in WINDOWS EVENTS as Kernel-Power 105).
- Only the fields listed under "What each tracker compares" are compared.

### WINDOWS EVENTS

Event log entries relevant to docks, USB and displays that were written since the previous file of the
same session (for the first files of a session: since the session info file), oldest first:

```
WINDOWS EVENTS from 2026-01-31 14:04:51.020 to 2026-01-31 14:05:09.250 (local time)
  2026-01-31 14:05:08.412  Information  Microsoft-Windows-Kernel-PnP 1010  [Microsoft-Windows-Kernel-PnP/Device Management]
      Device USB\VID_1B1C&PID_1B5D\0701C0... was surprise removed because it was reported as missing on the bus.
```

Together the files of a session cover the whole session without gaps. Sources:

- `Microsoft-Windows-Kernel-PnP/Device Management` and `.../Configuration`: all entries. Useful IDs:
  1010 surprise removal (the device vanished from the bus, as opposed to a clean removal), 400 / 410
  device configured / started, 411 device had a problem starting, 420 / 430 device deleted / needs
  further installation.
- `System`, only from these providers: Kernel-Power (sleep and wake, 41 unexpected power loss,
  105 AC/battery switch, 506 / 507 Modern Standby entry / exit), Power-Troubleshooter (wake source),
  Kernel-PnP and UserPnp (driver load failures, driver installs), Display (4101: the display driver
  stopped responding and recovered, which blanks monitors for a few seconds), Dxgkrnl and GPU drivers
  (nvlddmkm NVIDIA, amdkmdag AMD, igfx Intel), USB / xHCI / UCM (USB Type-C) / HID / Thunderbolt
  providers, WHEA-Logger (PCIe and other hardware errors; Thunderbolt tunnels PCIe), BugCheck and
  WER-SystemErrorReporting (blue screens), EventLog (6008 unexpected shutdown), WindowsUpdateClient
  (19 installed, 20 failed; Microsoft Defender signature and Microsoft Store app updates are left out).
- `Microsoft-Windows-DeviceSetupManager/Admin`: only warnings and errors (failed driver downloads and
  installs); its information entries are routine.
- `Application`, only Windows Error Reporting 1001 reports of the kinds `BlueScreen` and
  `LiveKernelEvent` (see "Session info file" for the codes). These are *reports*: Windows queues them and
  often sends many at once, possibly long after the crash; the dump file name in the report carries the
  crash date (`C:\Windows\Minidump\MMDDYY-...dmp`, `C:\Windows\LiveKernelReports\<area>\<area>-YYYYMMDD-HHMM.dmp`).

Shadow copy volumes (`VolumeSnapshot`), which backups and restore points add and remove, are left out.
At most 300 entries per file; when there are more, the oldest are left out and the section says how
many. If a log cannot be read, the section says so.

### MONITORS

One entry per monitor: first the active ones in source order, then monitors that are connected but not part
of the desktop (`[INACTIVE]`).

```
  [ACTIVE]  DELL U2723QE  [PRIMARY]
      source: \\.\DISPLAY1  |  mode: 3840 x 2160 @ 59.997 Hz (59997/1000), 32 bpp, rotation 0, scaling 150 %
      signal: pixel clock 533250 kHz  |  estimated data rate ≈ 12.8 Gbit/s (533.25 MHz × 24 bit), uncompressed
      connection: DisplayPort  |  advanced color (HDR): supported, off, 8 bits per channel
      monitor: DISPLAY\DELA1B2\5&2D6A5C3E&0&UID4352
      EDID: DEL A1B2  |  serial: 7XYZ123  |  manufactured: 2023 week 12
      adapter: NVIDIA GeForce RTX 4070 Laptop GPU
```

- `[PRIMARY]`: the monitor showing the primary desktop (taskbar with the clock by default).
- Name: the model name from the monitor's EDID. Built-in panels often have none and appear as
  "Built-in display".
- `source`: the display source (`\\.\DISPLAYn`). Windows may renumber sources after reconnects, so identify
  monitors by `monitor` or EDID serial, not by `source`.
- `mode`: resolution and exact refresh rate (the fraction as reported by Windows), color depth, rotation,
  Windows display scaling.
- `signal`: the pixel clock of the output signal (including blanking) and the data rate it needs
  uncompressed, pixel clock × bits per pixel on the link. If the link uses DSC compression the real rate is
  lower; Windows does not report whether DSC is active. Compare the sum over monitors on one dock with the
  dock's DisplayPort capacity (for example a DisplayPort 1.4 HBR3 stream carries about 25.9 Gbit/s of data);
  several monitors behind an MST hub in a dock share one DisplayPort stream.
- `connection`: how the GPU drives the monitor: DisplayPort, HDMI, DVI, VGA, Internal (eDP or LVDS),
  "DisplayPort over USB tunnel", "Indirect display (USB, e.g. DisplayLink)" and others. Monitors on a
  Thunderbolt / USB4 / USB-C dock usually show DisplayPort (alternate mode or tunneling); DisplayLink-style
  docks show as indirect displays with their own adapter.
- `advanced color (HDR)`: whether the monitor supports it and whether it is switched on, and the bits per
  color channel on the link.
- `monitor`: the monitor's device instance ID, the same device as the `[Monitor]` entry in the device tree.
  After `DISPLAY\` comes the EDID PnP ID.
- `EDID`: 3-letter manufacturer code and 4-hex-digit product code (for example DEL Dell, GSM LG, SAM
  Samsung, ACR Acer, AUS Asus, BNQ BenQ, HWP HP, LEN Lenovo; AUO, BOE, LGD, SDC, CMN and SHP are usually
  built-in laptop panels), the serial number and the manufacturing date.
- `adapter`: the GPU (or display adapter) driving the monitor.
- `EDID data`: the monitor's complete EDID as Windows stored it (128-byte base block plus extension blocks,
  for example CTA-861 or DisplayID), 32 bytes per line. Decode it for the supported resolutions and
  refresh rates, the preferred timing, maximum pixel clock, HDR and color capabilities, and DisplayPort /
  HDMI features. Useful when the current mode differs from what the monitor supports or prefers.

### USB DEVICE TREE

All present devices matching this filter, in Windows enumeration order:

- device class `USB`, `Monitor` or `Display`, or
- any device whose name, instance ID or hardware ID contains "thunderbolt" or "usb4" (case-insensitive), or
- any device connected through such a USB / USB4 / Thunderbolt device: the functions of a dock or USB
  device in other classes, for example `[Net]` network adapters, `[MEDIA]` audio, `[HIDClass]` /
  `[Keyboard]` / `[Mouse]` input, `[DiskDrive]` storage, `[Camera]`. Software devices (`SWD\...`) and
  the devices behind a Bluetooth radio are left out.

```
[TB/USB4] USB4(TM) Host Router
  class: USB4HostRouter  status: 0x180200A
  instance: PCI\VEN_8086&DEV_7EC2&SUBSYS_...\3&11583659&0&6A  parent: PCI\VEN_8086&DEV_7E4E&...\3&1158...
  hardware: PCI\VEN_8086&DEV_7EC2&SUBSYS_...  |  maker: Microsoft  |  location: PCI bus 0, device 13, function 2
  driver: Microsoft 10.0.26100.4061 (2025-05-01)  |  firmware revision: none
  last arrival: 2026-01-31 09:12:40  |  last removal: none
  power: D0 (on)  |  turn off to save power: not configured (driver default)
  [TB/USB4] USB4 Root Device Router
    class: USB  status: 0x180000A
    instance: USB4\ROOT_DEVICE_ROUTER&VID_8086&PID_7EC2\...  parent: PCI\VEN_8086&DEV_7EC2&...
    ...
          [USB] Generic SuperSpeed USB Hub
            ...
            usb: port 3 of parent  |  SuperSpeed+ (USB 3.x, 10 Gbit/s or more)  |  device: SuperSpeed+ capable, port: USB 3  |  port status: connected
            power: D2 (low power)  |  turn off to save power: allowed (WDF IdleInWorkingState=1)
```

- The list is nested: each device is directly followed by its children, indented two more spaces per
  level. Devices whose parent is not in the list (for example a USB controller whose parent is a PCI
  bridge) start at the left margin. Siblings keep Windows enumeration order, which can change between
  snapshots, so compare devices by `instance` rather than by line position.
- Names: when a USB device reports its own product name, it comes first and the Windows name follows in
  parentheses, e.g. `CORSAIR K55 RGB PRO Gaming Keyboard (USB Composite Device)`. Entries without their own
  product name that belong to the same physical device (same container) as such an ancestor get its name
  after a dot, e.g. `HID Keyboard Device · CORSAIR K55 RGB PRO Gaming Keyboard`. Otherwise the Windows name
  alone. Unnamed devices appear as `(unnamed device)` or `(unnamed <class> device)`.
- Tag: `[TB/USB4]` when the name or IDs mention Thunderbolt or USB4 (a text match, not a bus check),
  `[USB]` for other USB devices, otherwise the device class in brackets (for example `[Monitor]`).
- `class`: the Windows device setup class.
- `status`: devnode status flags from `CM_Get_DevNode_Status` in hex, followed by `(problem N)` when
  Windows reports a problem, plus the underlying NTSTATUS when there is one. Useful flags: 0x2 driver
  loaded, 0x8 started, 0x400 has a problem, 0x2000 can be disabled, 0x4000 removable, 0x800000 / 0x1000000
  enumerated / driven by NT drivers. A healthy fixed device is typically `0x180200A`, a healthy removable
  device `0x180600A`. Problem numbers match Device Manager's "Code N", for example 10 cannot start,
  22 disabled, 24 not present or not working, 28 drivers not installed, 31 not working properly,
  43 device reported a problem, 45 not connected, 52 driver signature could not be verified.
- `instance`: the device instance ID. For USB devices it contains `VID_xxxx&PID_xxxx` (vendor and product,
  for example VID_8086 Intel, VID_0BDA Realtek, VID_413C Dell, VID_17EF Lenovo, VID_2109 VIA Labs,
  VID_05E3 Genesys Logic, VID_1B1C Corsair); the last segment is a serial number or a port-based ID.
- `parent`: the parent device's instance ID (it may be outside this list, for example a PCI controller).
- `hardware`: the first (most specific) hardware ID; `maker`: the manufacturer from the driver package;
  `location`: the driver-reported location (for example `Port_#0002.Hub_#0004`); `container`: the ID
  Windows uses to group all entries of one physical device (a dock's hubs, network adapter and other
  functions usually share one). The line is left out when all are empty.
- `driver`: provider, version and date of the installed driver. `firmware revision`: the `REV_xxxx` part of
  the hardware ID; for USB devices this is the device release number (bcdDevice), which usually changes
  with firmware updates. `firmware version`: reported by some drivers, for example the USB4 router of a
  Thunderbolt / USB4 dock (the dock's Thunderbolt firmware). The line is left out when nothing is known.
- `last arrival` / `last removal`: when Windows last added / removed this device (local time). A last
  arrival shortly before a problem means the device (re)connected then. The line is left out when neither
  is known; `last removal` is usually empty for devices that are present.
- `usb` (USB devices only): the port on the parent hub, the speed the device is actually running at, what
  the device and the port support, and the port status. A SuperSpeed-capable device running at High
  Speed on a USB 3 port ("running below the device's capability") points at the cable, a hub or the
  port. Every USB 3 hub also appears as a separate USB 2.0 hub on the USB 2 bus ("Generic USB Hub",
  High Speed, "port: USB 2 only"); that is normal and not a fallback.
- `power`: the device power state (D0 on; D2 / D3 low power or suspended, normal for idle USB devices with
  selective suspend) and Device Manager's "Allow the computer to turn off this device to save power",
  with the registry values it was read from (`WDF IdleInWorkingState` for KMDF drivers such as USB hubs,
  `EnhancedPowerManagementEnabled` for many USB devices; 1 = allowed).

### USB PORT PROBLEMS

USB hub ports reporting a problem, whether or not Windows created a device for it:
`hub <hub instance ID>, port <n>: <status>`, or `none`. Statuses: failed enumeration (a device is
connected but did not respond correctly, often a cable, power or firmware problem), general failure,
overcurrent (the device drew too much power and the port was switched off), not enough power, not
enough bandwidth, hubs nested too deeply, being reset.

### THUNDERBOLT / USB4 DETAIL

The `[TB/USB4]` devices again, each with instance, parent, driver service name, status and hardware ID.
If none match, the section says "No Thunderbolt or USB4 devices matched in the current snapshot."

### Session info file

```
COMPUTER
  Manufacturer: ...  |  Model: ...  |  BIOS: ...
POWER
  Power source: AC power · battery 91 %, neither charging nor discharging, 16.9 V · 73.3 Wh now, 80.4 Wh full, 87.4 Wh design
  Power plan: Balanced
  Sleep: Modern Standby (S0 low power idle)
  USB selective suspend: on (AC power), on (battery)
  PCI Express link state power management: off (AC power), off (battery)
```

followed by RELIABILITY HISTORY and by WINDOWS EVENTS for the 30 minutes before tracking started.

RELIABILITY HISTORY groups the last 30 days of these events (the ones Windows Reliability Monitor is
built from), newest first within each category:

```
  Blue screen:
    0x116 VIDEO_TDR_FAILURE (graphics driver did not recover from a timeout)
      12 reports on 3 days, 2026-01-20 19:45:33 to 2026-01-29 23:05:29  |  newest dump: C:\WINDOWS\Minidump\012926-24609-01.dmp
  Kernel live dump (driver hang, no blue screen):
    0x144 BUGCODE_USB3_DRIVER (USB 3 controller or hub problem)
      ...
```

- Categories: `Blue screen` (WER BlueScreen reports and the bugcheck logged at the next boot),
  `Kernel live dump` (WER LiveKernelEvent: a driver hung and Windows recovered without a blue screen;
  the dump folder names the area, for example `USBXHCI` or `WATCHDOG`), `Unexpected shutdown`
  (Kernel-Power 41, EventLog 6008), `Driver installed` (UserPnp), `Windows Update failed` / `installed`.
- Counts are reports, not crashes: Windows can report one crash several times and sends queued reports
  in batches, so compare the number of days and the dump file names rather than the counts.
- Stop codes worth knowing for docks and displays: 0x116 / 0x117 / 0x141 graphics driver timeouts
  (TDR), 0x193 graphics kernel live dump, 0x144 USB 3 controller or hub, 0xFE USB driver, 0x9F driver
  power state failure (often around sleep / wake), 0x133 DPC watchdog (a driver blocked the system too
  long), 0x124 hardware error, 0x15C / 0x15F Modern Standby watchdogs, 0x1D4 USB Type-C connector.
  Repeated graphics timeouts together with monitor dropouts point at the GPU driver or the display link
  through the dock rather than at the monitors.

Points worth checking:

- USB selective suspend `on` lets Windows power down idle USB devices; it is a common cause of USB and dock
  dropouts and worth switching off as a test.
- PCI Express link state power management other than `off` can affect Thunderbolt / USB4 docks, which
  tunnel PCIe.
- Modern Standby machines keep running some activity while "asleep"; dock problems right after waking
  show up as Kernel-Power 507 (exit Modern Standby) shortly before the change.
- The power source here is the state when the session started; every snapshot repeats it in its POWER
  line, and AC/battery switches also appear as Kernel-Power 105.

## Known limitations

- Only devices present at capture time are listed; a removed device simply disappears between files.
- Not captured: USB bandwidth use, power delivery contracts, Thunderbolt / USB4 link details (speed,
  lanes, how the link's bandwidth is divided), dock vendor tools and firmware details beyond the hardware
  ID revision and the USB4 router firmware version, which monitor runs through which dock or cable, the
  contents of the crash dumps
  (`C:\Windows\Minidump`, `C:\Windows\LiveKernelReports`; only their names appear in the reports), the
  DirectX graphics kernel log and detailed USB traces. Most of these need administrator rights; if they
  matter, ask the user for them (for example the dock vendor's firmware version, or a dump analyzed with
  WinDbg `!analyze -v`).
- The Thunderbolt / USB4 tag is a text match; some such devices may appear as plain `[USB]` and the other
  way round.
- Local times can jump with daylight saving or clock changes; compare the UTC offsets in the headers.

## Suggested analysis

1. List the log files and group them by session, and within each session by tracker. If the user names a
   time or a session, focus on it.
2. Read the session's session info file, including its RELIABILITY HISTORY: recurring blue screens or
   kernel live dumps (graphics timeouts, USB 3 controller) often explain dropouts better than any single
   snapshot.
3. If there are marker files, look at the `change` files and events in the minutes before each marker
   first; they are the moments the user noticed a problem.
4. For each `change` file, read WHAT CHANGED, then the WINDOWS EVENTS of that file for the cause and the
   exact time: a surprise removal (Kernel-PnP 1010), a display driver reset (Display 4101), waking from
   sleep (Kernel-Power 507 / Power-Troubleshooter), PCIe or USB controller errors (WHEA, xHCI), a kernel
   live dump or blue screen report. Check `usb` lines for devices running below their capability and the
   USB PORT PROBLEMS list.
5. Correlate `monitor` and `usb-tree` changes within a few seconds of each other. For example, dock hubs
   re-enumerating at the same moment monitors drop out points at the dock connection rather than the
   monitors.
6. Identify hardware by VID/PID, EDID codes and serial numbers; Windows names are localized and generic.
7. Check the power settings in the session info file and the POWER lines against the symptoms, especially
   "DRAINING ON AC POWER" around the time of a problem.
)md";

struct CaptureTime {
    std::wstring iso;     // 2026-01-31T14:05:09.250+01:00
    std::wstring file;    // 2026-01-31_14-05-09-250
    std::wstring offset;  // +01:00
    std::wstring local;   // 2026-01-31 14:05:09.250
    unsigned long long utc = 0;
};

long long Ticks(const SYSTEMTIME& time) {
    FILETIME file{};
    SystemTimeToFileTime(&time, &file);
    return static_cast<long long>((static_cast<unsigned long long>(file.dwHighDateTime) << 32) | file.dwLowDateTime);
}

std::wstring LocalTimeText(unsigned long long utcTime) {
    FILETIME file{ static_cast<DWORD>(utcTime), static_cast<DWORD>(utcTime >> 32) };
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    FileTimeToSystemTime(&file, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    wchar_t text[40]{};
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", local.wYear, local.wMonth, local.wDay, local.wHour,
               local.wMinute, local.wSecond, local.wMilliseconds);
    return text;
}

CaptureTime Now() {
    CaptureTime result;
    result.utc = UtcNow();
    FILETIME file{ static_cast<DWORD>(result.utc), static_cast<DWORD>(result.utc >> 32) };
    SYSTEMTIME utc{};
    FileTimeToSystemTime(&file, &utc);
    SYSTEMTIME local{};
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    const long long minutes = (Ticks(local) - Ticks(utc)) / 600000000LL;
    const long long absolute = minutes < 0 ? -minutes : minutes;

    wchar_t offset[16]{};
    swprintf_s(offset, L"%c%02lld:%02lld", minutes < 0 ? L'-' : L'+', absolute / 60, absolute % 60);
    wchar_t iso[64]{};
    swprintf_s(iso, L"%04u-%02u-%02uT%02u:%02u:%02u.%03u%s", local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute, local.wSecond, local.wMilliseconds, offset);
    wchar_t name[64]{};
    swprintf_s(name, L"%04u-%02u-%02u_%02u-%02u-%02u-%03u", local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
    result.iso = iso;
    result.file = name;
    result.offset = offset;
    result.local = LocalTimeText(result.utc);
    return result;
}

std::wstring RegistryString(const wchar_t* name) {
    wchar_t buffer[256]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name,
                     RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return {};
    }
    return buffer;
}

DWORD RegistryDword(const wchar_t* name) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name,
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return 0;
    }
    return value;
}

std::wstring WindowsVersion() {
    const std::wstring build = RegistryString(L"CurrentBuildNumber");
    const std::wstring edition = RegistryString(L"EditionID");
    const std::wstring displayVersion = RegistryString(L"DisplayVersion");
    const int buildNumber = _wtoi(build.c_str());

    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    std::wstring architecture = L"unknown architecture";
    switch (system.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: architecture = L"x64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: architecture = L"ARM64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: architecture = L"x86"; break;
    }

    // ProductName still says "Windows 10" on Windows 11, so derive the name from the build number.
    std::wstring result = buildNumber >= 22000 ? L"Windows 11" : buildNumber > 0 ? L"Windows 10" : L"Windows";
    if (!edition.empty()) {
        result += L" " + edition;
    }
    if (!displayVersion.empty()) {
        result += L" " + displayVersion;
    }
    return result + L" (build " + (build.empty() ? L"?" : build) + L"." + std::to_wstring(RegistryDword(L"UBR")) +
           L", " + architecture + L")";
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

// Writes `text` as UTF-8. With CREATE_NEW this fails (ERROR_FILE_EXISTS) instead of overwriting.
bool WriteTextFile(const std::wstring& path, const std::wstring& text, DWORD disposition) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, disposition,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const std::string bytes = ToUtf8(text);
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == bytes.size();
}

void WriteAboutFile(const std::wstring& directory, const CaptureTime& time, const std::wstring& windows) {
    std::wstring about = AboutIntro;
    about += L"- Windows: " + windows + L"\n";
    about += L"- UTC offset of local time: " + time.offset + L"\n";
    about += L"- This file was last written: " + time.iso + L"\n";
    about += AboutBody;
    WriteTextFile(directory + L"\\" + AboutFileName, about, CREATE_ALWAYS);
}

// Writes `body` as <stem>.log, or <stem>-2.log etc. if that exists; returns the file name.
std::wstring WriteNewLogFile(const std::wstring& directory, const std::wstring& stem, const std::wstring& body) {
    for (int attempt = 1; attempt < 100; ++attempt) {
        const std::wstring name = stem + (attempt == 1 ? L"" : L"-" + std::to_wstring(attempt)) + L".log";
        if (WriteTextFile(directory + L"\\" + name, body, CREATE_NEW)) {
            return name;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) {
            return {};
        }
    }
    return {};
}

// The WINDOWS EVENTS section for (session.eventsCoveredUntil, until]; advances the session past them.
std::wstring FormatEventsSince(TrackingSession& session, unsigned long long until) {
    const unsigned long long from = session.eventsCoveredUntil;
    const WindowsEventList list = ReadWindowsEvents(from > EventOverlap ? from - EventOverlap : 0, until, MaxEventsPerFile);
    session.eventsCoveredUntil = until;

    std::wstringstream output;
    output << L"WINDOWS EVENTS from " << LocalTimeText(from) << L" to " << LocalTimeText(until) << L" (local time)\r\n";
    size_t shown = 0;
    for (const WindowsEvent& event : list.events) {
        if (!session.writtenEvents.insert({ event.channel, event.recordId }).second) {
            continue;  // already in an earlier file of this session
        }
        ++shown;
        output << L"  " << event.timeText << L"  " << event.level << L"  " << event.provider << L" " << event.id
               << L"  [" << event.channel << L"]\r\n      " << event.message << L"\r\n";
    }
    if (shown == 0) {
        output << L"  none\r\n";
    }
    if (list.omitted > 0) {
        output << L"  (" << list.omitted << L" older matching events left out; at most " << MaxEventsPerFile
               << L" per file)\r\n";
    }
    if (!list.error.empty()) {
        output << L"  Could not read: " << list.error << L"\r\n";
    }
    return output.str();
}

// ------------------------------------------------------------------ WHAT CHANGED

void CompareField(std::vector<std::wstring>& changes, const wchar_t* name, const std::wstring& before,
                  const std::wstring& after) {
    if (before != after) {
        changes.push_back(std::wstring(name) + L": " + OrNone(before) + L" -> " + OrNone(after));
    }
}

std::wstring DisplayKey(const DisplayRecord& display) {
    return display.monitorInstanceId.empty() ? display.sourceName + L"|" + DisplayLabel(display) : display.monitorInstanceId;
}

std::wstring DisplaySummary(const DisplayRecord& display) {
    return DisplayLabel(display) + L"  [" + OrNone(display.monitorInstanceId) + L"]  " + OrNone(display.connection) +
           L", " + DisplayMode(display);
}

} // namespace

std::wstring FormatChanges(const HardwareSnapshot& before, const HardwareSnapshot& after) {
    std::wstringstream output;

    std::map<std::wstring, const DisplayRecord*> displaysBefore;
    for (const DisplayRecord& display : before.displays) {
        displaysBefore[DisplayKey(display)] = &display;
    }
    std::vector<std::wstring> displayLines;
    std::set<std::wstring> displaysAfter;
    for (const DisplayRecord& display : after.displays) {
        displaysAfter.insert(DisplayKey(display));
        const auto old = displaysBefore.find(DisplayKey(display));
        if (old == displaysBefore.end()) {
            displayLines.push_back(L"added: " + DisplaySummary(display));
            continue;
        }
        const DisplayRecord& was = *old->second;
        std::vector<std::wstring> changes;
        CompareField(changes, L"active", was.active ? L"yes" : L"no", display.active ? L"yes" : L"no");
        CompareField(changes, L"primary", was.primary ? L"yes" : L"no", display.primary ? L"yes" : L"no");
        CompareField(changes, L"source", was.sourceName, display.sourceName);
        CompareField(changes, L"mode", DisplayMode(was), DisplayMode(display));
        CompareField(changes, L"color depth", std::to_wstring(was.bitsPerPixel) + L" bpp", std::to_wstring(display.bitsPerPixel) + L" bpp");
        CompareField(changes, L"rotation", std::to_wstring(was.rotation), std::to_wstring(display.rotation));
        CompareField(changes, L"scaling", std::to_wstring(was.scalePercent) + L" %", std::to_wstring(display.scalePercent) + L" %");
        CompareField(changes, L"connection", was.connection, display.connection);
        CompareField(changes, L"advanced color (HDR)", was.advancedColorEnabled ? L"on" : L"off",
                     display.advancedColorEnabled ? L"on" : L"off");
        CompareField(changes, L"name", was.monitorName, display.monitorName);
        if (!changes.empty()) {
            displayLines.push_back(L"changed: " + DisplayLabel(display) + L"  [" + OrNone(display.monitorInstanceId) + L"]");
            for (const std::wstring& change : changes) {
                displayLines.push_back(L"  " + change);
            }
        }
    }
    for (const DisplayRecord& display : before.displays) {
        if (displaysAfter.count(DisplayKey(display)) == 0) {
            displayLines.insert(displayLines.begin(), L"removed: " + DisplaySummary(display));
        }
    }

    std::map<std::wstring, const DeviceRecord*> devicesBefore;
    for (const DeviceRecord& device : before.devices) {
        devicesBefore[device.instanceId] = &device;
    }
    std::vector<std::wstring> removed;
    std::vector<std::wstring> added;
    std::vector<std::wstring> changed;
    std::set<std::wstring> devicesAfter;
    for (const DeviceRecord& device : after.devices) {
        devicesAfter.insert(device.instanceId);
        const auto old = devicesBefore.find(device.instanceId);
        if (old == devicesBefore.end()) {
            added.push_back(L"added: " + DeviceLabel(device) + L"  [" + device.instanceId + L"]  parent " +
                            OrNone(device.parentInstanceId) + L", status " + device.status);
            continue;
        }
        const DeviceRecord& was = *old->second;
        std::vector<std::wstring> changes;
        CompareField(changes, L"status", was.status, device.status);
        CompareField(changes, L"name", was.friendlyName, device.friendlyName);
        CompareField(changes, L"class", was.className, device.className);
        CompareField(changes, L"parent", was.parentInstanceId, device.parentInstanceId);
        CompareField(changes, L"hardware ID", was.hardwareId, device.hardwareId);
        CompareField(changes, L"driver version", was.driverVersion, device.driverVersion);
        CompareField(changes, L"usb speed", was.usbSpeed, device.usbSpeed);
        CompareField(changes, L"usb port status", was.usbPortStatus, device.usbPortStatus);
        if (was.lastArrival != device.lastArrival) {
            changes.push_back(L"re-arrived: last arrival " + OrNone(was.lastArrival) + L" -> " + OrNone(device.lastArrival) +
                              L" (removed and added again between the two snapshots)");
        }
        if (!changes.empty()) {
            changed.push_back(L"changed: " + DeviceLabel(device) + L"  [" + device.instanceId + L"]");
            for (const std::wstring& change : changes) {
                changed.push_back(L"  " + change);
            }
        }
    }
    for (const DeviceRecord& device : before.devices) {
        if (devicesAfter.count(device.instanceId) == 0) {
            removed.push_back(L"removed: " + DeviceLabel(device) + L"  [" + device.instanceId + L"]  parent " +
                              OrNone(device.parentInstanceId));
        }
    }

    auto section = [&](const wchar_t* title, const std::vector<std::wstring>& lines) {
        if (lines.empty()) {
            output << L"  " << title << L": no change\r\n";
            return;
        }
        output << L"  " << title << L":\r\n";
        for (const std::wstring& line : lines) {
            output << L"    " << line << L"\r\n";
        }
    };
    section(L"Displays", displayLines);
    std::vector<std::wstring> deviceLines = removed;
    deviceLines.insert(deviceLines.end(), added.begin(), added.end());
    deviceLines.insert(deviceLines.end(), changed.begin(), changed.end());
    section(L"Devices", deviceLines);

    auto portText = [](const UsbPortProblem& problem) {
        return L"hub " + problem.hubInstanceId + L", port " + std::to_wstring(problem.port) + L": " + problem.status;
    };
    std::set<std::wstring> portsBefore;
    std::set<std::wstring> portsAfter;
    for (const UsbPortProblem& problem : before.usbPortProblems) {
        portsBefore.insert(portText(problem));
    }
    for (const UsbPortProblem& problem : after.usbPortProblems) {
        portsAfter.insert(portText(problem));
    }
    std::vector<std::wstring> portLines;
    for (const std::wstring& text : portsAfter) {
        if (portsBefore.count(text) == 0) {
            portLines.push_back(L"new: " + text);
        }
    }
    for (const std::wstring& text : portsBefore) {
        if (portsAfter.count(text) == 0) {
            portLines.push_back(L"gone: " + text);
        }
    }
    section(L"USB port problems", portLines);

    std::vector<std::wstring> powerLines;
    CompareField(powerLines, L"power source", before.power.onAc ? L"AC power" : L"battery", after.power.onAc ? L"AC power" : L"battery");
    CompareField(powerLines, L"battery draining on AC power", before.power.drainingOnAc ? L"yes" : L"no",
                 after.power.drainingOnAc ? L"yes" : L"no");
    section(L"Power", powerLines);
    return output.str();
}

namespace {

std::wstring Header(const wchar_t* title, int session, const CaptureTime& time, const std::wstring& windows,
                    const std::wstring& extra) {
    return std::wstring(title) + L"\r\n" +
           L"Session:  " + std::to_wstring(session) + L"\r\n" + extra +
           L"Captured: " + time.iso + L"\r\n" +
           L"Windows:  " + windows + L"\r\n" +
           L"Format:   see " + AboutFileName + L" in this folder\r\n\r\n";
}

} // namespace

int NextSessionNumber(const std::vector<std::wstring>& fileNames) {
    int highest = 0;
    for (const std::wstring& name : fileNames) {
        // "s12_2026-...log" -> 12. Only digits between the "s" and the first "_", and only .log files.
        const size_t underscore = name.find(L'_');
        if (name.size() < 7 || towlower(name[0]) != L's' || underscore == std::wstring::npos || underscore < 2 ||
            underscore > 7 || Lower(name.substr(name.size() - 4)) != L".log" ||
            !std::all_of(name.begin() + 1, name.begin() + underscore, [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
            continue;
        }
        highest = std::max(highest, std::stoi(name.substr(1, underscore - 1)));
    }
    return highest + 1;
}

TrackingSession StartTrackingSession() {
    TrackingSession session;
    std::vector<std::wstring> fileNames;
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((TrackingDirectory() + L"\\s*_*.log").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            fileNames.push_back(entry.cFileName);
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }
    session.number = NextSessionNumber(fileNames);

    const std::wstring directory = TrackingDirectory();
    const CaptureTime time = Now();
    const std::wstring windows = WindowsVersion();
    WriteAboutFile(directory, time, windows);

    const SystemInfo info = CaptureSystemInfo();
    std::wstring body = Header(L"Dock-Debug session info", session.number, time, windows, L"");
    body += L"COMPUTER\r\n"
            L"  Manufacturer: " + OrNone(info.manufacturer) + L"  |  Model: " + OrNone(info.model) +
            L"  |  BIOS: " + OrNone(info.biosVersion) + L"\r\n"
            L"POWER\r\n"
            L"  Power source: " + OrNone(info.power.summary) + L"\r\n"
            L"  Power plan: " + OrNone(info.powerPlan) + L"\r\n"
            L"  Sleep: " + OrNone(info.sleepModel) + L"\r\n"
            L"  USB selective suspend: " + OrNone(info.usbSelectiveSuspendAc) + L" (AC power), " +
            OrNone(info.usbSelectiveSuspendDc) + L" (battery)\r\n"
            L"  PCI Express link state power management: " + OrNone(info.pcieLinkPowerAc) + L" (AC power), " +
            OrNone(info.pcieLinkPowerDc) + L" (battery)\r\n\r\n";
    const ReliabilityHistory history = ReadReliabilityHistory(HistoryDays);
    body += L"RELIABILITY HISTORY of the last " + std::to_wstring(HistoryDays) + L" days (local time, newest first)\r\n";
    if (history.entries.empty()) {
        body += L"  none\r\n";
    }
    std::wstring category;
    for (const HistoryEntry& item : history.entries) {
        if (item.category != category) {
            category = item.category;
            body += L"  " + category + L":\r\n";
        }
        body += L"    " + item.title + L"\r\n      " + std::to_wstring(item.count) + L" report" +
                (item.count == 1 ? L"" : L"s") + L" on " + std::to_wstring(item.days) + L" day" +
                (item.days == 1 ? L"" : L"s") + L", " +
                (item.count == 1 ? item.last : item.first + L" to " + item.last) +
                (item.detail.empty() ? L"" : L"  |  newest " + item.detail) + L"\r\n";
    }
    if (!history.error.empty()) {
        body += L"  Could not read: " + history.error + L"\r\n";
    }
    body += L"\r\n";

    session.eventsCoveredUntil = time.utc > SessionInfoEventWindow ? time.utc - SessionInfoEventWindow : 0;
    body += FormatEventsSince(session, time.utc);

    WriteNewLogFile(directory, L"s" + std::to_wstring(session.number) + L"_" + time.file + L"_session_info", body);
    return session;
}

std::wstring WriteTrackingLog(TrackingSession& session, Tracker tracker, TrackingEvent event,
                              const HardwareSnapshot* snapshot, const TrackerBaseline* previous) {
    const std::wstring directory = TrackingDirectory();
    const CaptureTime time = Now();
    const std::wstring windows = WindowsVersion();
    WriteAboutFile(directory, time, windows);

    const wchar_t* trackerSlug = tracker == Tracker::Monitor ? L"monitor" : L"usb-tree";
    const wchar_t* trackerName = tracker == Tracker::Monitor ? L"Monitor" : L"USB tree";
    const wchar_t* eventSlug = L"change";
    const wchar_t* eventName = L"change (snapshot differs from the previous one logged by this tracker)";
    if (event == TrackingEvent::Started) {
        eventSlug = L"started";
        eventName = L"started (tracker switched on; baseline snapshot for its later changes)";
    } else if (event == TrackingEvent::Stopped) {
        eventSlug = L"stopped";
        eventName = L"stopped (tracking switched off; no snapshot)";
    }

    std::wstring body = Header(L"Dock-Debug tracking log", session.number, time, windows,
                               std::wstring(L"Tracker:  ") + trackerName + L"\r\n" + L"Event:    " + eventName + L"\r\n");
    if (event == TrackingEvent::Change && snapshot != nullptr && previous != nullptr) {
        body += L"WHAT CHANGED since " + previous->fileName + L"\r\n" + FormatChanges(previous->snapshot, *snapshot) + L"\r\n";
    }
    body += FormatEventsSince(session, time.utc) + L"\r\n";
    if (snapshot != nullptr) {
        body += FormatLive(*snapshot);
    }
    return WriteNewLogFile(directory, L"s" + std::to_wstring(session.number) + L"_" + time.file + L"_" + trackerSlug +
                                          L"_" + eventSlug, body);
}

std::wstring WriteMarker(TrackingSession& session, const std::wstring& note, const HardwareSnapshot& snapshot) {
    const std::wstring directory = TrackingDirectory();
    const CaptureTime time = Now();
    const std::wstring windows = WindowsVersion();
    WriteAboutFile(directory, time, windows);

    // Keep the note on one line so it cannot be mistaken for log structure.
    std::wstring flat;
    for (wchar_t c : note) {
        flat += (c == L'\r' || c == L'\n' || c == L'\t') ? L' ' : c;
    }
    std::wstring body = Header(L"Dock-Debug problem marker", session.number, time, windows,
                               L"Event:    marker (the user noticed a problem at this moment)\r\n");
    body += L"NOTE: " + (flat.empty() ? std::wstring(L"(no note)") : flat) + L"\r\n\r\n";
    body += FormatEventsSince(session, time.utc) + L"\r\n";
    body += FormatLive(snapshot);
    return WriteNewLogFile(directory, L"s" + std::to_wstring(session.number) + L"_" + time.file + L"_marker", body);
}