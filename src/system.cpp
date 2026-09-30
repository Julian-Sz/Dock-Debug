#include "system.h"

#include <windows.h>
#include <batclass.h>
#include <powrprof.h>
#include <setupapi.h>
#include <winevt.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "wevtapi.lib")

namespace {

std::wstring RegistryString(const wchar_t* key, const wchar_t* name) {
    wchar_t buffer[256]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return {};
    }
    std::wstring value = buffer;
    value.erase(value.find_last_not_of(L' ') + 1);
    return value;
}

// Power setting GUIDs (see "powercfg /q"). Spelled out because the SDK only declares some of them.
constexpr GUID UsbSubgroup = { 0x2a737441, 0x1930, 0x4402, { 0x8d, 0x77, 0xb2, 0xbe, 0xbb, 0xa3, 0x08, 0xa3 } };
constexpr GUID UsbSelectiveSuspend = { 0x48e6b7a6, 0x50f5, 0x4782, { 0xa5, 0xd4, 0x53, 0xbb, 0x8f, 0x07, 0xe2, 0x26 } };
constexpr GUID PcieSubgroup = { 0x501a4d13, 0x42af, 0x4429, { 0x9f, 0xd1, 0xa8, 0x21, 0x8c, 0x26, 0x8e, 0x20 } };
constexpr GUID PcieLinkStatePowerManagement = { 0xee12f906, 0xd277, 0x404b, { 0xb6, 0xda, 0xe5, 0xfa, 0x1a, 0x57, 0x6d, 0xf5 } };

// Reads a power setting of the active plan; returns false when the plan does not contain it.
bool ReadPowerSetting(const GUID* scheme, const GUID& subgroup, const GUID& setting, bool ac, DWORD& value) {
    return (ac ? PowerReadACValueIndex(nullptr, scheme, &subgroup, &setting, &value)
               : PowerReadDCValueIndex(nullptr, scheme, &subgroup, &setting, &value)) == ERROR_SUCCESS;
}

std::wstring OnOff(bool known, DWORD value) {
    return !known ? L"not set in this power plan" : value != 0 ? L"on" : L"off";
}

std::wstring PcieLinkPower(bool known, DWORD value) {
    if (!known) {
        return L"not set in this power plan";
    }
    switch (value) {
    case 0: return L"off";
    case 1: return L"moderate power savings";
    case 2: return L"maximum power savings";
    default: return L"value " + std::to_wstring(value);
    }
}

std::wstring FormatUtc(ULONGLONG time, bool iso) {
    FILETIME file{ static_cast<DWORD>(time), static_cast<DWORD>(time >> 32) };
    SYSTEMTIME utc{};
    FileTimeToSystemTime(&file, &utc);
    wchar_t text[40]{};
    if (iso) {
        swprintf_s(text, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
                   utc.wMinute, utc.wSecond, utc.wMilliseconds);
    } else {
        SYSTEMTIME local{};
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
        swprintf_s(text, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", local.wYear, local.wMonth, local.wDay, local.wHour,
                   local.wMinute, local.wSecond, local.wMilliseconds);
    }
    return text;
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

} // namespace

bool IsRelevantSystemProvider(const std::wstring& provider) {
    static const wchar_t* const keywords[] = {
        L"kernel-power", L"power-troubleshooter",       // sleep, wake, power loss, AC/battery switches
        L"kernel-pnp", L"userpnp",                      // driver load failures and installs
        L"display", L"dxg",                             // display driver timeouts (event 4101) and GPU kernel
        L"nvlddmkm", L"amdkmdag", L"amdwddmg", L"igfx",  // NVIDIA, AMD and Intel GPU drivers
        L"usb", L"xhci", L"ucm", L"hid", L"thunderbolt",  // USB controllers, hubs, Type-C, Thunderbolt
        L"whea",                                        // PCIe / hardware errors (Thunderbolt tunnels PCIe)
        L"wer-systemerrorreporting", L"bugcheck",       // blue screens
        L"eventlog",                                    // unexpected shutdowns (6008)
        L"windowsupdateclient",                         // driver and system updates
    };
    const std::wstring name = Lower(provider);
    return std::any_of(std::begin(keywords), std::end(keywords),
                       [&](const wchar_t* keyword) { return name.find(keyword) != std::wstring::npos; });
}

namespace {

std::wstring LevelName(BYTE level) {
    switch (level) {
    case 1: return L"Critical";
    case 2: return L"Error";
    case 3: return L"Warning";
    case 5: return L"Verbose";
    default: return L"Information";
    }
}

class EventFormatter {
public:
    EventFormatter() : context_(EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem)) {}
    ~EventFormatter() {
        for (auto& [name, publisher] : publishers_) {
            if (publisher) {
                EvtClose(publisher);
            }
        }
        if (context_) {
            EvtClose(context_);
        }
    }
    EventFormatter(const EventFormatter&) = delete;
    EventFormatter& operator=(const EventFormatter&) = delete;

    // Reads the system properties; false when the event cannot be rendered.
    bool Render(EVT_HANDLE event, WindowsEvent& result) {
        DWORD used = 0;
        DWORD count = 0;
        EvtRender(context_, event, EvtRenderEventValues, 0, nullptr, &used, &count);
        buffer_.resize(used);
        if (used == 0 || !EvtRender(context_, event, EvtRenderEventValues, used, buffer_.data(), &used, &count)) {
            return false;
        }
        const auto* values = reinterpret_cast<const EVT_VARIANT*>(buffer_.data());
        auto text = [&](int index) {
            return values[index].Type == EvtVarTypeString && values[index].StringVal ? std::wstring(values[index].StringVal)
                                                                                     : std::wstring();
        };
        result.provider = text(EvtSystemProviderName);
        result.channel = text(EvtSystemChannel);
        result.id = values[EvtSystemEventID].Type == EvtVarTypeUInt16 ? values[EvtSystemEventID].UInt16Val : 0;
        result.level = LevelName(values[EvtSystemLevel].Type == EvtVarTypeByte ? values[EvtSystemLevel].ByteVal : 4);
        result.time = values[EvtSystemTimeCreated].Type == EvtVarTypeFileTime ? values[EvtSystemTimeCreated].FileTimeVal : 0;
        result.recordId = values[EvtSystemEventRecordId].Type == EvtVarTypeUInt64 ? values[EvtSystemEventRecordId].UInt64Val : 0;
        result.timeText = FormatUtc(result.time, false);
        return true;
    }

    std::wstring Message(EVT_HANDLE event, const std::wstring& provider) {
        auto publisher = publishers_.find(provider);
        if (publisher == publishers_.end()) {
            publisher = publishers_.emplace(provider, EvtOpenPublisherMetadata(nullptr, provider.c_str(), nullptr, 0, 0)).first;
        }
        if (!publisher->second) {
            return L"(no message text available)";
        }
        DWORD used = 0;
        EvtFormatMessage(publisher->second, event, 0, 0, nullptr, EvtFormatMessageEvent, 0, nullptr, &used);
        std::wstring message(used, L'\0');
        if (used == 0 ||
            !EvtFormatMessage(publisher->second, event, 0, 0, nullptr, EvtFormatMessageEvent, used, message.data(), &used)) {
            // Messages with unresolved inserts still come back as text; anything else has none.
            if (GetLastError() != ERROR_EVT_UNRESOLVED_VALUE_INSERT || message.empty()) {
                return L"(no message text available)";
            }
        }
        // One line: collapse line breaks, tabs and repeated spaces.
        std::wstring flat;
        for (wchar_t c : message) {
            if (c == L'\0') {
                break;
            }
            const bool space = std::iswspace(c) != 0;
            if (space && (flat.empty() || flat.back() == L' ')) {
                continue;
            }
            flat += space ? L' ' : c;
        }
        flat.erase(flat.find_last_not_of(L' ') + 1);
        return flat;
    }

private:
    EVT_HANDLE context_;
    std::vector<BYTE> buffer_;
    std::map<std::wstring, EVT_HANDLE> publishers_;
};

} // namespace

ULONGLONG UtcNow() {
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    return (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

namespace {

// GUID_DEVCLASS_BATTERY, which is also the battery device interface class.
constexpr GUID BatteryClass = { 0x72631e54, 0x78a4, 0x11d0, { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a } };

std::wstring Watts(long milliwatts) {
    wchar_t text[32]{};
    swprintf_s(text, L"%.1f", milliwatts / 1000.0);
    return text;
}

// Charge, rate and capacity of the first system battery, via the battery class driver
// (IOCTL_BATTERY_QUERY_STATUS / _INFORMATION). Empty when there is no battery.
std::wstring BatteryDetails(bool& discharging) {
    discharging = false;
    std::wstring result;
    HDEVINFO batteries = SetupDiGetClassDevsW(&BatteryClass, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (batteries == INVALID_HANDLE_VALUE) {
        return result;
    }
    SP_DEVICE_INTERFACE_DATA battery{};
    battery.cbSize = sizeof(battery);
    for (DWORD index = 0; result.empty() && SetupDiEnumDeviceInterfaces(batteries, nullptr, &BatteryClass, index, &battery); ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(batteries, &battery, nullptr, 0, &required, nullptr);
        std::vector<BYTE> buffer(required + sizeof(wchar_t));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (required == 0 || !SetupDiGetDeviceInterfaceDetailW(batteries, &battery, detail, required, nullptr, nullptr)) {
            continue;
        }
        HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }
        DWORD bytes = 0;
        DWORD wait = 0;
        BATTERY_QUERY_INFORMATION query{};
        BATTERY_INFORMATION information{};
        BATTERY_WAIT_STATUS waitStatus{};
        BATTERY_STATUS status{};
        query.InformationLevel = BatteryInformation;
        if (DeviceIoControl(handle, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof(wait), &query.BatteryTag, sizeof(query.BatteryTag), &bytes, nullptr) &&
            query.BatteryTag != 0 &&
            DeviceIoControl(handle, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query), &information, sizeof(information), &bytes, nullptr) &&
            (information.Capabilities & BATTERY_SYSTEM_BATTERY) != 0) {
            waitStatus.BatteryTag = query.BatteryTag;
            if (DeviceIoControl(handle, IOCTL_BATTERY_QUERY_STATUS, &waitStatus, sizeof(waitStatus), &status, sizeof(status), &bytes, nullptr)) {
                const long rate = static_cast<long>(status.Rate);
                const bool rateKnown = status.Rate != BATTERY_UNKNOWN_RATE && rate != 0;
                discharging = (status.PowerState & BATTERY_DISCHARGING) != 0 || (rateKnown && rate < 0);
                if ((status.PowerState & BATTERY_CHARGING) != 0 || (rateKnown && rate > 0)) {
                    result = rateKnown ? L"charging at " + Watts(rate) + L" W" : L"charging";
                } else if (discharging) {
                    result = rateKnown ? L"discharging at " + Watts(-rate) + L" W" : L"discharging";
                } else {
                    result = L"neither charging nor discharging";
                }
                if (status.Voltage != BATTERY_UNKNOWN_VOLTAGE && status.Voltage != 0) {
                    result += L", " + Watts(static_cast<long>(status.Voltage)) + L" V";
                }
                if ((information.Capabilities & BATTERY_CAPACITY_RELATIVE) == 0 && status.Capacity != BATTERY_UNKNOWN_CAPACITY) {
                    result += L" · " + Watts(static_cast<long>(status.Capacity)) + L" Wh now, " +
                              Watts(static_cast<long>(information.FullChargedCapacity)) + L" Wh full, " +
                              Watts(static_cast<long>(information.DesignedCapacity)) + L" Wh design";
                }
            }
        }
        CloseHandle(handle);
    }
    SetupDiDestroyDeviceInfoList(batteries);
    return result;
}

} // namespace

PowerStatus CapturePowerStatus() {
    PowerStatus status;
    SYSTEM_POWER_STATUS power{};
    if (!GetSystemPowerStatus(&power)) {
        status.summary = L"unknown";
        return status;
    }
    status.onAc = power.ACLineStatus == 1;
    status.summary = power.ACLineStatus == 1 ? L"AC power" : power.ACLineStatus == 0 ? L"Battery" : L"unknown power source";
    const bool hasBattery = (power.BatteryFlag & 128) == 0 && power.BatteryFlag != 255;
    if (!hasBattery) {
        return status;
    }
    if (power.BatteryLifePercent <= 100) {
        status.summary += L" · battery " + std::to_wstring(power.BatteryLifePercent) + L" %";
    }
    bool discharging = false;
    const std::wstring details = BatteryDetails(discharging);
    if (!details.empty()) {
        status.summary += L", " + details;
    }
    status.drainingOnAc = status.onAc && discharging;
    if (status.drainingOnAc) {
        status.summary += L" · DRAINING ON AC POWER: the power supply delivers less than the computer uses";
    }
    return status;
}

SystemInfo CaptureSystemInfo() {
    SystemInfo info;
    const wchar_t* bios = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    info.manufacturer = RegistryString(bios, L"SystemManufacturer");
    info.model = RegistryString(bios, L"SystemProductName");
    info.biosVersion = RegistryString(bios, L"BIOSVersion");
    const std::wstring biosDate = RegistryString(bios, L"BIOSReleaseDate");
    if (!biosDate.empty()) {
        info.biosVersion += L" (" + biosDate + L")";
    }

    info.power = CapturePowerStatus();

    SYSTEM_POWER_CAPABILITIES capabilities{};
    if (GetPwrCapabilities(&capabilities)) {
        info.sleepModel = capabilities.AoAc ? L"Modern Standby (S0 low power idle)"
                        : capabilities.SystemS3 ? L"S3 sleep"
                                                : L"no sleep state reported";
    }

    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) == ERROR_SUCCESS) {
        wchar_t name[256]{};
        DWORD size = sizeof(name);
        if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, reinterpret_cast<UCHAR*>(name), &size) == ERROR_SUCCESS) {
            info.powerPlan = name;
        }
        DWORD value = 0;
        bool known = ReadPowerSetting(scheme, UsbSubgroup, UsbSelectiveSuspend, true, value);
        info.usbSelectiveSuspendAc = OnOff(known, value);
        info.usbSelectiveSuspendOn = known && value != 0;
        known = ReadPowerSetting(scheme, UsbSubgroup, UsbSelectiveSuspend, false, value);
        info.usbSelectiveSuspendDc = OnOff(known, value);
        info.usbSelectiveSuspendOn = info.usbSelectiveSuspendOn || (known && value != 0);
        known = ReadPowerSetting(scheme, PcieSubgroup, PcieLinkStatePowerManagement, true, value);
        info.pcieLinkPowerAc = PcieLinkPower(known, value);
        known = ReadPowerSetting(scheme, PcieSubgroup, PcieLinkStatePowerManagement, false, value);
        info.pcieLinkPowerDc = PcieLinkPower(known, value);
        LocalFree(scheme);
    }
    return info;
}



bool IsNoise(const std::wstring& message) {
    if (message.find(L"VolumeSnapshot") != std::wstring::npos) {
        return true;  // shadow copy volumes come and go with backups and restore points
    }
    if (message.find(L"KB2267602") != std::wstring::npos) {
        return true;  // Microsoft Defender security intelligence updates, several per day
    }
    // Microsoft Store app updates are named after the 12-character Store ID, e.g. "9MW2LKJ0TPJF-Microsoft...".
    for (size_t i = 0; i + 13 <= message.size(); ++i) {
        if (message[i] == L'9' && (i == 0 || !std::iswalnum(message[i - 1])) && message[i + 12] == L'-' &&
            std::all_of(message.begin() + i, message.begin() + i + 12,
                        [](wchar_t c) { return std::iswdigit(c) || (c >= L'A' && c <= L'Z'); })) {
            return true;
        }
    }
    return false;
}

namespace {

bool IsKernelCrashReport(const std::wstring& message) {
    return message.find(L"BlueScreen") != std::wstring::npos || message.find(L"LiveKernelEvent") != std::wstring::npos;
}

// Runs an XPath query on one channel, newest first, and calls `handle` for every rendered event (with its
// message) until it returns false. Returns an error text, or an empty string on success.
template <typename Handler>
std::wstring QueryChannel(EventFormatter& formatter, const wchar_t* channel, const std::wstring& query, Handler handle) {
    EVT_HANDLE results = EvtQuery(nullptr, channel, query.c_str(), EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!results) {
        const DWORD error = GetLastError();
        return std::wstring(channel) + (error == ERROR_ACCESS_DENIED ? L": access denied"
                                                                     : L": not available (error " + std::to_wstring(error) + L")");
    }
    EVT_HANDLE batch[64];
    DWORD returned = 0;
    bool more = true;
    while (more && EvtNext(results, ARRAYSIZE(batch), batch, INFINITE, 0, &returned)) {
        for (DWORD i = 0; i < returned; ++i) {
            WindowsEvent event;
            if (more && formatter.Render(batch[i], event)) {
                if (event.channel.empty()) {
                    event.channel = channel;
                }
                more = handle(batch[i], event);
            }
            EvtClose(batch[i]);
        }
    }
    EvtClose(results);
    return {};
}

std::wstring TimeFilter(ULONGLONG fromUtc, ULONGLONG toUtc) {
    return L"TimeCreated[@SystemTime>'" + FormatUtc(fromUtc, true) + L"' and @SystemTime<='" + FormatUtc(toUtc, true) + L"']";
}

// Names of the stop codes that matter for docks, USB and displays (hex, lower case, no leading zeros).
std::wstring StopCodeName(const std::wstring& code) {
    static const std::map<std::wstring, const wchar_t*> names = {
        { L"a", L"IRQL_NOT_LESS_OR_EQUAL" },
        { L"1a", L"MEMORY_MANAGEMENT" },
        { L"3b", L"SYSTEM_SERVICE_EXCEPTION" },
        { L"50", L"PAGE_FAULT_IN_NONPAGED_AREA" },
        { L"7e", L"SYSTEM_THREAD_EXCEPTION_NOT_HANDLED" },
        { L"9f", L"DRIVER_POWER_STATE_FAILURE" },
        { L"d1", L"DRIVER_IRQL_NOT_LESS_OR_EQUAL" },
        { L"ef", L"CRITICAL_PROCESS_DIED" },
        { L"fe", L"BUGCODE_USB_DRIVER" },
        { L"116", L"VIDEO_TDR_FAILURE (graphics driver did not recover from a timeout)" },
        { L"117", L"VIDEO_TDR_TIMEOUT_DETECTED (graphics driver timeout, recovered)" },
        { L"124", L"WHEA_UNCORRECTABLE_ERROR (hardware error)" },
        { L"133", L"DPC_WATCHDOG_VIOLATION (a driver blocked the system too long)" },
        { L"141", L"VIDEO_ENGINE_TIMEOUT_DETECTED (graphics engine timeout)" },
        { L"144", L"BUGCODE_USB3_DRIVER (USB 3 controller or hub problem)" },
        { L"15c", L"PDC_WATCHDOG_TIMEOUT_LIVEDUMP (Modern Standby watchdog)" },
        { L"15f", L"CONNECTED_STANDBY_WATCHDOG_TIMEOUT_LIVEDUMP (Modern Standby watchdog)" },
        { L"193", L"VIDEO_DXGKRNL_LIVEDUMP (graphics kernel diagnostic dump)" },
        { L"1d4", L"UCMUCSI_LIVEDUMP (USB Type-C connector problem)" },
    };
    const auto found = names.find(code);
    return found == names.end() ? std::wstring() : found->second;
}

} // namespace

std::wstring StopCodeTitle(std::wstring code) {
    code = Lower(code);
    if (code.rfind(L"0x", 0) == 0) {
        code.erase(0, 2);
    }
    code.erase(0, std::min(code.find_first_not_of(L'0'), code.size() - 1));
    const std::wstring name = StopCodeName(code);
    return L"0x" + code + (name.empty() ? L"" : L" " + name);
}

namespace {

// The text after `label` up to the next space, e.g. "P1: 116" -> "116".
std::wstring Token(const std::wstring& message, const wchar_t* label) {
    const size_t start = message.find(label);
    if (start == std::wstring::npos) {
        return {};
    }
    const size_t begin = start + wcslen(label);
    const size_t end = message.find(L' ', begin);
    return message.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
}

// The first .dmp file named in a WER report, without the \\?\ prefix.
std::wstring DumpFile(const std::wstring& message) {
    const size_t dmp = message.find(L".dmp");
    if (dmp == std::wstring::npos) {
        return {};
    }
    const size_t space = message.rfind(L' ', dmp);
    std::wstring path = message.substr(space == std::wstring::npos ? 0 : space + 1, dmp + 4 - (space == std::wstring::npos ? 0 : space + 1));
    if (path.rfind(L"\\\\?\\", 0) == 0) {
        path.erase(0, 4);
    }
    return path;
}

std::wstring Shorten(const std::wstring& text, size_t length) {
    return text.size() <= length ? text : text.substr(0, length) + L"...";
}

} // namespace

WindowsEventList ReadWindowsEvents(ULONGLONG fromUtc, ULONGLONG toUtc, size_t maxEvents) {
    enum class Filter { None, SystemProviders, KernelCrashReports };
    struct Channel {
        const wchar_t* path;
        const wchar_t* condition;  // extra XPath condition inside System[...]
        Filter filter;
    };
    static const Channel channels[] = {
        { L"System", nullptr, Filter::SystemProviders },
        { L"Microsoft-Windows-Kernel-PnP/Device Management", nullptr, Filter::None },
        { L"Microsoft-Windows-Kernel-PnP/Configuration", nullptr, Filter::None },
        // Mostly routine information; only its warnings and errors (failed driver updates) are kept.
        { L"Microsoft-Windows-DeviceSetupManager/Admin", L"(Level=1 or Level=2 or Level=3)", Filter::None },
        { L"Application", L"Provider[@Name='Windows Error Reporting'] and EventID=1001", Filter::KernelCrashReports },
    };

    WindowsEventList result;
    EventFormatter formatter;
    std::vector<WindowsEvent> collected;
    for (const Channel& channel : channels) {
        const std::wstring query = L"*[System[" + (channel.condition ? std::wstring(channel.condition) + L" and " : L"") +
                                   TimeFilter(fromUtc, toUtc) + L"]]";
        size_t taken = 0;
        const std::wstring error = QueryChannel(formatter, channel.path, query, [&](EVT_HANDLE handle, WindowsEvent& event) {
            if (channel.filter == Filter::SystemProviders && !IsRelevantSystemProvider(event.provider)) {
                return true;
            }
            event.message = formatter.Message(handle, event.provider);
            if (IsNoise(event.message) || (channel.filter == Filter::KernelCrashReports && !IsKernelCrashReport(event.message))) {
                return true;
            }
            if (taken < maxEvents) {
                // Newest first, so the newest maxEvents per log are kept.
                collected.push_back(std::move(event));
                ++taken;
            } else {
                ++result.omitted;
            }
            return true;
        });
        if (!error.empty()) {
            result.error += (result.error.empty() ? L"" : L"; ") + error;
        }
    }

    std::sort(collected.begin(), collected.end(),
              [](const WindowsEvent& a, const WindowsEvent& b) { return a.time > b.time; });
    if (collected.size() > maxEvents) {
        result.omitted += collected.size() - maxEvents;
        collected.resize(maxEvents);
    }
    std::reverse(collected.begin(), collected.end());
    result.events = std::move(collected);
    return result;
}

ReliabilityHistory ReadReliabilityHistory(unsigned days) {
    ReliabilityHistory history;
    EventFormatter formatter;
    const ULONGLONG now = UtcNow();
    const std::wstring time = TimeFilter(now - static_cast<ULONGLONG>(days) * 24 * 60 * 60 * UtcSecond, now);

    struct Group {
        HistoryEntry entry;
        std::set<std::wstring> days;
        int order = 0;
    };
    std::map<std::pair<std::wstring, std::wstring>, Group> groups;
    auto add = [&](int order, const std::wstring& category, const std::wstring& title, const WindowsEvent& event,
                   const std::wstring& detail) {
        Group& group = groups[{ category, title }];
        if (group.entry.count == 0) {  // newest first: the first report seen is the latest
            group.entry.category = category;
            group.entry.title = title;
            group.entry.last = event.timeText.substr(0, 19);
            group.entry.detail = detail;
            group.order = order;
        }
        group.entry.first = event.timeText.substr(0, 19);
        ++group.entry.count;
        group.days.insert(event.timeText.substr(0, 10));
    };
    auto report = [&](const std::wstring& error) {
        if (!error.empty()) {
            history.error += (history.error.empty() ? L"" : L"; ") + error;
        }
    };

    // Windows Error Reporting: blue screens and kernel live dumps (driver hangs Windows recovered from).
    report(QueryChannel(formatter, L"Application",
                        L"*[System[Provider[@Name='Windows Error Reporting'] and EventID=1001 and " + time + L"]]",
                        [&](EVT_HANDLE handle, WindowsEvent& event) {
                            const std::wstring message = formatter.Message(handle, event.provider);
                            const bool blueScreen = message.find(L"BlueScreen") != std::wstring::npos;
                            if (blueScreen || message.find(L"LiveKernelEvent") != std::wstring::npos) {
                                const std::wstring dump = DumpFile(message);
                                add(blueScreen ? 0 : 1, blueScreen ? L"Blue screen" : L"Kernel live dump (driver hang, no blue screen)",
                                    StopCodeTitle(Token(message, L"P1: ")), event, dump.empty() ? L"" : L"dump: " + dump);
                            }
                            return true;
                        }));

    // Unexpected shutdowns and blue screens as logged at the next boot.
    report(QueryChannel(formatter, L"System",
                        L"*[System[((Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=41) or "
                        L"(Provider[@Name='EventLog'] and EventID=6008) or "
                        L"(Provider[@Name='Microsoft-Windows-WER-SystemErrorReporting'] and EventID=1001)) and " + time + L"]]",
                        [&](EVT_HANDLE handle, WindowsEvent& event) {
                            const std::wstring message = formatter.Message(handle, event.provider);
                            if (event.id == 1001) {
                                const size_t code = message.find(L"0x");
                                add(0, L"Blue screen", StopCodeTitle(code == std::wstring::npos ? L"?" : message.substr(code, 10)),
                                    event, L"logged at restart: " + Shorten(message, 200));
                            } else {
                                add(2, L"Unexpected shutdown",
                                    event.id == 41 ? L"Kernel-Power 41: restarted without shutting down cleanly (crash, hang or power loss)"
                                                   : L"EventLog 6008: the previous shutdown was unexpected",
                                    event, L"");
                            }
                            return true;
                        }));

    // Driver installs and Windows Update results.
    report(QueryChannel(formatter, L"System",
                        L"*[System[((Provider[@Name='Microsoft-Windows-UserPnp'] and (EventID=20001 or EventID=20003)) or "
                        L"(Provider[@Name='Microsoft-Windows-WindowsUpdateClient'] and (EventID=19 or EventID=20))) and " + time + L"]]",
                        [&](EVT_HANDLE handle, WindowsEvent& event) {
                            const std::wstring message = formatter.Message(handle, event.provider);
                            if (IsNoise(message)) {
                                return true;
                            }
                            if (event.provider == L"Microsoft-Windows-UserPnp") {
                                add(3, L"Driver installed", Shorten(message, 300), event, L"");
                            } else {
                                add(event.id == 20 ? 4 : 5, event.id == 20 ? L"Windows Update failed" : L"Windows Update installed",
                                    Shorten(message, 300), event, L"");
                            }
                            return true;
                        }));

    std::vector<Group*> sorted;
    for (auto& [key, group] : groups) {
        group.entry.days = group.days.size();
        sorted.push_back(&group);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Group* a, const Group* b) {
        return a->order != b->order ? a->order < b->order : a->entry.last > b->entry.last;
    });
    for (const Group* group : sorted) {
        history.entries.push_back(group->entry);
    }
    return history;
}