// Tests for the parsing helpers in src/monitor.cpp and the event filters in src/system.cpp, plus one smoke
// test that runs the real data collection on the current machine.

#include "test.h"

#include "monitor.h"
#include "system.h"

namespace {

// A 128-byte EDID base block: "DEL" product 0xA1B2, serial 12345, week 12 of 2023, with a monitor name
// descriptor and optionally a serial number descriptor.
std::vector<BYTE> MakeEdid(bool textSerial) {
    std::vector<BYTE> edid(128, 0);
    const BYTE header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    std::copy(std::begin(header), std::end(header), edid.begin());
    edid[8] = 0x10;  // "DEL": (4 << 10) | (5 << 5) | 12 = 0x10AC
    edid[9] = 0xAC;
    edid[10] = 0xB2;  // product code 0xA1B2, little endian
    edid[11] = 0xA1;
    edid[12] = 0x39;  // serial number 12345 = 0x3039, little endian
    edid[13] = 0x30;
    edid[16] = 12;    // week
    edid[17] = 33;    // 1990 + 33 = 2023
    auto descriptor = [&](size_t offset, BYTE tag, const char* text) {
        edid[offset + 3] = tag;
        size_t i = offset + 5;
        for (; *text && i < offset + 18; ++text, ++i) {
            edid[i] = static_cast<BYTE>(*text);
        }
        if (i < offset + 18) {
            edid[i++] = 0x0A;
        }
        for (; i < offset + 18; ++i) {
            edid[i] = 0x20;
        }
    };
    edid[54] = 0x01;  // first descriptor: a detailed timing (not text), must be skipped
    descriptor(72, 0xFC, "DELL U2723QE");
    if (textSerial) {
        descriptor(90, 0xFF, "ABC123");
    }
    return edid;
}

} // namespace

// ------------------------------------------------------------------ monitor.cpp

TEST_CASE(ParseEdid_reads_id_name_serial_and_date) {
    DisplayRecord display;
    ParseEdid(MakeEdid(true), display);
    CHECK_EQ(display.edidId, L"DEL A1B2");
    CHECK_EQ(display.monitorName, L"DELL U2723QE");
    CHECK_EQ(display.edidSerial, L"ABC123");
    CHECK_EQ(display.edidManufactured, L"2023 week 12");
}

TEST_CASE(ParseEdid_uses_the_numeric_serial_without_a_text_serial) {
    DisplayRecord display;
    ParseEdid(MakeEdid(false), display);
    CHECK_EQ(display.edidSerial, L"12345");
}

TEST_CASE(ParseEdid_keeps_an_existing_name_and_handles_model_years) {
    DisplayRecord display;
    display.monitorName = L"Name from Windows";
    std::vector<BYTE> edid = MakeEdid(true);
    edid[16] = 0xFF;  // week 255: the year is a model year
    ParseEdid(edid, display);
    CHECK_EQ(display.monitorName, L"Name from Windows");
    CHECK_EQ(display.edidManufactured, L"model year 2023");
}

TEST_CASE(ParseEdid_ignores_invalid_data) {
    DisplayRecord display;
    std::vector<BYTE> edid = MakeEdid(true);
    edid[0] = 0x12;
    ParseEdid(edid, display);
    ParseEdid(std::vector<BYTE>(64, 0), display);
    ParseEdid({}, display);
    CHECK_EQ(display.edidId, L"");
    CHECK_EQ(display.monitorName, L"");
}

TEST_CASE(FirmwareRevision_extracts_the_REV_part) {
    CHECK_EQ(FirmwareRevision(L"USB\\VID_1B1C&PID_1B5D&REV_0324"), L"0324");
    CHECK_EQ(FirmwareRevision(L"PCI\\VEN_8086&DEV_5781&SUBSYS_00008086&REV_84"), L"84");
    CHECK_EQ(FirmwareRevision(L"USB\\VID_1B1C&PID_1B5D&REV_0324&MI_00"), L"0324");
    CHECK_EQ(FirmwareRevision(L"USB\\ROOT_HUB30"), L"");
}

TEST_CASE(InterfacePathToInstanceId_converts_monitor_paths) {
    CHECK_EQ(InterfacePathToInstanceId(L"\\\\?\\DISPLAY#DELA1B2#5&2d6a5c3e&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}"),
             L"DISPLAY\\DELA1B2\\5&2D6A5C3E&0&UID4352");
    CHECK_EQ(InterfacePathToInstanceId(L"\\\\?\\PCI#VEN_10DE&DEV_2F18#4&1b2c#{5b45201d-f2f2-4f3b-85bb-30ff1f953599}"),
             L"PCI\\VEN_10DE&DEV_2F18\\4&1B2C");
}

// ------------------------------------------------------------------ system.cpp

TEST_CASE(IsRelevantSystemProvider_keeps_hardware_providers) {
    CHECK(IsRelevantSystemProvider(L"Microsoft-Windows-Kernel-Power"));
    CHECK(IsRelevantSystemProvider(L"Microsoft-Windows-Kernel-PnP"));
    CHECK(IsRelevantSystemProvider(L"Display"));
    CHECK(IsRelevantSystemProvider(L"nvlddmkm"));
    CHECK(IsRelevantSystemProvider(L"Microsoft-Windows-USB-USBXHCI"));
    CHECK(IsRelevantSystemProvider(L"Microsoft-Windows-WHEA-Logger"));
    CHECK(IsRelevantSystemProvider(L"Microsoft-Windows-WindowsUpdateClient"));
    CHECK(!IsRelevantSystemProvider(L"Microsoft-Windows-Time-Service"));
    CHECK(!IsRelevantSystemProvider(L"Service Control Manager"));
}

TEST_CASE(IsNoise_drops_routine_messages) {
    CHECK(IsNoise(L"Device STORAGE\\VolumeSnapshot\\HarddiskVolumeSnapshot1 was surprise removed."));
    CHECK(IsNoise(L"Installation successful: Security Intelligence Update for Microsoft Defender Antivirus - KB2267602 (Version 1.4)"));
    CHECK(IsNoise(L"Installation successful: 9MW2LKJ0TPJF-Microsoft.NET.Native.Framework.2.2"));
    CHECK(!IsNoise(L"Installation failed: Intel - System - 2.2.10204.8 with error 0x80240016"));
    CHECK(!IsNoise(L"Installation successful: 2026-09 Security Update (KB5129195) (26200.9457)"));
    CHECK(!IsNoise(L"Device USB\\VID_1B1C&PID_1B5D\\0701C013 was surprise removed."));
}

TEST_CASE(StopCodeTitle_normalizes_and_names_codes) {
    CHECK_EQ(StopCodeTitle(L"116"), L"0x116 VIDEO_TDR_FAILURE (graphics driver did not recover from a timeout)");
    CHECK_EQ(StopCodeTitle(L"0x00000116"), StopCodeTitle(L"116"));
    CHECK_EQ(StopCodeTitle(L"7E"), L"0x7e SYSTEM_THREAD_EXCEPTION_NOT_HANDLED");
    CHECK_EQ(StopCodeTitle(L"144"), L"0x144 BUGCODE_USB3_DRIVER (USB 3 controller or hub problem)");
    CHECK_EQ(StopCodeTitle(L"abc"), L"0xabc");
    CHECK_EQ(StopCodeTitle(L"0"), L"0x0");
}

// ------------------------------------------------------------------ smoke test on this machine

TEST_CASE(Smoke_real_data_collection_runs) {
    const HardwareSnapshot snapshot = CaptureSnapshot();
    for (const DeviceRecord& device : snapshot.devices) {
        CHECK(!device.instanceId.empty());
        CHECK_EQ(device.instanceId, InterfacePathToInstanceId(device.instanceId));  // stored in upper case
    }
    CHECK(!CapturePowerStatus().summary.empty());
    const ULONGLONG now = UtcNow();
    const WindowsEventList events = ReadWindowsEvents(now - 60 * UtcSecond, now, 10);
    CHECK(events.events.size() <= 10);
    CaptureSystemInfo();
}
