# Dock-Debug

A small Windows tool for tracking down dock problems: monitors that go black or flicker, USB devices
that disconnect, a Thunderbolt / USB4 / USB-C dock that stops working.

It shows your displays and connected USB / Thunderbolt devices live, and it can **record** every change
to log files. Those logs are written so that an AI assistant can read them and help you work out what
went wrong.

## How to use it

1. **Start Dock-Debug.** The *Live view* shows your monitors and the USB / Thunderbolt device tree.
2. **Start recording.** Open the *Tracking* page and switch on **Monitor tracking** and
   **USB tree tracking**.
3. **Wait for something interesting to happen.** Leave the app running (it can stay in the background)
   and use your PC normally until the problem shows up: a monitor drops out, the dock disconnects, a
   device stops responding. Note roughly what time it happened.
4. **Collect the logs.** On the *Tracking* page, click **Open folder**, select everything in it
   (Ctrl+A), right-click and choose **Compress to ZIP file**. Take all of it; nothing needs to be picked
   out by hand.
5. **Upload the zip to an AI of your choice** (Claude, ChatGPT, Gemini, ...), together with a sentence
   about what happened and when. For example:

   > My two external monitors went black for a few seconds around 14:05 while the dock stayed connected.
   > Attached are my Dock-Debug logs. What happened?

   The folder contains an `about-dock-debug-logs.md` file that explains the log format to the AI, so you
   do not need to explain it yourself.

You can switch tracking off again afterwards. The logs stay in the folder until you delete them.

### What is in the logs?

A list of your monitors, docks and USB devices: names, hardware IDs (some include device serial
numbers), driver status, and your Windows version. No files, passwords or personal documents. Have a look
before sharing if you are unsure; they are plain text files.

## Requirements

- Windows 10 or 11.
- The [Windows App Runtime](https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads) 2.5
  (already installed on many Windows 11 PCs).
- If Windows says *"Smart App Control blocked this app"*, the build you have is not code-signed. Signed
  releases avoid this; for a self-built copy you have to allow it or turn Smart App Control off.

## Building from source

```powershell
msbuild winui\DockDebug.vcxproj -t:restore -p:RestorePackagesConfig=true
msbuild winui\DockDebug.vcxproj /p:Configuration=Release /p:Platform=x64
```

This needs Visual Studio 2026 with the C++ and Windows App SDK workloads. Details for developers and
AI coding assistants are in [AGENTS.md](AGENTS.md).

## Credits

The app icon uses the "zap" icon from [Feather Icons](https://feathericons.com) (MIT). See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
