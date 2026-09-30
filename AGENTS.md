# Dock-Debug: notes for developers and AI coding agents

Technical reference for this repository. The user guide is `README.md`. The meaning of the log files is
documented in the text the app writes as `about-dock-debug-logs.md` (`AboutIntro` / `AboutBody` in
`src/report.cpp`).

## What the app does

Native Windows diagnostic tool for docks and displays. It shows the connected displays and devices live,
and records changes, problem markers and the relevant Windows events into per-event text files that a
user zips and hands to an AI for analysis. Everything works without administrator rights. The app has no
network access and never sends data anywhere.

## Layout

- `src/monitor.h/.cpp`: the hardware snapshot (`CaptureSnapshot`) and `DeviceWatcher`, which registers USB
  and monitor interface notifications on an HWND.
- `src/system.h/.cpp`: `CapturePowerStatus`, `CaptureSystemInfo` (computer, power plan settings),
  `ReadWindowsEvents` and `ReadReliabilityHistory` (event logs via wevtapi).
- `src/traffic.h/.cpp`: `ReadTrafficCounters`, cumulative byte counters of network adapters and drives.
- `src/report.h/.cpp`: UI-independent logic: device and display labels, change signatures, the snapshot
  text (`FormatLive`), the WHAT CHANGED diff, the about file and the log files (`StartTrackingSession`,
  `WriteTrackingLog`, `WriteMarker`).
- `winui/`: the app, WinUI 3 / Windows App SDK 2.5.1, C++/WinRT, packaged as MSIX for the Microsoft Store
  (`Package.appxmanifest` holds the Partner Center identity; Store ID 9MSPPCP4HLL5). `MainWindow.xaml(.cpp)`
  holds all UI: Mica, TitleBar, NavigationView with the Live view, Tracking, Topology and System pages.
  `DeviceNodeItem` is the bindable TreeView node content. NuGet packages are referenced component by
  component (no Windows App SDK metapackage) in `packages.config`.
- `tools/make-icon.ps1`: generates `winui/Assets/DockDebug.ico` and all MSIX logo PNGs (Feather Icons
  "zap", MIT; see `THIRD-PARTY-NOTICES.md`). Edit the script, not the PNGs.
- `tools/make-store-package.ps1`: builds Release x64 + ARM64 and bundles them into
  `winui/AppPackages/DockDebug_<version>_x64_arm64.msixupload`.

## Data collection

Snapshot (`CaptureSnapshot`, about 0.1 s):

- Devices: SetupAPI enumeration in two passes. Pass 1 reads class, names, IDs and parent of every present
  device. A device is kept if it passes `IsInterestingDevice` (class USB / Monitor / Display, or
  Thunderbolt / USB4 in its name or IDs) or has an `IsUsbConnection` ancestor (`behindUsb`: a dock's
  network adapter, audio, HID, storage; not `SWD\` devices, not the devices below a Bluetooth radio). The
  keep decision is made for all devices before pass 2 moves records out, because it reads ancestors.
  Pass 2 reads the rest for kept devices only: status (cfgmgr32), driver, firmware version, arrival /
  removal time, power state and container ID (`SetupDiGetDeviceProperty`), power-saving settings
  (`Device Parameters` key).
- Instance IDs are stored in upper case: Windows returns the same ID in different cases from different
  APIs, which otherwise breaks parent links.
- Names: `reportedName` is the USB product string (`DEVPKEY_Device_BusReportedDeviceDesc`, `USB\` devices
  only); `nameHint` is the reported name of the nearest ancestor with the same `containerId`. Windows'
  container display name is not used (a dock's container can be named after its network adapter).
- USB ports: `ReadUsbPorts` opens every USB hub interface and queries each port
  (`IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX` / `_EX_V2`) like Microsoft's USBView; devices are matched
  by driver key (`SPDRP_DRIVER`). Ports with a problem status become `usbPortProblems`.
- Displays: `QueryDisplayConfig(QDC_ALL_PATHS)` + `DisplayConfigGetDeviceInfo` (source, monitor name,
  connection type, advanced color, adapter), pixel clock from the target mode, EDID from the monitor's
  `Device Parameters` key, scaling via `GetDpiForMonitor` (correct only because the app is PerMonitorV2
  DPI aware).
- Power: `CapturePowerStatus`: AC / battery plus battery rate, voltage and capacity via the battery class
  IOCTLs; `drainingOnAc` flags a power supply (often a dock) that delivers too little.

Other sources:

- `ReadWindowsEvents`: the two Kernel-PnP channels fully; the System log filtered by provider keywords
  (`IsRelevantSystemProvider`); DeviceSetupManager warnings and errors; WER BlueScreen / LiveKernelEvent
  reports from the Application log. `IsNoise` drops shadow copies, Defender signature updates and
  Microsoft Store app updates.
- `ReadReliabilityHistory`: the same kinds of events over 30 days, grouped by category and stop code
  (`StopCodeName`).
- `CaptureSystemInfo`: SMBIOS values from the registry, sleep model, USB selective suspend and PCIe ASPM
  of the active power plan.
- `ReadTrafficCounters`: `GetIfTable2` and `IOCTL_DISK_PERFORMANCE` (works without admin).

Deliberately not used: the USB4 routers' undocumented property set
{5DF7E321-1C1B-4CE2-B4FA-55F4A5BC2CB6} (behind *Settings › USB4 hubs and devices*; e.g. 20 / 21 look like
link generation and lane configuration). Its encoding is unverified, and features must work for every
dock, not just one vendor's.

## Runtime behavior

- Snapshots run on a background thread: immediately on `WM_DEVICECHANGE` and `WM_DISPLAYCHANGE` (the
  window subclasses its HWND to receive them) and every 10 s by polling. Bursts are coalesced into one
  follow-up scan.
- The Live view only rebuilds the display cards / device tree when `DisplaySignature` / `UsbSignature`
  changes, and keeps TreeView expansion and selection across rebuilds. The Topology page rebuilds on the
  same signatures while visible and polls traffic every 2 s.
- Tracking: switching a tracker on writes a `started` file with the baseline snapshot; each later
  signature change writes a `change` file with a WHAT CHANGED diff against the tracker's
  `TrackerBaseline`; switching off writes `stopped`. Nothing is written on exit; closing the window ends
  the process.
- Sessions: switching on a tracker while none is running calls `StartTrackingSession` (highest `s<N>_`
  prefix in the folder + 1, writes `s<N>_<time>_session_info.log` with the 30-day reliability history).
  Both trackers write with that number until both are off.
- Problem markers: *Mark problem* (Tracking page; Live view header while tracking) and Ctrl+M (a
  `KeyboardAccelerator` on the root grid) take a fresh snapshot, apply it (so a pending change file is
  written first) and call `WriteMarker`. Tracking must be on.
- Every file gets the WINDOWS EVENTS since the previous file of the session: `TrackingSession` tracks the
  covered time and the written record IDs; queries overlap by 60 s because events can be logged late.
- Log writing (including the event query, about 10-50 ms) runs on the UI thread.

## Building, testing, releasing

```powershell
msbuild winui\DockDebug.vcxproj -t:restore -p:RestorePackagesConfig=true   # first time: NuGet packages into winui\packages
msbuild winui\DockDebug.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:WindowsPackageType=None
.\winui\x64\Debug\DockDebug\DockDebug.exe
```

- Requires Visual Studio 2026 (v145 toolset) with the C++ and Windows App SDK workloads. The unpackaged
  build (`/p:WindowsPackageType=None`) needs the Windows App Runtime 2.5 installed. Without it the build is
  packaged and the .exe cannot be started directly (run it from Visual Studio with Developer Mode on).
- Unit tests: `powershell -ExecutionPolicy Bypass -File tests\run-unit-tests.ps1` builds and runs
  `tests/UnitTests` (a console program compiling `src/*.cpp` with the tests; a minimal harness in
  `test.h`, no external dependencies; exit code = failed tests). They cover the pure logic with made-up
  data: labels and names, display values, change signatures, the WHAT CHANGED diff (`FormatChanges`),
  session numbering (`NextSessionNumber`), EDID parsing (`ParseEdid`), `FirmwareRevision`,
  `InterfacePathToInstanceId` and the event filters (`IsRelevantSystemProvider`, `IsNoise`,
  `StopCodeTitle`); these are exposed in the headers for that reason. One smoke test runs the real data
  collection on the current machine. Add a test when changing any of this logic.
- UI test: `powershell -ExecutionPolicy Bypass -File tests\ui-test.ps1` drives the unpackaged Debug build
  through Windows UI Automation (trackers, markers, pages, the files written, clean exit). It needs a real
  desktop session and writes into the real log folder (it deletes only its own files), so it is a manual
  check before a release, not for CI.
- Code that simulates device changes for manual testing must not stay in the app.
- Store upload: `powershell -ExecutionPolicy Bypass -File tools\make-store-package.ps1`. Raise `Version`
  in `winui/Package.appxmanifest` before each submission (the fourth number stays 0).

## Gotchas

- WinUI `TitleBar` crashes the process (0xC000027B in Microsoft.UI.Xaml.dll) when its `IconSource` is a
  `PathIconSource`; the title bar icon is an `ImageIconSource` with an in-memory SVG instead.
- WinUI `TreeView` corrupts its heap (0xC0000005 / 0xC0000374 in Microsoft.UI.Xaml.Controls.dll) when
  `RootNodes().Clear()` removes the selected node. `RebuildDeviceTree` sets `SelectedNode(nullptr)` first;
  keep that when changing the rebuild.
- In C++/WinRT, `InitializeComponent` runs after the constructor, so window setup lives in the
  `MainWindow::InitializeComponent` override.
- Logs go to Documents, not `%LOCALAPPDATA%`: in the MSIX package, AppData writes are redirected to a
  per-package folder, so "Open folder" in Explorer would show an empty directory.
- Freshly built, unsigned executables may be blocked by Smart App Control ("An application control policy
  has blocked this file").
- Switching between packaged and unpackaged builds reuses the same output folders; if a build behaves
  oddly afterwards, delete `winui\x64`, `winui\ARM64` and `winui\DockDebug`.
- Segoe Fluent icon glyphs in C++ are written as `\uXXXX` escapes; literal private-use characters in the
  source are invisible and easy to break when editing.
- The log format is consumed by AI tools. If you change the snapshot text, signatures, the diff, event
  sources or file naming, update the about-file text in `src/report.cpp` in the same change.

## Ideas not implemented yet

- Tracking while the window is closed (a tray icon, or a separate background process; a Store app cannot
  easily install a Windows service).
- Thunderbolt / USB4 link speed and lanes, once there is a documented, vendor-independent source.
