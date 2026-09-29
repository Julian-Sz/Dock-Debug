#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <vector>

struct DeviceRecord {
    std::wstring className;
    std::wstring friendlyName;
    std::wstring instanceId;
    std::wstring hardwareId;
    std::wstring parentInstanceId;
    std::wstring manufacturer;
    std::wstring location;
    std::wstring service;
    std::wstring status;
};

struct DisplayRecord {
    std::wstring deviceName;
    std::wstring friendlyName;
    std::wstring deviceString;
    std::wstring adapterName;
    std::wstring adapterString;
    LONG pixelWidth = 0;
    LONG pixelHeight = 0;
    DWORD refreshHz = 0;
    DWORD bitsPerPixel = 0;
    bool active = false;
    bool primary = false;
};

struct HardwareSnapshot {
    std::vector<DeviceRecord> devices;
    std::vector<DisplayRecord> displays;
};

HardwareSnapshot CaptureSnapshot();

class DeviceWatcher {
public:
    using ChangeHandler = std::function<void()>;

    bool Start(void* windowHandle, ChangeHandler handler);
    void Stop();

private:
    ChangeHandler handler_;
    std::vector<void*> notificationHandles_;
};
