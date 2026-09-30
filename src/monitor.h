#pragma once

#include "system.h"

#include <windows.h>
#include <string>
#include <vector>

struct DeviceRecord {
    std::wstring className;
    std::wstring friendlyName;      // the Windows name, as in Device Manager
    std::wstring reportedName;      // USB devices: the product name the device reports ("CORSAIR K55 ... Keyboard")
    std::wstring nameHint;          // no own reported name: that of the nearest ancestor in the same physical device
    std::wstring containerId;       // groups the entries of one physical device
    std::wstring firmwareVersion;   // reported by some drivers, e.g. USB4 routers
    std::wstring instanceId;
    std::wstring hardwareId;
    std::wstring parentInstanceId;
    std::wstring manufacturer;
    std::wstring location;
    std::wstring service;
    std::wstring status;
    // The fields below are empty when Windows does not report them.
    std::wstring driverProvider;
    std::wstring driverVersion;
    std::wstring driverDate;        // YYYY-MM-DD
    std::wstring firmwareRevision;  // the REV_xxxx part of the hardware ID (USB: bcdDevice)
    std::wstring lastArrival;       // local time YYYY-MM-DD HH:MM:SS
    std::wstring lastRemoval;
    // USB connection, from the parent hub (USB devices only).
    std::wstring usbPort;           // "port 3"
    std::wstring usbSpeed;          // "High Speed (USB 2.0, 480 Mbit/s)"
    std::wstring usbCapability;     // what the device and the port support, e.g. "device: SuperSpeed capable"
    std::wstring usbPortStatus;     // "connected", or a problem such as "overcurrent"
    // Power management.
    std::wstring powerState;        // "D0 (on)", "D3 (off / suspended)"
    std::wstring powerSaving;       // Device Manager's "Allow the computer to turn off this device to save power"
    // Not a USB / display device itself, but connected through USB / USB4 / Thunderbolt (for example a dock's
    // network adapter, audio, storage or HID function).
    bool behindUsb = false;
};

// A USB hub port that reports a problem (failed enumeration, overcurrent, ...), whether or not Windows
// created a device for it.
struct UsbPortProblem {
    std::wstring hubInstanceId;
    ULONG port = 0;
    std::wstring status;
};

// One monitor (display target) from QueryDisplayConfig: every active one, plus monitors that are connected
// but not part of the desktop.
struct DisplayRecord {
    std::wstring sourceName;         // \\.\DISPLAYn; empty for an inactive monitor
    std::wstring monitorName;        // model name from the EDID; often empty for built-in panels
    std::wstring monitorInstanceId;  // DISPLAY\DELA1B2\...; the same device appears in the device list
    std::wstring connection;         // DisplayPort, HDMI, Internal (eDP), ...
    std::wstring adapterName;        // the GPU driving it
    std::wstring edidId;             // EDID manufacturer and product code, e.g. "DEL A1B2"
    std::wstring edidSerial;
    std::wstring edidManufactured;   // e.g. "2023 week 12"
    std::vector<BYTE> edid;          // the raw EDID as stored by Windows (base block plus extensions)
    LONG pixelWidth = 0;
    LONG pixelHeight = 0;
    UINT64 pixelRate = 0;            // pixel clock of the output signal in Hz, including blanking
    UINT32 refreshNumerator = 0;     // exact refresh rate as a fraction (59.94 Hz = 60000 / 1001)
    UINT32 refreshDenominator = 0;
    DWORD bitsPerPixel = 0;
    UINT32 rotation = 0;             // degrees
    UINT32 scalePercent = 0;         // Windows display scaling (100, 125, 150, ...); 0 when inactive
    bool active = false;
    bool primary = false;
    bool advancedColorSupported = false;  // HDR / advanced color
    bool advancedColorEnabled = false;
    UINT32 bitsPerColorChannel = 0;
};

struct HardwareSnapshot {
    std::vector<DeviceRecord> devices;
    std::vector<DisplayRecord> displays;
    std::vector<UsbPortProblem> usbPortProblems;
    PowerStatus power;
};

HardwareSnapshot CaptureSnapshot();

// Parsing helpers used by CaptureSnapshot (pure functions, unit tested).

// "USB\VID_1B1C&PID_1B5D&REV_0324" -> "0324"; empty without a REV_ part.
std::wstring FirmwareRevision(const std::wstring& hardwareId);
// "\\?\DISPLAY#DELA1B2#5&2d6a5c3e&0&UID4352#{e6f07b5f-...}" -> "DISPLAY\DELA1B2\5&2D6A5C3E&0&UID4352"
std::wstring InterfacePathToInstanceId(std::wstring path);
// Fills edidId, edidSerial, edidManufactured and (if still empty) monitorName from a raw EDID; does
// nothing unless it starts with a valid 128-byte EDID base block.
void ParseEdid(const std::vector<BYTE>& edid, DisplayRecord& record);

// Registers USB device and monitor interface notifications on a window, which then receives
// WM_DEVICECHANGE when such a device arrives or is removed.
class DeviceWatcher {
public:
    bool Start(HWND window);
    void Stop();

private:
    std::vector<HDEVNOTIFY> notificationHandles_;
};
