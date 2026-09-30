#include "traffic.h"

#include <winsock2.h>
#include <windows.h>
#include <netioapi.h>
#include <setupapi.h>
#include <winioctl.h>
#include <initguid.h>
#include <ntddstor.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "setupapi.lib")

namespace {

std::wstring BusName(STORAGE_BUS_TYPE bus) {
    switch (bus) {
    case BusTypeUsb: return L"USB";
    case BusTypeNvme: return L"NVMe";
    case BusTypeSata: return L"SATA";
    case BusTypeAta: return L"ATA";
    case BusTypeScsi: return L"SCSI";
    case BusTypeSas: return L"SAS";
    case BusTypeSd: return L"SD card";
    case BusTypeMmc: return L"MMC";
    case BusTypeRAID: return L"RAID";
    case BusType1394: return L"FireWire";
    case BusTypeVirtual: return L"virtual";
    case BusTypeFileBackedVirtual: return L"virtual (file)";
    case BusTypeSpaces: return L"Storage Spaces";
    default: return L"other (" + std::to_wstring(static_cast<int>(bus)) + L")";
    }
}

void ReadNetworkAdapters(std::vector<TrafficCounter>& counters) {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR) {
        return;
    }
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        // One row per physical adapter: skip virtual interfaces and the filter layers stacked on adapters.
        if (!row.InterfaceAndOperStatusFlags.HardwareInterface || row.InterfaceAndOperStatusFlags.FilterInterface) {
            continue;
        }
        TrafficCounter counter;
        counter.key = L"net:" + std::to_wstring(row.InterfaceLuid.Value);
        counter.name = row.Description;
        counter.kind = L"Network adapter";
        counter.connected = row.MediaConnectState == MediaConnectStateConnected;
        counter.detail = counter.connected ? L"link " + std::to_wstring(row.ReceiveLinkSpeed / 1000000) + L" Mbit/s"
                                           : L"not connected";
        counter.received = row.InOctets;
        counter.sent = row.OutOctets;
        counters.push_back(std::move(counter));
    }
    FreeMibTable(table);
}

void ReadDrives(std::vector<TrafficCounter>& counters) {
    HDEVINFO disks = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (disks == INVALID_HANDLE_VALUE) {
        return;
    }
    SP_DEVICE_INTERFACE_DATA diskInterface{};
    diskInterface.cbSize = sizeof(diskInterface);
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(disks, nullptr, &GUID_DEVINTERFACE_DISK, index, &diskInterface); ++index) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(disks, &diskInterface, nullptr, 0, &required, nullptr);
        std::vector<BYTE> buffer(required + sizeof(wchar_t));
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (required == 0 || !SetupDiGetDeviceInterfaceDetailW(disks, &diskInterface, detail, required, nullptr, &device)) {
            continue;
        }
        wchar_t instanceId[512]{};
        SetupDiGetDeviceInstanceIdW(disks, &device, instanceId, ARRAYSIZE(instanceId), nullptr);
        wchar_t name[256]{};
        if (!SetupDiGetDeviceRegistryPropertyW(disks, &device, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<BYTE*>(name),
                                               sizeof(name) - sizeof(wchar_t), nullptr)) {
            SetupDiGetDeviceRegistryPropertyW(disks, &device, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<BYTE*>(name),
                                              sizeof(name) - sizeof(wchar_t), nullptr);
        }

        // No access rights needed for these two queries.
        HANDLE disk = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (disk == INVALID_HANDLE_VALUE) {
            continue;
        }
        DWORD bytes = 0;
        DISK_PERFORMANCE performance{};
        const bool havePerformance = DeviceIoControl(disk, IOCTL_DISK_PERFORMANCE, nullptr, 0, &performance,
                                                     sizeof(performance), &bytes, nullptr) != FALSE;
        STORAGE_PROPERTY_QUERY query{ StorageDeviceProperty, PropertyStandardQuery };
        BYTE descriptor[1024]{};
        const bool haveDescriptor = DeviceIoControl(disk, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), descriptor,
                                                    sizeof(descriptor), &bytes, nullptr) != FALSE;
        CloseHandle(disk);
        if (!havePerformance) {
            continue;
        }
        TrafficCounter counter;
        counter.key = L"disk:" + std::wstring(instanceId);
        counter.name = name;
        counter.kind = L"Drive";
        counter.instanceId = instanceId;
        counter.detail = haveDescriptor ? L"bus: " + BusName(reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(descriptor)->BusType)
                                        : L"";
        counter.received = static_cast<unsigned long long>(performance.BytesRead.QuadPart);
        counter.sent = static_cast<unsigned long long>(performance.BytesWritten.QuadPart);
        counters.push_back(std::move(counter));
    }
    SetupDiDestroyDeviceInfoList(disks);
}

} // namespace

std::vector<TrafficCounter> ReadTrafficCounters() {
    std::vector<TrafficCounter> counters;
    ReadNetworkAdapters(counters);
    ReadDrives(counters);
    return counters;
}
