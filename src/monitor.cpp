#include "monitor.h"

#include <initguid.h>
#include <windows.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <devguid.h>
#include <ntddvdeo.h>
#include <setupapi.h>
#include <usbiodef.h>

#include <algorithm>
#include <cwctype>
#include <sstream>

#pragma comment(lib, "setupapi.lib")

namespace {

std::wstring ReadProperty(HDEVINFO infoSet, SP_DEVINFO_DATA& device, DWORD property) {
    DWORD type = 0;
    DWORD required = 0;
    SetupDiGetDeviceRegistryPropertyW(infoSet, &device, property, &type, nullptr, 0, &required);
    if (required == 0) {
        return {};
    }

    std::vector<BYTE> buffer(required);
    if (!SetupDiGetDeviceRegistryPropertyW(infoSet, &device, property, &type,
                                           buffer.data(), static_cast<DWORD>(buffer.size()), nullptr)) {
        return {};
    }

    if (type == REG_MULTI_SZ) {
        return reinterpret_cast<const wchar_t*>(buffer.data());
    }
    return reinterpret_cast<const wchar_t*>(buffer.data());
}

std::wstring ReadInstanceId(HDEVINFO infoSet, SP_DEVINFO_DATA& device) {
    DWORD required = 0;
    SetupDiGetDeviceInstanceIdW(infoSet, &device, nullptr, 0, &required);
    if (required == 0) {
        return {};
    }

    std::wstring result(required, L'\0');
    if (!SetupDiGetDeviceInstanceIdW(infoSet, &device, &result[0], required, nullptr)) {
        return {};
    }
    result.resize(wcslen(result.c_str()));
    return result;
}

std::wstring ReadParentInstanceId(SP_DEVINFO_DATA& device) {
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, device.DevInst, 0) != CR_SUCCESS) {
        return {};
    }
    ULONG required = 0;
    if (CM_Get_Device_ID_Size(&required, parent, 0) != CR_SUCCESS) {
        return {};
    }
    std::wstring result(required + 1, L'\0');
    if (CM_Get_Device_IDW(parent, &result[0], static_cast<ULONG>(result.size()), 0) != CR_SUCCESS) {
        return {};
    }
    result.resize(wcslen(result.c_str()));
    return result;
}

std::wstring ReadStatus(SP_DEVINFO_DATA& device) {
    ULONG status = 0;
    ULONG problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) != CR_SUCCESS) {
        return {};
    }
    std::wstringstream output;
    output << L"0x" << std::hex << std::uppercase << status;
    if (problem != 0) {
        output << L" (problem " << std::dec << problem << L")";
    }
    return output.str();
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

bool IsInterestingDevice(const DeviceRecord& device) {
    const std::wstring className = Lower(device.className);
    const std::wstring searchable = Lower(device.instanceId + L" " + device.hardwareId + L" " + device.friendlyName);
    return className == L"usb" || className == L"monitor" || className == L"display" ||
           searchable.find(L"thunderbolt") != std::wstring::npos ||
           searchable.find(L"usb4") != std::wstring::npos;
}

void CaptureDisplays(std::vector<DisplayRecord>& displays) {
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    for (DWORD adapterIndex = 0; EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0); ++adapterIndex) {
        if ((adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0) {
            adapter = {};
            adapter.cb = sizeof(adapter);
            continue;
        }

        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        for (DWORD monitorIndex = 0; EnumDisplayDevicesW(adapter.DeviceName, monitorIndex, &monitor, 0); ++monitorIndex) {
            DisplayRecord record;
            record.deviceName = adapter.DeviceName;
            record.friendlyName = monitor.DeviceString;
            record.deviceString = monitor.DeviceID;
            record.adapterName = adapter.DeviceName;
            record.adapterString = adapter.DeviceString;
            record.active = (monitor.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0;
            record.primary = (adapter.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                record.pixelWidth = mode.dmPelsWidth;
                record.pixelHeight = mode.dmPelsHeight;
                record.refreshHz = mode.dmDisplayFrequency;
                record.bitsPerPixel = mode.dmBitsPerPel;
            }
            displays.push_back(std::move(record));
            monitor = {};
            monitor.cb = sizeof(monitor);
        }

        adapter = {};
        adapter.cb = sizeof(adapter);
    }
}

} // namespace

HardwareSnapshot CaptureSnapshot() {
    HardwareSnapshot snapshot;
    HDEVINFO infoSet = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (infoSet == INVALID_HANDLE_VALUE) {
        return snapshot;
    }

    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(infoSet, index, &device); ++index) {
        DeviceRecord record;
        record.className = ReadProperty(infoSet, device, SPDRP_CLASS);
        record.friendlyName = ReadProperty(infoSet, device, SPDRP_FRIENDLYNAME);
        if (record.friendlyName.empty()) {
            record.friendlyName = ReadProperty(infoSet, device, SPDRP_DEVICEDESC);
        }
        record.instanceId = ReadInstanceId(infoSet, device);
        record.hardwareId = ReadProperty(infoSet, device, SPDRP_HARDWAREID);
        record.parentInstanceId = ReadParentInstanceId(device);
        record.manufacturer = ReadProperty(infoSet, device, SPDRP_MFG);
        record.location = ReadProperty(infoSet, device, SPDRP_LOCATION_INFORMATION);
        record.service = ReadProperty(infoSet, device, SPDRP_SERVICE);
        record.status = ReadStatus(device);
        if (IsInterestingDevice(record)) {
            snapshot.devices.push_back(std::move(record));
        }
        device = {};
        device.cbSize = sizeof(device);
    }

    SetupDiDestroyDeviceInfoList(infoSet);
    CaptureDisplays(snapshot.displays);
    return snapshot;
}

bool DeviceWatcher::Start(void* windowHandle, ChangeHandler handler) {
    Stop();
    handler_ = std::move(handler);

    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

    const GUID interfaces[] = {GUID_DEVINTERFACE_USB_DEVICE, GUID_DEVINTERFACE_MONITOR};
    for (const GUID& interfaceClass : interfaces) {
        filter.dbcc_classguid = interfaceClass;
        HDEVNOTIFY notification = RegisterDeviceNotificationW(windowHandle, &filter,
                                                               DEVICE_NOTIFY_WINDOW_HANDLE);
        if (notification == nullptr) {
            Stop();
            return false;
        }
        notificationHandles_.push_back(notification);
    }
    return true;
}

void DeviceWatcher::Stop() {
    for (void* notification : notificationHandles_) {
        UnregisterDeviceNotification(notification);
    }
    notificationHandles_.clear();
    handler_ = {};
}
