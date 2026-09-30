# Dock-Debug

A Windows tool for tracking down dock and display problems: monitors that go black or flicker, USB
devices that disconnect, a Thunderbolt / USB4 / USB-C dock that stops working.

Dock-Debug shows your displays and connected devices live, and it can **record** every change together
with the relevant Windows events. The recordings are written so that an AI assistant (Claude, ChatGPT,
Gemini, ...) can read them and work out what went wrong.

## What the app shows

- **Live view**: your displays (model, resolution, refresh rate, connection, scaling) and a tree of all
  USB, USB4 and Thunderbolt devices plus everything connected through them, such as a dock's network
  adapter. Select a device for its details: driver, firmware, USB speed, power state, when it last
  connected.
- **Tracking**: switch recording on and off, mark problems, open the log folder.
- **Topology**: how everything is physically connected (Thunderbolt / USB4 connection with firmware
  versions, hubs, ports), the speed each USB connection negotiated, the estimated bandwidth of your
  displays and the live traffic of network adapters and drives.
- **System**: your computer model, power settings that often cause dropouts (for example USB selective
  suspend), blue screens, driver hangs, driver installs and updates of the last 30 days, and the recent
  device, USB, display and power events from the Windows event logs.

## How to find out what is wrong

1. **Start recording.** Open *Tracking* and switch on **Monitor tracking** and **USB tree tracking**.
2. **Use your PC normally until the problem shows up.** Keep Dock-Debug open; minimizing it is fine.
3. **When the problem happens, press Ctrl+M** (or *Mark problem*). You can type a short note first, for
   example "left monitor went black". The marker tells the AI exactly which moment to look at.
4. **Collect the logs.** On *Tracking*, click **Open folder** (`Documents\Dock-Debug\tracking`), select
   everything (Ctrl+A), right-click and choose **Compress to ZIP file**.
5. **Give the zip to an AI** together with a sentence about what happened, for example:

   > My two external monitors went black for a few seconds around 14:05 while the dock stayed connected.
   > Attached are my Dock-Debug logs. What happened?

   The folder contains `about-dock-debug-logs.md`, which explains the log format to the AI; you do not
   need to explain anything yourself.

Switch tracking off when you are done. The logs stay in the folder until you delete them.

### Good to know

- **Recordings are numbered.** File names start with `s1_`, `s2_`, ...; a new number starts each time you
  switch tracking on after both trackers were off, and the *Tracking* page shows the current one. To
  share a single recording, zip the files with its prefix plus `about-dock-debug-logs.md`.
- **Tracking runs only while the window is open.** Minimizing and locking the PC (Windows + L) are fine;
  closing the window ends tracking. While the PC sleeps nothing is scanned, but the Windows events of that
  time are still included afterwards.
- **Each recording starts with a session info file**: computer, power settings, the crash and update
  history of the last 30 days and the Windows events of the 30 minutes before you started. Even a short
  recording therefore helps with problems that happened earlier.
- **The System page may point at the cause directly**, for example with a warning about USB selective
  suspend, a battery that drains although the dock supplies power, or recurring blue screens of the
  graphics or USB drivers.

## What is in the logs?

- Your monitors (model names, serial numbers, EDID data), docks, USB devices and the devices connected
  through them (network adapters, audio, input devices, storage): names, hardware IDs (some include
  device serial numbers), driver and firmware versions, USB connection speeds, power states and status.
- Power source and battery state (charge, charging / discharging rate, capacity).
- Your computer model, BIOS version, Windows version and power settings.
- A 30-day history of blue screens, driver hangs, unexpected shutdowns, driver installs and Windows
  updates (the names of crash dump files, not their contents).
- Windows event log entries about devices, USB, displays, power and hardware errors. Only these sources
  are read, and their messages are mostly about devices and drivers.
- Your problem markers and the notes you typed into them.

No files, passwords or personal documents, and nothing is sent anywhere: the logs only leave your PC when
you share them. They are plain text files, so have a look before sharing if you are unsure. See also the
[privacy policy](https://julian-sz.github.io/Dock-Debug/privacy.html).

## Requirements

- Windows 10 (version 1809 or later) or Windows 11, on an Intel / AMD (x64) or ARM64 PC. Developed and
  tested on Windows 11.
- No administrator rights.
- Installed from the Microsoft Store, everything else it needs is installed automatically.

## Limitations

Windows does not report everything, so some things cannot be shown for any dock:

- the live traffic of individual USB devices (only network adapters and drives are measured);
- how a Thunderbolt / USB4 link divides its bandwidth between displays, USB and PCIe, and the link speed
  itself (Windows shows it in *Settings › Bluetooth & devices › USB › USB4 hubs and devices*);
- which monitor runs through which dock or cable;
- the contents of crash dumps (analyzing them needs administrator rights and WinDbg).

## For developers

Build and run a plain .exe (needs Visual Studio 2026 with the C++ and Windows App SDK workloads, and the
[Windows App Runtime](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads) 2.5):

```powershell
msbuild winui\DockDebug.vcxproj -t:restore -p:RestorePackagesConfig=true
msbuild winui\DockDebug.vcxproj /p:Configuration=Release /p:Platform=x64 /p:WindowsPackageType=None
.\winui\x64\Release\DockDebug\DockDebug.exe
```

If Windows says *"Smart App Control blocked this app"*, the self-built .exe is not code-signed; allow it
or turn Smart App Control off.

Run the tests:

```powershell
powershell -ExecutionPolicy Bypass -File tests\run-unit-tests.ps1   # unit tests, no hardware needed
powershell -ExecutionPolicy Bypass -File tests\ui-test.ps1          # end-to-end test of the Debug build, needs a desktop session
```

Build the Microsoft Store upload (x64 and ARM64, unsigned on purpose; the Store signs it):

```powershell
powershell -ExecutionPolicy Bypass -File tools\make-store-package.ps1
```

It writes `winui\AppPackages\DockDebug_<version>_x64_arm64.msixupload` for the *Packages* step of the
Partner Center submission. Raise `Version` in [winui/Package.appxmanifest](winui/Package.appxmanifest)
before each new submission (the fourth number must stay 0).

Architecture, data sources and pitfalls are described in [AGENTS.md](AGENTS.md).

## Credits

The app icon uses the "zap" icon from [Feather Icons](https://feathericons.com) (MIT). See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
