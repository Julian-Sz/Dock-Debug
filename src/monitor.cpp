#include "monitor.h"

#include <initguid.h>
#include <windows.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <devpkey.h>
#include <ntddvdeo.h>
#include <setupapi.h>
#include <shellscalingapi.h>
#include <usbiodef.h>
#include <winioctl.h>
#include <usbioctl.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "ole32.lib")

namespace {

std::wstring ReadProperty(HDEVINFO infoSet, SP_DEVINFO_DATA& device, DWORD property) {
    DWORD type = 0;
    DWORD required = 0;
    SetupDiGetDeviceRegistryPropertyW(infoSet, &device, property, &type, nullptr, 0, &required);
    if (required == 0) {
        return {};
    }

    std::vector<BYTE> buffer(required + sizeof(wchar_t));
    if (!SetupDiGetDeviceRegistryPropertyW(infoSet, &device, property, &type,
                                           buffer.data(), required, nullptr)) {
        return {};
    }
    // For REG_MULTI_SZ properties (hardware IDs) this is the first, most specific entry.
    return reinterpret_cast<const wchar_t*>(buffer.data());
}

// A DEVPKEY property as raw bytes; empty when the device does not have it.
std::vector<BYTE> ReadDeviceProperty(HDEVINFO infoSet, SP_DEVINFO_DATA& device, const DEVPROPKEY& key,
                                     DEVPROPTYPE expectedType) {
    DEVPROPTYPE type = 0;
    DWORD required = 0;
    SetupDiGetDevicePropertyW(infoSet, &device, &key, &type, nullptr, 0, &required, 0);
    if (required == 0 || type != expectedType) {
        return {};
    }
    std::vector<BYTE> buffer(required + sizeof(wchar_t));
    if (!SetupDiGetDevicePropertyW(infoSet, &device, &key, &type, buffer.data(), required, nullptr, 0)) {
        return {};
    }
    return buffer;
}

std::wstring ReadStringProperty(HDEVINFO infoSet, SP_DEVINFO_DATA& device, const DEVPROPKEY& key) {
    const std::vector<BYTE> value = ReadDeviceProperty(infoSet, device, key, DEVPROP_TYPE_STRING);
    return value.empty() ? std::wstring() : reinterpret_cast<const wchar_t*>(value.data());
}

std::wstring FormatFileTime(const FILETIME& time, bool dateOnly) {
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&time, &utc)) {
        return {};
    }
    wchar_t text[32]{};
    if (dateOnly) {
        // Driver dates are stored as midnight UTC; converting them to local time could shift the day.
        swprintf_s(text, L"%04u-%02u-%02u", utc.wYear, utc.wMonth, utc.wDay);
    } else {
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
        swprintf_s(text, L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour,
                   local.wMinute, local.wSecond);
    }
    return text;
}

std::wstring ReadTimeProperty(HDEVINFO infoSet, SP_DEVINFO_DATA& device, const DEVPROPKEY& key, bool dateOnly) {
    const std::vector<BYTE> value = ReadDeviceProperty(infoSet, device, key, DEVPROP_TYPE_FILETIME);
    return value.size() < sizeof(FILETIME) ? std::wstring()
                                           : FormatFileTime(*reinterpret_cast<const FILETIME*>(value.data()), dateOnly);
}

// Instance IDs are case-insensitive, but Windows returns the same ID in different cases depending on the
// API (for example "9&13678d7c&0&0000" and "9&13678D7C&0&0000"). All IDs are stored in upper case so that
// parent links and comparisons match.
std::wstring Upper(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return value;
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
    return Upper(result);
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
    return Upper(result);
}

// "0x180200A", or "0x1802400 (problem 43, status 0xC0000001)" when Windows reports a problem.
std::wstring ReadStatus(HDEVINFO infoSet, SP_DEVINFO_DATA& device) {
    ULONG status = 0;
    ULONG problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) != CR_SUCCESS) {
        return {};
    }
    std::wstringstream output;
    output << L"0x" << std::hex << std::uppercase << status;
    if (problem != 0) {
        output << L" (problem " << std::dec << problem;
        const std::vector<BYTE> ntstatus = ReadDeviceProperty(infoSet, device, DEVPKEY_Device_ProblemStatus,
                                                              DEVPROP_TYPE_NTSTATUS);
        if (ntstatus.size() >= sizeof(LONG) && *reinterpret_cast<const LONG*>(ntstatus.data()) != 0) {
            output << L", status 0x" << std::hex << std::uppercase
                   << static_cast<ULONG>(*reinterpret_cast<const LONG*>(ntstatus.data()));
        }
        output << L")";
    }
    return output.str();
}

} // namespace

std::wstring FirmwareRevision(const std::wstring& hardwareId) {
    const size_t start = hardwareId.find(L"REV_");
    if (start == std::wstring::npos) {
        return {};
    }
    const size_t end = hardwareId.find_first_of(L"&\\", start);
    return hardwareId.substr(start + 4, end == std::wstring::npos ? std::wstring::npos : end - start - 4);
}

namespace {

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

// ------------------------------------------------------------------ power

std::wstring ReadPowerState(HDEVINFO infoSet, SP_DEVINFO_DATA& device) {
    const std::vector<BYTE> value = ReadDeviceProperty(infoSet, device, DEVPKEY_Device_PowerData, DEVPROP_TYPE_BINARY);
    if (value.size() < sizeof(CM_POWER_DATA)) {
        return {};
    }
    switch (reinterpret_cast<const CM_POWER_DATA*>(value.data())->PD_MostRecentPowerState) {
    case PowerDeviceD0: return L"D0 (on)";
    case PowerDeviceD1: return L"D1 (low power)";
    case PowerDeviceD2: return L"D2 (low power)";
    case PowerDeviceD3: return L"D3 (off / suspended)";
    default: return {};
    }
}

// A DWORD (or 1..4 byte binary) value; false when missing.
bool ReadSmallValue(HKEY key, const wchar_t* subkey, const wchar_t* name, DWORD& value) {
    BYTE data[4]{};
    DWORD size = sizeof(data);
    if (RegGetValueW(key, subkey, name, RRF_RT_REG_DWORD | RRF_RT_REG_BINARY, nullptr, data, &size) != ERROR_SUCCESS ||
        size == 0) {
        return false;
    }
    value = 0;
    memcpy(&value, data, size);
    return true;
}

// Device Manager's "Allow the computer to turn off this device to save power" lives in the device's
// "Device Parameters" key: WDF\IdleInWorkingState for KMDF drivers (USB hubs), EnhancedPowerManagementEnabled
// for many USB devices. Returns the setting and the raw values found.
std::wstring ReadPowerSaving(HDEVINFO infoSet, SP_DEVINFO_DATA& device) {
    HKEY key = SetupDiOpenDevRegKey(infoSet, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) {
        return {};
    }
    struct Value {
        const wchar_t* subkey;
        const wchar_t* name;
        const wchar_t* label;
    };
    static const Value values[] = {
        { L"WDF", L"IdleInWorkingState", L"WDF IdleInWorkingState" },
        { nullptr, L"EnhancedPowerManagementEnabled", L"EnhancedPowerManagementEnabled" },
        { nullptr, L"SelectiveSuspendEnabled", L"SelectiveSuspendEnabled" },
        { nullptr, L"AllowIdleIrpInD3", L"AllowIdleIrpInD3" },
    };
    std::wstring raw;
    int allowed = -1;  // from the first of the two values that decide the checkbox
    for (size_t i = 0; i < std::size(values); ++i) {
        DWORD value = 0;
        if (ReadSmallValue(key, values[i].subkey, values[i].name, value)) {
            raw += (raw.empty() ? L"" : L", ") + std::wstring(values[i].label) + L"=" + std::to_wstring(value);
            if (i < 2 && allowed < 0) {
                allowed = value != 0 ? 1 : 0;
            }
        }
    }
    RegCloseKey(key);
    if (raw.empty()) {
        return L"not configured (driver default)";
    }
    return std::wstring(allowed == 1 ? L"allowed" : allowed == 0 ? L"not allowed" : L"not configured") + L" (" + raw + L")";
}

// ------------------------------------------------------------------ USB ports

struct UsbPortInfo {
    ULONG port = 0;
    std::wstring speed;
    std::wstring capability;
    std::wstring status;
};

std::wstring ConnectionStatusText(USB_CONNECTION_STATUS status) {
    switch (status) {
    case DeviceConnected: return L"connected";
    case DeviceFailedEnumeration: return L"failed enumeration (device did not respond correctly)";
    case DeviceGeneralFailure: return L"general failure";
    case DeviceCausedOvercurrent: return L"overcurrent (device drew too much power, port switched off)";
    case DeviceNotEnoughPower: return L"not enough power";
    case DeviceNotEnoughBandwidth: return L"not enough bandwidth";
    case DeviceHubNestedTooDeeply: return L"hubs nested too deeply";
    case DeviceInLegacyHub: return L"in legacy hub";
    case DeviceEnumerating: return L"enumerating";
    case DeviceReset: return L"being reset";
    default: return L"unknown status " + std::to_wstring(static_cast<int>(status));
    }
}

// The driver key ("{36fc9e60-...}\0005") of the device on a hub port; empty when there is none.
std::wstring PortDriverKey(HANDLE hub, ULONG port) {
    USB_NODE_CONNECTION_DRIVERKEY_NAME probe{};
    probe.ConnectionIndex = port;
    DWORD bytes = 0;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME, &probe, sizeof(probe), &probe,
                         sizeof(probe), &bytes, nullptr) ||
        probe.ActualLength <= sizeof(probe)) {
        return {};
    }
    std::vector<BYTE> buffer(probe.ActualLength + sizeof(wchar_t));
    auto* name = reinterpret_cast<USB_NODE_CONNECTION_DRIVERKEY_NAME*>(buffer.data());
    name->ConnectionIndex = port;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME, name, probe.ActualLength, name,
                         probe.ActualLength, &bytes, nullptr)) {
        return {};
    }
    return Lower(name->DriverKeyName);
}

// Reads every port of every USB hub (as Microsoft's USBView does). Connected devices are returned by driver
// key; ports reporting a problem are added to `problems`.
std::map<std::wstring, UsbPortInfo> ReadUsbPorts(std::vector<UsbPortProblem>& problems) {
    std::map<std::wstring, UsbPortInfo> ports;
    HDEVINFO hubs = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_HUB, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hubs == INVALID_HANDLE_VALUE) {
        return ports;
    }
    SP_DEVICE_INTERFACE_DATA hubInterface{};
    hubInterface.cbSize = sizeof(hubInterface);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(hubs, nullptr, &GUID_DEVINTERFACE_USB_HUB, index, &hubInterface); ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(hubs, &hubInterface, nullptr, 0, &required, nullptr);
        std::vector<BYTE> detailBuffer(required + sizeof(wchar_t));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA hubDevice{};
        hubDevice.cbSize = sizeof(hubDevice);
        if (required == 0 || !SetupDiGetDeviceInterfaceDetailW(hubs, &hubInterface, detail, required, nullptr, &hubDevice)) {
            continue;
        }
        const std::wstring hubId = ReadInstanceId(hubs, hubDevice);
        HANDLE hub = CreateFileW(detail->DevicePath, GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hub == INVALID_HANDLE_VALUE) {
            hub = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (hub == INVALID_HANDLE_VALUE) {
            continue;
        }

        DWORD bytes = 0;
        ULONG portCount = 0;
        USB_HUB_INFORMATION_EX hubInfo{};
        if (DeviceIoControl(hub, IOCTL_USB_GET_HUB_INFORMATION_EX, nullptr, 0, &hubInfo, sizeof(hubInfo), &bytes, nullptr)) {
            portCount = hubInfo.HighestPortNumber;
        } else {
            USB_NODE_INFORMATION node{};
            if (DeviceIoControl(hub, IOCTL_USB_GET_NODE_INFORMATION, &node, sizeof(node), &node, sizeof(node), &bytes, nullptr)) {
                portCount = node.u.HubInformation.HubDescriptor.bNumberOfPorts;
            }
        }

        for (ULONG port = 1; port <= portCount; ++port) {
            std::vector<BYTE> buffer(sizeof(USB_NODE_CONNECTION_INFORMATION_EX) + 32 * sizeof(USB_PIPE_INFO));
            auto* connection = reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(buffer.data());
            connection->ConnectionIndex = port;
            if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, connection,
                                 static_cast<DWORD>(buffer.size()), connection, static_cast<DWORD>(buffer.size()), &bytes, nullptr) ||
                connection->ConnectionStatus == NoDeviceConnected) {
                continue;
            }

            UsbPortInfo info;
            info.port = port;
            info.status = ConnectionStatusText(connection->ConnectionStatus);

            USB_NODE_CONNECTION_INFORMATION_EX_V2 v2{};
            v2.ConnectionIndex = port;
            v2.Length = sizeof(v2);
            v2.SupportedUsbProtocols.Usb110 = 1;
            v2.SupportedUsbProtocols.Usb200 = 1;
            v2.SupportedUsbProtocols.Usb300 = 1;
            const bool haveV2 = DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2, &v2, sizeof(v2), &v2,
                                                sizeof(v2), &bytes, nullptr) != FALSE;
            const bool superSpeedPlus = haveV2 && v2.Flags.DeviceIsOperatingAtSuperSpeedPlusOrHigher;
            const bool superSpeed = superSpeedPlus || (haveV2 && v2.Flags.DeviceIsOperatingAtSuperSpeedOrHigher) ||
                                    connection->Speed == UsbSuperSpeed;
            if (connection->ConnectionStatus == DeviceConnected) {
                info.speed = superSpeedPlus ? L"SuperSpeed+ (USB 3.x, 10 Gbit/s or more)"
                           : superSpeed ? L"SuperSpeed (USB 3.x, 5 Gbit/s)"
                           : connection->Speed == UsbHighSpeed ? L"High Speed (USB 2.0, 480 Mbit/s)"
                           : connection->Speed == UsbFullSpeed ? L"Full Speed (USB 1.1, 12 Mbit/s)"
                           : connection->Speed == UsbLowSpeed ? L"Low Speed (USB 1.1, 1.5 Mbit/s)"
                                                              : L"unknown";
            }
            if (haveV2) {
                const std::wstring device = v2.Flags.DeviceIsSuperSpeedPlusCapableOrHigher ? L"SuperSpeed+ capable"
                                          : v2.Flags.DeviceIsSuperSpeedCapableOrHigher ? L"SuperSpeed capable"
                                                                                        : L"USB 2.0 or lower";
                info.capability = L"device: " + device + L", port: " +
                                  (v2.SupportedUsbProtocols.Usb300 ? L"USB 3" : L"USB 2 only");
                if (v2.Flags.DeviceIsSuperSpeedCapableOrHigher && !superSpeed && v2.SupportedUsbProtocols.Usb300) {
                    info.capability += L"; running below the device's capability (cable, hub or port problem?)";
                }
            }
            if (connection->ConnectionStatus != DeviceConnected && connection->ConnectionStatus != DeviceEnumerating) {
                problems.push_back({ hubId, port, info.status });
            }
            const std::wstring driverKey = PortDriverKey(hub, port);
            if (!driverKey.empty()) {
                ports[driverKey] = info;
            }
        }
        CloseHandle(hub);
    }
    SetupDiDestroyDeviceInfoList(hubs);
    return ports;
}

// A USB, USB4 or Thunderbolt device: devices below one are connected through it.
bool IsUsbConnection(const DeviceRecord& device) {
    const std::wstring instance = Lower(device.instanceId);
    const std::wstring searchable = Lower(device.friendlyName + L" " + device.instanceId + L" " + device.hardwareId);
    return Lower(device.className) == L"usb" || instance.rfind(L"usb\\", 0) == 0 || instance.rfind(L"usb4\\", 0) == 0 ||
           searchable.find(L"thunderbolt") != std::wstring::npos || searchable.find(L"usb4") != std::wstring::npos;
}

bool IsInterestingDevice(const DeviceRecord& device) {
    const std::wstring className = Lower(device.className);
    const std::wstring searchable = Lower(device.instanceId + L" " + device.hardwareId + L" " + device.friendlyName);
    return className == L"usb" || className == L"monitor" || className == L"display" ||
           searchable.find(L"thunderbolt") != std::wstring::npos ||
           searchable.find(L"usb4") != std::wstring::npos;
}

// ------------------------------------------------------------------ displays

} // namespace

std::wstring InterfacePathToInstanceId(std::wstring path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        path.erase(0, 4);
    }
    const size_t guid = path.find(L"#{");
    if (guid != std::wstring::npos) {
        path.resize(guid);
    }
    std::replace(path.begin(), path.end(), L'#', L'\\');
    std::transform(path.begin(), path.end(), path.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    return path;
}

namespace {

std::wstring DeviceDescription(const std::wstring& instanceId) {
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return {};
    }
    for (ULONG property : { CM_DRP_FRIENDLYNAME, CM_DRP_DEVICEDESC }) {
        wchar_t buffer[256]{};
        ULONG size = sizeof(buffer);
        if (CM_Get_DevNode_Registry_PropertyW(node, property, nullptr, buffer, &size, 0) == CR_SUCCESS && buffer[0]) {
            return buffer;
        }
    }
    return {};
}

// The monitor's EDID block as Windows stored it under the device's hardware key ("Device Parameters").
std::vector<BYTE> ReadEdid(const std::wstring& instanceId) {
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return {};
    }
    HKEY key = nullptr;
    if (CM_Open_DevNode_Key(node, KEY_READ, 0, RegDisposition_OpenExisting, &key, CM_REGISTRY_HARDWARE) != CR_SUCCESS) {
        return {};
    }
    std::vector<BYTE> edid(1024);
    DWORD size = static_cast<DWORD>(edid.size());
    DWORD type = 0;
    const LSTATUS result = RegQueryValueExW(key, L"EDID", nullptr, &type, edid.data(), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_BINARY) {
        return {};
    }
    edid.resize(size);
    return edid;
}

} // namespace

void ParseEdid(const std::vector<BYTE>& edid, DisplayRecord& record) {
    static const BYTE header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    if (edid.size() < 128 || !std::equal(std::begin(header), std::end(header), edid.begin())) {
        return;
    }
    const unsigned manufacturer = (edid[8] << 8) | edid[9];
    const wchar_t letters[4] = { static_cast<wchar_t>(L'A' - 1 + ((manufacturer >> 10) & 31)),
                                 static_cast<wchar_t>(L'A' - 1 + ((manufacturer >> 5) & 31)),
                                 static_cast<wchar_t>(L'A' - 1 + (manufacturer & 31)), 0 };
    wchar_t id[16]{};
    swprintf_s(id, L"%s %02X%02X", letters, edid[11], edid[10]);
    record.edidId = id;

    std::wstring name;
    std::wstring serialText;
    for (size_t offset : { 54, 72, 90, 108 }) {
        if (edid[offset] != 0 || edid[offset + 1] != 0 || edid[offset + 2] != 0) {
            continue;  // a detailed timing descriptor, not a text descriptor
        }
        std::wstring text;
        for (size_t i = offset + 5; i < offset + 18 && edid[i] != 0x0A && edid[i] != 0; ++i) {
            text += static_cast<wchar_t>(edid[i]);
        }
        text.erase(text.find_last_not_of(L' ') + 1);
        if (edid[offset + 3] == 0xFC) {
            name = text;
        } else if (edid[offset + 3] == 0xFF) {
            serialText = text;
        }
    }
    const unsigned serialNumber = edid[12] | (edid[13] << 8) | (edid[14] << 16) | (static_cast<unsigned>(edid[15]) << 24);
    record.edidSerial = !serialText.empty() ? serialText : serialNumber != 0 ? std::to_wstring(serialNumber) : L"";
    if (record.monitorName.empty()) {
        record.monitorName = name;
    }
    const unsigned year = 1990u + edid[17];
    if (edid[16] == 0xFF) {
        record.edidManufactured = L"model year " + std::to_wstring(year);
    } else if (edid[16] != 0) {
        record.edidManufactured = std::to_wstring(year) + L" week " + std::to_wstring(edid[16]);
    } else {
        record.edidManufactured = std::to_wstring(year);
    }
}

namespace {

std::wstring ConnectionName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology) {
    switch (technology) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return L"VGA";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SVIDEO:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPOSITE_VIDEO:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPONENT_VIDEO:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_D_JPN:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDTVDONGLE: return L"Analog TV";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return L"DVI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return L"HDMI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS: return L"Internal (LVDS)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDI: return L"SDI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return L"DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return L"Internal (eDP)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EXTERNAL: return L"UDI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED: return L"Internal (UDI)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return L"Miracast (wireless)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED: return L"Indirect display (USB, e.g. DisplayLink)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL: return L"Virtual display";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_USB_TUNNEL: return L"DisplayPort over USB tunnel";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return L"Internal";
    default: return L"Other";
    }
}

DWORD BitsPerPixel(DISPLAYCONFIG_PIXELFORMAT format) {
    switch (format) {
    case DISPLAYCONFIG_PIXELFORMAT_8BPP: return 8;
    case DISPLAYCONFIG_PIXELFORMAT_16BPP: return 16;
    case DISPLAYCONFIG_PIXELFORMAT_24BPP: return 24;
    case DISPLAYCONFIG_PIXELFORMAT_32BPP: return 32;
    default: return 0;
    }
}

UINT32 RotationDegrees(DISPLAYCONFIG_ROTATION rotation) {
    switch (rotation) {
    case DISPLAYCONFIG_ROTATION_ROTATE90: return 90;
    case DISPLAYCONFIG_ROTATION_ROTATE180: return 180;
    case DISPLAYCONFIG_ROTATION_ROTATE270: return 270;
    default: return 0;
    }
}

// Windows display scaling of a source (\\.\DISPLAYn) in percent. Correct only in a per-monitor DPI aware
// process (the app manifest declares PerMonitorV2); 0 when the source has no monitor handle.
UINT32 ScalePercent(const std::wstring& sourceName) {
    struct Search {
        const std::wstring* name;
        HMONITOR found;
    } search{ &sourceName, nullptr };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto* search = reinterpret_cast<Search*>(data);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) && *search->name == info.szDevice) {
            search->found = monitor;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    UINT dpiX = 0;
    UINT dpiY = 0;
    if (!search.found || FAILED(GetDpiForMonitor(search.found, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        return 0;
    }
    return (dpiX * 100 + 48) / 96;
}

// Fills the monitor and adapter fields that do not depend on the path being active.
void DescribeTarget(const DISPLAYCONFIG_PATH_INFO& path, DisplayRecord& record,
                    std::map<std::pair<DWORD, LONG>, std::wstring>& adapterNames) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
        record.monitorName = target.monitorFriendlyDeviceName;
        record.monitorInstanceId = InterfacePathToInstanceId(target.monitorDevicePath);
        record.connection = ConnectionName(target.outputTechnology);
    } else {
        record.connection = ConnectionName(path.targetInfo.outputTechnology);
    }
    if (!record.monitorInstanceId.empty()) {
        record.edid = ReadEdid(record.monitorInstanceId);
        ParseEdid(record.edid, record);
    }

    const auto adapterKey = std::make_pair(path.targetInfo.adapterId.LowPart, path.targetInfo.adapterId.HighPart);
    auto adapter = adapterNames.find(adapterKey);
    if (adapter == adapterNames.end()) {
        DISPLAYCONFIG_ADAPTER_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = path.targetInfo.adapterId;
        std::wstring description;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS) {
            description = DeviceDescription(InterfacePathToInstanceId(name.adapterDevicePath));
        }
        adapter = adapterNames.emplace(adapterKey, description).first;
    }
    record.adapterName = adapter->second;

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
    color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    color.header.size = sizeof(color);
    color.header.adapterId = path.targetInfo.adapterId;
    color.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) {
        record.advancedColorSupported = color.advancedColorSupported != 0;
        record.advancedColorEnabled = color.advancedColorEnabled != 0;
        record.bitsPerColorChannel = color.bitsPerColorChannel;
    }
}

void CaptureDisplays(std::vector<DisplayRecord>& displays) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 5 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
            return;
        }
        paths.resize(pathCount);
        modes.resize(modeCount);
        result = QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        paths.resize(pathCount);
        modes.resize(modeCount);
    }
    if (result != ERROR_SUCCESS) {
        return;
    }

    std::map<std::pair<DWORD, LONG>, std::wstring> adapterNames;
    std::set<std::tuple<DWORD, LONG, UINT32>> seenTargets;
    auto targetKey = [](const DISPLAYCONFIG_PATH_INFO& path) {
        return std::make_tuple(path.targetInfo.adapterId.LowPart, path.targetInfo.adapterId.HighPart, path.targetInfo.id);
    };

    // Active paths: one per monitor that is part of the desktop.
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
            continue;
        }
        seenTargets.insert(targetKey(path));
        DisplayRecord record;
        record.active = true;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) {
            record.sourceName = source.viewGdiDeviceName;
            record.scalePercent = ScalePercent(record.sourceName);
        }
        if (path.sourceInfo.modeInfoIdx < modes.size() &&
            modes[path.sourceInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const DISPLAYCONFIG_SOURCE_MODE& mode = modes[path.sourceInfo.modeInfoIdx].sourceMode;
            record.pixelWidth = static_cast<LONG>(mode.width);
            record.pixelHeight = static_cast<LONG>(mode.height);
            record.bitsPerPixel = BitsPerPixel(mode.pixelFormat);
            record.primary = mode.position.x == 0 && mode.position.y == 0;
        }
        if (path.targetInfo.modeInfoIdx < modes.size() &&
            modes[path.targetInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
            record.pixelRate = modes[path.targetInfo.modeInfoIdx].targetMode.targetVideoSignalInfo.pixelRate;
        }
        record.refreshNumerator = path.targetInfo.refreshRate.Numerator;
        record.refreshDenominator = path.targetInfo.refreshRate.Denominator;
        record.rotation = RotationDegrees(path.targetInfo.rotation);
        DescribeTarget(path, record, adapterNames);
        displays.push_back(std::move(record));
    }
    std::sort(displays.begin(), displays.end(),
              [](const DisplayRecord& a, const DisplayRecord& b) { return a.sourceName < b.sourceName; });

    // Monitors that are connected but not part of the desktop. QDC_ALL_PATHS lists every possible
    // source/target combination, so keep each available target once.
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        if (!path.targetInfo.targetAvailable || !seenTargets.insert(targetKey(path)).second) {
            continue;
        }
        DisplayRecord record;
        DescribeTarget(path, record, adapterNames);
        displays.push_back(std::move(record));
    }
}

} // namespace

HardwareSnapshot CaptureSnapshot() {
    HardwareSnapshot snapshot;
    const std::map<std::wstring, UsbPortInfo> usbPorts = ReadUsbPorts(snapshot.usbPortProblems);
    HDEVINFO infoSet = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (infoSet == INVALID_HANDLE_VALUE) {
        return snapshot;
    }

    // First pass: the cheap properties and the parent of every present device.
    struct Candidate {
        DeviceRecord record;
        SP_DEVINFO_DATA data;
    };
    std::vector<Candidate> candidates;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(infoSet, index, &device); ++index) {
        Candidate candidate{ {}, device };
        DeviceRecord& record = candidate.record;
        record.className = ReadProperty(infoSet, device, SPDRP_CLASS);
        record.friendlyName = ReadProperty(infoSet, device, SPDRP_FRIENDLYNAME);
        if (record.friendlyName.empty()) {
            record.friendlyName = ReadProperty(infoSet, device, SPDRP_DEVICEDESC);
        }
        record.instanceId = ReadInstanceId(infoSet, device);
        record.hardwareId = ReadProperty(infoSet, device, SPDRP_HARDWAREID);
        record.parentInstanceId = ReadParentInstanceId(device);
        candidates.push_back(std::move(candidate));
        device = {};
        device.cbSize = sizeof(device);
    }

    // Keep the relevant devices themselves, plus every device connected through USB / USB4 / Thunderbolt
    // (a dock's network, audio, HID, storage, card reader functions). Not kept: software devices and the
    // devices of a Bluetooth radio, which only happens to sit on USB.
    std::map<std::wstring, const DeviceRecord*> byId;
    for (const Candidate& candidate : candidates) {
        byId[Lower(candidate.record.instanceId)] = &candidate.record;
    }
    auto behindUsb = [&](const DeviceRecord& record) {
        if (Lower(record.instanceId).rfind(L"swd\\", 0) == 0) {
            return false;
        }
        std::set<std::wstring> seen;
        for (auto parent = byId.find(Lower(record.parentInstanceId)); parent != byId.end();
             parent = byId.find(Lower(parent->second->parentInstanceId))) {
            const DeviceRecord& ancestor = *parent->second;
            if (!seen.insert(parent->first).second || Lower(ancestor.className) == L"bluetooth") {
                return false;
            }
            if (IsUsbConnection(ancestor)) {
                return true;
            }
        }
        return false;
    };

    // Decide for all devices first: the second pass moves records out, and behindUsb reads ancestors.
    std::vector<bool> keep(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        const bool interesting = IsInterestingDevice(candidates[i].record);
        keep[i] = interesting || behindUsb(candidates[i].record);
        candidates[i].record.behindUsb = keep[i] && !interesting;
    }

    // Second pass: everything else, only for the devices that are kept.
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (!keep[i]) {
            continue;
        }
        DeviceRecord& record = candidates[i].record;
        SP_DEVINFO_DATA& data = candidates[i].data;
        record.manufacturer = ReadProperty(infoSet, data, SPDRP_MFG);
        record.location = ReadProperty(infoSet, data, SPDRP_LOCATION_INFORMATION);
        record.service = ReadProperty(infoSet, data, SPDRP_SERVICE);
        record.status = ReadStatus(infoSet, data);
        record.driverProvider = ReadStringProperty(infoSet, data, DEVPKEY_Device_DriverProvider);
        record.driverVersion = ReadStringProperty(infoSet, data, DEVPKEY_Device_DriverVersion);
        record.driverDate = ReadTimeProperty(infoSet, data, DEVPKEY_Device_DriverDate, true);
        record.firmwareRevision = FirmwareRevision(record.hardwareId);
        record.lastArrival = ReadTimeProperty(infoSet, data, DEVPKEY_Device_LastArrivalDate, false);
        record.lastRemoval = ReadTimeProperty(infoSet, data, DEVPKEY_Device_LastRemovalDate, false);
        record.powerState = ReadPowerState(infoSet, data);
        record.powerSaving = ReadPowerSaving(infoSet, data);
        record.firmwareVersion = ReadStringProperty(infoSet, data, DEVPKEY_Device_FirmwareVersion);
        if (Lower(record.instanceId).rfind(L"usb\\", 0) == 0) {
            // The USB product string. Other buses report class descriptions here, which are no better.
            record.reportedName = ReadStringProperty(infoSet, data, DEVPKEY_Device_BusReportedDeviceDesc);
            record.reportedName.erase(record.reportedName.find_last_not_of(L' ') + 1);
        }
        const std::vector<BYTE> container = ReadDeviceProperty(infoSet, data, DEVPKEY_Device_ContainerId, DEVPROP_TYPE_GUID);
        if (container.size() >= sizeof(GUID)) {
            wchar_t text[64]{};
            StringFromGUID2(*reinterpret_cast<const GUID*>(container.data()), text, ARRAYSIZE(text));
            // {00000000-0000-0000-FFFF-FFFFFFFFFFFF} is Windows' "no container" (built-in devices).
            if (Lower(text) != L"{00000000-0000-0000-ffff-ffffffffffff}") {
                record.containerId = text;
            }
        }
        const auto port = usbPorts.find(Lower(ReadProperty(infoSet, data, SPDRP_DRIVER)));
        if (port != usbPorts.end()) {
            record.usbPort = L"port " + std::to_wstring(port->second.port);
            record.usbSpeed = port->second.speed;
            record.usbCapability = port->second.capability;
            record.usbPortStatus = port->second.status;
        }
        snapshot.devices.push_back(std::move(record));
    }

    SetupDiDestroyDeviceInfoList(infoSet);

    // Entries without their own product name (HID interfaces, audio functions, ...) get the name of the
    // nearest ancestor belonging to the same physical device (same container). Windows' container display
    // name is not used: a dock's container can be named after one of its functions, e.g. its network adapter.
    std::map<std::wstring, const DeviceRecord*> kept;
    for (const DeviceRecord& record : snapshot.devices) {
        kept[Lower(record.instanceId)] = &record;
    }
    for (DeviceRecord& record : snapshot.devices) {
        if (!record.reportedName.empty() || record.containerId.empty()) {
            continue;
        }
        std::set<std::wstring> seen;
        for (auto parent = kept.find(Lower(record.parentInstanceId));
             parent != kept.end() && parent->second->containerId == record.containerId && seen.insert(parent->first).second;
             parent = kept.find(Lower(parent->second->parentInstanceId))) {
            if (!parent->second->reportedName.empty()) {
                record.nameHint = parent->second->reportedName;
                break;
            }
        }
    }

    CaptureDisplays(snapshot.displays);
    snapshot.power = CapturePowerStatus();
    return snapshot;
}
bool DeviceWatcher::Start(HWND window) {
    Stop();

    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

    const GUID interfaces[] = {GUID_DEVINTERFACE_USB_DEVICE, GUID_DEVINTERFACE_MONITOR};
    for (const GUID& interfaceClass : interfaces) {
        filter.dbcc_classguid = interfaceClass;
        HDEVNOTIFY notification = RegisterDeviceNotificationW(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
        if (notification == nullptr) {
            Stop();
            return false;
        }
        notificationHandles_.push_back(notification);
    }
    return true;
}

void DeviceWatcher::Stop() {
    for (HDEVNOTIFY notification : notificationHandles_) {
        UnregisterDeviceNotification(notification);
    }
    notificationHandles_.clear();
}
