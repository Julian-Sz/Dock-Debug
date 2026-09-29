#include "report.h"

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

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(towlower(character));
    });
    return value;
}

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

std::wstring DeviceLabel(const DeviceRecord& device) {
    if (!device.friendlyName.empty()) {
        return device.friendlyName;
    }
    if (!device.className.empty()) {
        return L"(unnamed " + device.className + L" device)";
    }
    return L"(unnamed device)";
}

std::wstring DisplayMode(const DisplayRecord& display) {
    std::wstringstream output;
    output << display.pixelWidth << L" x " << display.pixelHeight << L" @ " << display.refreshHz << L" Hz";
    return output.str();
}

std::wstring DeviceDetails(const DeviceRecord& device) {
    std::wstringstream output;
    output << DeviceLabel(device) << L"\r\n\r\n"
           << L"Class: " << device.className << L"\r\n"
           << L"Status: " << device.status << L"\r\n"
           << L"Instance ID: " << device.instanceId << L"\r\n"
           << L"Parent: " << device.parentInstanceId << L"\r\n"
           << L"Hardware ID: " << device.hardwareId << L"\r\n"
           << L"Manufacturer: " << device.manufacturer << L"\r\n"
           << L"Location: " << device.location << L"\r\n"
           << L"Service: " << device.service;
    return output.str();
}

std::wstring DisplaySignature(const HardwareSnapshot& snapshot) {
    std::wstringstream output;
    for (const DisplayRecord& display : snapshot.displays) {
        output << display.deviceName << L'|' << display.friendlyName << L'|' << display.deviceString
               << L'|' << display.pixelWidth << L'x' << display.pixelHeight << L'@' << display.refreshHz << L'|'
               << display.active << L'|' << display.primary << L'\n';
    }
    return output.str();
}

std::wstring UsbSignature(const HardwareSnapshot& snapshot) {
    std::wstringstream output;
    for (const DeviceRecord& device : snapshot.devices) {
        output << device.className << L'|' << device.friendlyName << L'|' << device.instanceId << L'|'
               << device.hardwareId << L'|' << device.parentInstanceId << L'|' << device.status << L'\n';
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

std::wstring FormatLive(const HardwareSnapshot& snapshot) {
    std::wstringstream output;
    output << L"LIVE VIEW  /  " << snapshot.displays.size() << L" displays  /  "
           << snapshot.devices.size() << L" tracked devices\r\n\r\n";
    output << L"MONITORS\r\n";
    for (const DisplayRecord& display : snapshot.displays) {
        output << L"  " << (display.active ? L"[ACTIVE]  " : L"[INACTIVE] ")
               << display.friendlyName << (display.primary ? L"  [PRIMARY]" : L"") << L"\r\n"
               << L"      path: " << display.deviceName << L"  |  mode: "
               << display.pixelWidth << L" x " << display.pixelHeight << L" @ " << display.refreshHz
               << L" Hz, " << display.bitsPerPixel << L" bpp\r\n"
               << L"      id: " << display.deviceString << L"\r\n"
               << L"      adapter: " << display.adapterString << L"\r\n";
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
                   << L"  |  location: " << device.location << L"\r\n";
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

namespace {

constexpr wchar_t AboutFileName[] = L"about-dock-debug-logs.md";

// Keep this text ASCII-only: the CMake build compiles without /utf-8.
constexpr wchar_t AboutIntro[] = LR"md(# About these logs

This folder was written by **Dock-Debug**, a Windows diagnostic tool for docks, Thunderbolt / USB4 / USB
devices and external displays. Its tracking feature saves a snapshot of the connected hardware every time
something changes. Users typically zip the whole folder when a problem happens (monitors blanking or
dropping out, dock devices disconnecting or failing to start, wrong resolution or refresh rate) and hand
it over for analysis.

Dock-Debug runs on arbitrary Windows 10 / 11 PCs with arbitrary docks, GPUs and monitors. Do not assume any
specific hardware; everything below describes the format, and the logs themselves describe the machine.

## Machine that wrote this folder

)md";

constexpr wchar_t AboutBody[] = LR"md(
Every log file repeats its capture time and the Windows version in its header.

## Files

- `about-dock-debug-logs.md`: this file. It is rewritten every time a log file is written.
- `<date>_<time>_<tracker>_<event>.log`: one file per event, for example
  `2026-01-31_14-05-09-250_usb-tree_change.log`.
  - `<date>_<time>` is the local capture time as `YYYY-MM-DD_HH-MM-SS-mmm`, so sorting by file name sorts
    chronologically. (If two events share a millisecond, the second gets a `-2` suffix.)
  - `<tracker>` is `monitor` (display configuration) or `usb-tree` (USB / USB4 / Thunderbolt devices).
    The user switches the two trackers on and off independently.
  - `<event>` is one of:
    - `started`: the tracker was switched on. Contains the baseline snapshot that later changes are
      compared against.
    - `change`: the part of the hardware state this tracker watches differs from the previous file of the
      same tracker. Contains the complete new snapshot, not a diff.
    - `stopped`: the tracker was switched off. Contains no snapshot.
- One tracking session is `started`, then zero or more `change`, then `stopped`. A session without a
  `stopped` file means the app was closed or crashed, or was still running when the folder was zipped;
  Dock-Debug does not write a file when it exits. Nothing is recorded between sessions.

To see what changed, diff consecutive files of the same tracker, starting with the `started` file.

## How snapshots are captured

A snapshot is taken:

- when Windows sends a device-change notification: a USB device or monitor interface arriving or being
  removed, or the generic "device tree changed" broadcast; and
- every 10 seconds by polling, which catches changes without a notification (for example a resolution or
  refresh-rate change, or a device's status changing).

Notifications that arrive while a snapshot is being taken are merged into one follow-up snapshot. As a
result:

- The capture time can be up to about 10 seconds after the real hardware event.
- A device that drops out and comes back within one polling interval, without a notification in between,
  may not appear at all, and several quick changes can end up in a single `change` file.
- A `change` file is only written when the watched state actually differs; an unchanged system produces
  no files.

What each tracker compares:

- `monitor`: for every display entry, the display source name, monitor name, monitor device ID,
  resolution, refresh rate, active flag and primary flag.
- `usb-tree`: for every entry in the device list (which also contains monitor and display-adapter
  devices, see below), its class, name, instance ID, first hardware ID, parent instance ID and status.
  A monitor or GPU driver restart can therefore also cause a `usb-tree` change.

Both trackers write the same full snapshot (displays and devices), whichever part changed.

Data sources (Win32 APIs):

- Displays: `EnumDisplayDevices` for each display source (GPU output) and the monitors attached to it,
  `EnumDisplaySettings(ENUM_CURRENT_SETTINGS)` for the current mode.
- Devices: SetupAPI enumeration of all currently *present* devices (`SetupDiGetClassDevs` with
  `DIGCF_PRESENT | DIGCF_ALLCLASSES`), properties from `SetupDiGetDeviceRegistryProperty`, the parent from
  `CM_Get_Parent`, the status from `CM_Get_DevNode_Status`.

## Log file format

Text is UTF-8 with CRLF line endings. Device and class names are localized to the Windows display language
of the machine (for example German "USB-Stammrouter" for "USB Root Router"), so match devices by their IDs
rather than their names.

### Header

```
Dock-Debug tracking log
Tracker:  USB tree
Event:    change (snapshot differs from the previous one logged by this tracker)
Captured: 2026-01-31T14:05:09.250+01:00
Windows:  Windows 11 Professional 24H2 (build 26100.4061, x64)
Format:   see about-dock-debug-logs.md in this folder
```

`Captured` is local time with its UTC offset. `Windows` shows the registry EditionID (`Core` means Home,
`Professional` means Pro), the feature version and the exact build. The snapshot follows, starting with a summary line:
`LIVE VIEW  /  <n> displays  /  <m> tracked devices`.

### MONITORS

One entry per monitor attached to a display source:

```
  [ACTIVE]  Generic PnP Monitor  [PRIMARY]
      path: \\.\DISPLAY1  |  mode: 3840 x 2160 @ 60 Hz, 32 bpp
      id: MONITOR\DELA1B2\{4d36e96e-e325-11ce-bfc1-08002be10318}\0003
      adapter: Intel(R) Arc(TM) Graphics
```

- `[ACTIVE]` / `[INACTIVE]`: whether the monitor is currently part of the desktop.
- `[PRIMARY]`: the monitor's display source is the primary display.
- Name: the monitor driver's description. Usually "Generic PnP Monitor" (localized), not the model name.
- `path`: the display source name (`\\.\DISPLAYn`). Windows assigns these numbers and may change them after
  reconnects, so identify monitors by `id`, not `path`.
- `mode`: the current mode of that source. `0 x 0 @ 0 Hz, 0 bpp` means the source has no active mode.
- `id`: the monitor device ID. The segment after `MONITOR\` is the EDID PnP ID: a 3-letter manufacturer
  code (for example DEL Dell, GSM LG, SAM Samsung, ACR Acer, AUS Asus, HWP HP, LEN Lenovo; AUO, BOE, LGD,
  SDC, CMN and SHP are usually built-in laptop panels) followed by a 4-hex-digit product code.
- `adapter`: the GPU (or display adapter) driving that source. Monitors on a Thunderbolt / USB4 / USB-C
  dock usually appear under the same GPU as the built-in panel (DisplayPort alternate mode or tunneling);
  DisplayLink-style docks appear with their own adapter.

### USB DEVICE TREE

All present devices matching this filter, in Windows enumeration order:

- device class `USB`, `Monitor` or `Display`, or
- any device whose name, instance ID or hardware ID contains "thunderbolt" or "usb4" (case-insensitive).

Dock functions in other classes (network adapter, audio, HID, storage, card reader, ...) are **not** listed
unless their names or IDs match; only their USB parent nodes (hubs, composite devices) are.

```
[TB/USB4] USB4(TM) Host Router
  class: USB4HostRouter  status: 0x180200A
  instance: PCI\VEN_8086&DEV_7EC2&SUBSYS_...\3&11583659&0&6A  parent: PCI\VEN_8086&DEV_7E4E&...\3&1158...
  hardware: PCI\VEN_8086&DEV_7EC2&SUBSYS_...  |  maker: Microsoft  |  location: PCI bus 0, device 13, function 2
  [TB/USB4] USB4 Root Device Router
    class: USB  status: 0x180000A
    instance: USB4\ROOT_DEVICE_ROUTER&VID_8086&PID_7EC2\...  parent: PCI\VEN_8086&DEV_7EC2&...
    [TB/USB4] USB4 Device Router (Contoso Thunderbolt Dock)
      ...
```

- The list is nested: each device is directly followed by its children, indented two more spaces per
  level. Devices whose parent is not in the list (for example a USB controller whose parent is a PCI
  bridge) start at the left margin. Siblings keep Windows enumeration order, which can change between
  snapshots, so compare devices by `instance` rather than by line position.
- Unnamed devices appear as `(unnamed device)` or `(unnamed <class> device)`.
- Tag: `[TB/USB4]` when the name or IDs mention Thunderbolt or USB4 (a text match, not a bus check),
  `[USB]` for other USB devices, otherwise the device class in brackets (for example `[Monitor]`).
- `class`: the Windows device setup class.
- `status`: devnode status flags from `CM_Get_DevNode_Status` in hex, followed by `(problem N)` when
  Windows reports a problem. Useful flags: 0x2 driver loaded, 0x8 started, 0x400 has a problem,
  0x2000 can be disabled, 0x4000 removable, 0x800000 / 0x1000000 enumerated / driven by NT drivers.
  A healthy fixed device is typically `0x180200A`, a healthy removable device `0x180600A`.
  Problem numbers match Device Manager's "Code N", for example 10 cannot start, 22 disabled,
  24 not present or not working, 28 drivers not installed, 31 not working properly, 43 device reported
  a problem, 45 not connected, 52 driver signature could not be verified.
- `instance`: the device instance ID. For USB devices it contains `VID_xxxx&PID_xxxx` (vendor and product,
  for example VID_8086 Intel, VID_0BDA Realtek, VID_413C Dell, VID_17EF Lenovo, VID_2109 VIA Labs,
  VID_05E3 Genesys Logic); the last segment is a serial number or a port-based ID.
- `parent`: the parent device's instance ID (it may be outside this list, for example a PCI controller).
- `hardware`: the first (most specific) hardware ID; `maker`: the manufacturer from the driver package;
  `location`: the driver-reported location (for example `Port_#0002.Hub_#0004`). The line is left out
  when all three are empty.

### THUNDERBOLT / USB4 DETAIL

The `[TB/USB4]` devices again, each with instance, parent, driver service name, status and hardware ID.
If none match, the section says "No Thunderbolt or USB4 devices matched in the current snapshot."

## Known limitations

- Only devices present at capture time are listed; a removed device simply disappears between files.
- Not captured: driver versions, firmware versions, link speed or bandwidth, power delivery, docking
  station vendor tools, or the Windows event log. If those are needed, ask the user for them (Device
  Manager properties, the dock vendor's firmware version, or the System event log around the capture
  times, for example Kernel-PnP entries).
- The Thunderbolt / USB4 tag is a text match; some such devices may appear as plain `[USB]` and the other
  way round.
- Display data comes from GDI display enumeration; a monitor that Windows detects but has not attached
  to a display source may be missing.
- Local times can jump with daylight saving or clock changes; compare the UTC offsets in the headers.

## Suggested analysis

1. List the log files by name and group them by tracker and session.
2. Diff each `change` file against the previous file of the same tracker. Look for monitors turning
   `[INACTIVE]` or disappearing, mode or refresh-rate changes, devices disappearing and reappearing
   (compare instance IDs), status flags changing, and `(problem N)` appearing.
3. Correlate `monitor` and `usb-tree` events within a few seconds of each other. For example, dock hubs
   re-enumerating at the same moment monitors drop out points at the dock connection rather than the
   monitors.
4. Identify hardware by VID/PID and EDID manufacturer/product codes; names are localized and generic.
)md";

struct CaptureTime {
    std::wstring iso;     // 2026-01-31T14:05:09.250+01:00
    std::wstring file;    // 2026-01-31_14-05-09-250
    std::wstring offset;  // +01:00
};

long long Ticks(const SYSTEMTIME& time) {
    FILETIME file{};
    SystemTimeToFileTime(&time, &file);
    return static_cast<long long>((static_cast<unsigned long long>(file.dwHighDateTime) << 32) | file.dwLowDateTime);
}

CaptureTime Now() {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    SYSTEMTIME local{};
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    const long long minutes = (Ticks(local) - Ticks(utc)) / 600000000LL;
    const long long absolute = minutes < 0 ? -minutes : minutes;

    wchar_t offset[16]{};
    swprintf_s(offset, L"%c%02lld:%02lld", minutes < 0 ? L'-' : L'+', absolute / 60, absolute % 60);
    wchar_t iso[64]{};
    swprintf_s(iso, L"%04u-%02u-%02uT%02u:%02u:%02u.%03u%s", local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute, local.wSecond, local.wMilliseconds, offset);
    wchar_t file[64]{};
    swprintf_s(file, L"%04u-%02u-%02u_%02u-%02u-%02u-%03u", local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
    return { iso, file, offset };
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

} // namespace

std::wstring WriteTrackingLog(Tracker tracker, TrackingEvent event, const HardwareSnapshot* snapshot) {
    const std::wstring directory = TrackingDirectory();
    const CaptureTime time = Now();
    const std::wstring windows = WindowsVersion();

    std::wstring about = AboutIntro;
    about += L"- Windows: " + windows + L"\n";
    about += L"- UTC offset of local time: " + time.offset + L"\n";
    about += L"- This file was last written: " + time.iso + L"\n";
    about += AboutBody;
    WriteTextFile(directory + L"\\" + AboutFileName, about, CREATE_ALWAYS);

    const wchar_t* trackerSlug = tracker == Tracker::Monitor ? L"monitor" : L"usb-tree";
    const wchar_t* trackerName = tracker == Tracker::Monitor ? L"Monitor" : L"USB tree";
    const wchar_t* eventSlug = L"change";
    const wchar_t* eventName = L"change (snapshot differs from the previous one logged by this tracker)";
    if (event == TrackingEvent::Started) {
        eventSlug = L"started";
        eventName = L"started (baseline snapshot for this tracking session)";
    } else if (event == TrackingEvent::Stopped) {
        eventSlug = L"stopped";
        eventName = L"stopped (tracking switched off; no snapshot)";
    }

    std::wstring body = std::wstring(L"Dock-Debug tracking log\r\n") +
                        L"Tracker:  " + trackerName + L"\r\n" +
                        L"Event:    " + eventName + L"\r\n" +
                        L"Captured: " + time.iso + L"\r\n" +
                        L"Windows:  " + windows + L"\r\n" +
                        L"Format:   see " + AboutFileName + L" in this folder\r\n\r\n";
    if (snapshot != nullptr) {
        body += FormatLive(*snapshot);
    }

    const std::wstring stem = time.file + L"_" + trackerSlug + L"_" + eventSlug;
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
