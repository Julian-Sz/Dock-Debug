# Dock-Debug: notes for AI coding agents

Technical context for AI assistants (and developers) working on this repository. The user-facing guide is
`README.md`. For the meaning of the tracking log files, see the text generated into
`about-dock-debug-logs.md` (source: `AboutIntro` / `AboutBody` in `src/report.cpp`).

## What the app does

Native Windows diagnostic tool for docks: shows the connected displays and the USB / USB4 / Thunderbolt
device tree live, and can track changes to either into per-event log files that a user zips and hands to
an AI for analysis.

## Layout

- `src/monitor.h/.cpp`: hardware snapshot (`CaptureSnapshot`: SetupAPI device enumeration, parent/status
  via cfgmgr32, displays via `EnumDisplayDevices`/`EnumDisplaySettings`) and `DeviceWatcher`, which
  registers USB and monitor interface notifications on an HWND.
- `src/report.h/.cpp`: UI-independent logic shared by both front ends: device classification
  (`IsThunderbolt`, `IsUsbFamily`), change signatures, `FormatLive` (snapshot text), and
  `WriteTrackingLog`, which writes one UTF-8 file per tracking event plus `about-dock-debug-logs.md` into
  `Documents\Dock-Debug\tracking\`.
- `winui/`: the main app, WinUI 3 / Windows App SDK 2.5.1, C++/WinRT, packaged as **MSIX** for the
  Microsoft Store (`Package.appxmanifest` holds the Partner Center identity; Store ID 9MSPPCP4HLL5).
  `/p:WindowsPackageType=None` builds it unpackaged instead. `MainWindow.xaml(.cpp)` holds all UI:
  Mica, TitleBar, NavigationView with Live view and Tracking pages. `DeviceNodeItem` is a bindable class
  used as TreeView node content.
- `src/main.cpp`: older plain Win32/GDI dashboard, built by CMake. Still compiles and shares `report.cpp`,
  but the WinUI app is the one being developed. Known issue: its tracking buttons and tree selection send
  WM_COMMAND/WM_NOTIFY to STATIC container windows that do not forward them.
- `src/service_main.cpp`: Windows Service stub (captures snapshots, logs counts to
  `C:\ProgramData\Dock-Debug\monitor.log`). Not installed or wired to the UI.
- `tools/make-icon.ps1`: generates `winui/Assets/DockDebug.ico` and all MSIX logo PNGs in `winui/Assets/`
  (Feather Icons "zap", MIT; see `THIRD-PARTY-NOTICES.md`). Edit the script, not the PNGs.
- `tools/make-store-package.ps1`: builds Release x64 + ARM64 MSIX packages and bundles them into
  `winui/AppPackages/DockDebug_<version>_x64_arm64.msixupload` for Partner Center.

## Runtime behavior

- Snapshots are captured on a background thread: immediately on `WM_DEVICECHANGE` (the WinUI window
  subclasses its HWND to receive it) and every 10 s by polling. Bursts are coalesced into one follow-up scan.
- The Live view only rebuilds the display cards / device tree when `DisplaySignature` / `UsbSignature`
  changes, and keeps TreeView expansion and selection across rebuilds.
- Tracking: switching a tracker on writes a `started` file with the baseline snapshot; each later
  signature change writes a `change` file; switching off writes `stopped`. Nothing is written on exit.

## Building

WinUI app (main):

```powershell
msbuild winui\DockDebug.vcxproj -t:restore -p:RestorePackagesConfig=true   # first time: NuGet packages into winui\packages
msbuild winui\DockDebug.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:WindowsPackageType=None
.\winui\x64\Debug\DockDebug\DockDebug.exe
```

Requires Visual Studio 2026 (v145 toolset) with the C++ and Windows App SDK workloads. The unpackaged
build needs the Windows App Runtime 2.5 installed. Without `/p:WindowsPackageType=None` the build is
packaged: the .exe cannot be started directly (run it from Visual Studio with Developer Mode on).

Store upload: `powershell -ExecutionPolicy Bypass -File tools\make-store-package.ps1`. Raise `Version` in
`winui/Package.appxmanifest` before each submission (fourth number stays 0).

Win32 dashboard and service (CMake):

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug
```

## Gotchas

- The WinUI project compiles with `/utf-8`; the CMake build does not. Keep `src/*.cpp` ASCII-only (use
  `\uXXXX` escapes) or text will be garbled in one of the two builds.
- WinUI `TitleBar` crashes the process (0xC000027B in Microsoft.UI.Xaml.dll) when its `IconSource` is a
  `PathIconSource`; the title bar icon is an `ImageIconSource` with an in-memory SVG instead.
- In C++/WinRT, `InitializeComponent` runs after the constructor, so window setup lives in the
  `MainWindow::InitializeComponent` override.
- Freshly built, unsigned executables may be blocked by Smart App Control ("An application control policy
  has blocked this file").
- Logs go to Documents, not `%LOCALAPPDATA%`: in the MSIX package, AppData writes are redirected to a
  per-package folder, so "Open folder" in Explorer would show an empty directory.
- Switching between packaged and unpackaged builds reuses the same output folders; if a build behaves
  oddly afterwards, delete `winui\x64`, `winui\ARM64` and `winui\DockDebug`.
- The log format is consumed by AI tools. If you change `FormatLive`, signatures or file naming, update the
  about-file text in `src/report.cpp` in the same change.

## Ideas not implemented yet

- Named-pipe IPC so the service can track while no UI is open (a GUI cannot run in Session 0).
- A "what changed" diff section at the top of each `change` log file.
