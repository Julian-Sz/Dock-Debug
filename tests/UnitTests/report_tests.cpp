// Tests for src/report.cpp: labels, display values, change signatures, the WHAT CHANGED diff and session
// numbering. All inputs are made up; nothing here touches the hardware or the log folder.

#include "test.h"

#include "report.h"

#include <cmath>

namespace {

DeviceRecord Device(const std::wstring& id, const std::wstring& name, const std::wstring& className = L"USB",
                    const std::wstring& parent = L"") {
    DeviceRecord device;
    device.instanceId = id;
    device.friendlyName = name;
    device.className = className;
    device.parentInstanceId = parent;
    device.status = L"0x180200A";
    return device;
}

DisplayRecord Display(const std::wstring& name, const std::wstring& instanceId) {
    DisplayRecord display;
    display.monitorName = name;
    display.monitorInstanceId = instanceId;
    display.sourceName = L"\\\\.\\DISPLAY1";
    display.connection = L"DisplayPort";
    display.active = true;
    display.pixelWidth = 3840;
    display.pixelHeight = 2160;
    display.refreshNumerator = 60000;
    display.refreshDenominator = 1000;
    display.bitsPerPixel = 32;
    display.scalePercent = 150;
    return display;
}

HardwareSnapshot Snapshot() {
    HardwareSnapshot snapshot;
    snapshot.devices.push_back(Device(L"USB\\ROOT_HUB30\\1", L"USB Root Hub (USB 3.0)"));
    snapshot.devices.push_back(Device(L"USB\\VID_1B1C&PID_1B5D\\1", L"USB Composite Device", L"USB", L"USB\\ROOT_HUB30\\1"));
    snapshot.displays.push_back(Display(L"LG ULTRAFINE", L"DISPLAY\\GSM5CBC\\1"));
    snapshot.power.onAc = true;
    return snapshot;
}

} // namespace

// ------------------------------------------------------------------ labels

TEST_CASE(DeviceLabel_puts_the_reported_name_first) {
    DeviceRecord device = Device(L"USB\\VID_1B1C&PID_1B5D\\1", L"USB Composite Device");
    device.reportedName = L"CORSAIR IRONCLAW RGB Gaming Mouse";
    CHECK_EQ(DeviceLabel(device), L"CORSAIR IRONCLAW RGB Gaming Mouse (USB Composite Device)");
}

TEST_CASE(DeviceLabel_does_not_repeat_an_equal_reported_name) {
    DeviceRecord device = Device(L"USB\\VID_5986&PID_1193\\1", L"FHD Camera");
    device.reportedName = L"fhd camera";
    CHECK_EQ(DeviceLabel(device), L"FHD Camera");
}

TEST_CASE(DeviceLabel_adds_the_name_hint_after_a_dot) {
    DeviceRecord device = Device(L"HID\\VID_1B1C&PID_1B5D&MI_00\\1", L"HID-compliant mouse", L"Mouse");
    device.nameHint = L"CORSAIR IRONCLAW RGB Gaming Mouse";
    CHECK_EQ(DeviceLabel(device), L"HID-compliant mouse · CORSAIR IRONCLAW RGB Gaming Mouse");
}

TEST_CASE(DeviceLabel_names_unnamed_devices) {
    CHECK_EQ(DeviceLabel(Device(L"USB\\X\\1", L"", L"USB")), L"(unnamed USB device)");
    CHECK_EQ(DeviceLabel(Device(L"USB\\X\\1", L"", L"")), L"(unnamed device)");
}

TEST_CASE(DisplayLabel_falls_back_for_panels_without_a_name) {
    DisplayRecord display = Display(L"", L"DISPLAY\\AUO7DB2\\1");
    display.connection = L"Internal (eDP)";
    CHECK_EQ(DisplayLabel(display), L"Built-in display");
    display.connection = L"HDMI";
    display.edidId = L"ACR 0123";
    CHECK_EQ(DisplayLabel(display), L"Unknown display (ACR 0123)");
    CHECK_EQ(DisplayLabel(Display(L"BenQ PD2705UA", L"DISPLAY\\BNQ8050\\1")), L"BenQ PD2705UA");
}

TEST_CASE(IsThunderbolt_and_IsUsbFamily_classify_devices) {
    CHECK(IsThunderbolt(Device(L"USB4\\ROOT_DEVICE_ROUTER&VID_8086\\1", L"USB4 Root Router")));
    CHECK(IsThunderbolt(Device(L"PCI\\VEN_8086\\1", L"Thunderbolt(TM) Controller", L"System")));
    CHECK(!IsThunderbolt(Device(L"USB\\VID_1B1C\\1", L"USB Composite Device")));
    CHECK(IsUsbFamily(Device(L"USB\\VID_1B1C\\1", L"USB Composite Device", L"HIDClass")));
    CHECK(!IsUsbFamily(Device(L"DISPLAY\\GSM5CBC\\1", L"Generic Monitor", L"Monitor")));
}

// ------------------------------------------------------------------ display values

TEST_CASE(DisplayMode_uses_the_exact_refresh_fraction) {
    DisplayRecord display = Display(L"LG", L"DISPLAY\\GSM\\1");
    display.refreshNumerator = 60000;
    display.refreshDenominator = 1001;
    CHECK_EQ(DisplayMode(display), L"3840 x 2160 @ 59.94 Hz");
    display.refreshNumerator = 240001;
    display.refreshDenominator = 1000;
    CHECK_EQ(DisplayMode(display), L"3840 x 2160 @ 240.001 Hz");
    display.active = false;
    CHECK_EQ(DisplayMode(display), L"not active");
}

TEST_CASE(DisplayBandwidth_is_pixel_clock_times_bits_per_pixel) {
    DisplayRecord display = Display(L"LG", L"DISPLAY\\GSM\\1");
    display.pixelRate = 533250000;
    display.bitsPerColorChannel = 8;
    CHECK(std::abs(DisplayBandwidthGbps(display) - 12.798) < 0.001);
    CHECK_CONTAINS(DisplayBandwidthText(display), L"12.8 Gbit/s (533.25 MHz × 24 bit)");
    display.bitsPerColorChannel = 10;
    CHECK(std::abs(DisplayBandwidthGbps(display) - 15.9975) < 0.001);
    display.bitsPerColorChannel = 0;  // unknown: 8 bits per channel assumed
    CHECK(std::abs(DisplayBandwidthGbps(display) - 12.798) < 0.001);
    display.active = false;
    CHECK_EQ(DisplayBandwidthGbps(display), 0.0);
    CHECK_EQ(DisplayBandwidthText(display), L"");
}

TEST_CASE(FormatGbps_picks_the_unit) {
    CHECK_EQ(FormatGbps(25.1), L"25 Gbit/s");
    CHECK_EQ(FormatGbps(5.0), L"5.0 Gbit/s");
    CHECK_EQ(FormatGbps(0.48), L"480 Mbit/s");
    CHECK_EQ(FormatGbps(0.0015), L"1.5 Mbit/s");
}

// ------------------------------------------------------------------ change signatures

TEST_CASE(UsbSignature_changes_with_tracked_fields_only) {
    const HardwareSnapshot base = Snapshot();
    const std::wstring signature = UsbSignature(base);

    HardwareSnapshot changed = base;
    changed.devices[1].status = L"0x1802400 (problem 43)";
    CHECK(UsbSignature(changed) != signature);
    changed = base;
    changed.devices[1].lastArrival = L"2026-01-31 14:05:08";
    CHECK(UsbSignature(changed) != signature);
    changed = base;
    changed.devices[1].usbSpeed = L"High Speed (USB 2.0, 480 Mbit/s)";
    CHECK(UsbSignature(changed) != signature);
    changed = base;
    changed.usbPortProblems.push_back({ L"USB\\ROOT_HUB30\\1", 2, L"overcurrent" });
    CHECK(UsbSignature(changed) != signature);

    // Power states flip whenever idle devices suspend; they must not create change files.
    changed = base;
    changed.devices[1].powerState = L"D3 (off / suspended)";
    changed.devices[1].powerSaving = L"allowed";
    CHECK_EQ(UsbSignature(changed), signature);
}

TEST_CASE(DisplaySignature_changes_with_tracked_fields_only) {
    const HardwareSnapshot base = Snapshot();
    const std::wstring signature = DisplaySignature(base);

    HardwareSnapshot changed = base;
    changed.displays[0].refreshNumerator = 30000;
    CHECK(DisplaySignature(changed) != signature);
    changed = base;
    changed.displays[0].scalePercent = 125;
    CHECK(DisplaySignature(changed) != signature);
    changed = base;
    changed.displays[0].advancedColorEnabled = true;
    CHECK(DisplaySignature(changed) != signature);

    changed = base;
    changed.displays[0].edidSerial = L"123";
    changed.displays[0].pixelRate = 1;
    CHECK_EQ(DisplaySignature(changed), signature);
}

// ------------------------------------------------------------------ WHAT CHANGED

TEST_CASE(FormatChanges_reports_no_change) {
    const std::wstring text = FormatChanges(Snapshot(), Snapshot());
    CHECK_CONTAINS(text, L"Displays: no change");
    CHECK_CONTAINS(text, L"Devices: no change");
    CHECK_CONTAINS(text, L"USB port problems: no change");
    CHECK_CONTAINS(text, L"Power: no change");
}

TEST_CASE(FormatChanges_reports_removed_and_added_devices) {
    HardwareSnapshot after = Snapshot();
    after.devices.pop_back();
    after.devices.push_back(Device(L"USB\\VID_0BDA&PID_8156\\1", L"Realtek USB NIC", L"Net", L"USB\\ROOT_HUB30\\1"));
    const std::wstring text = FormatChanges(Snapshot(), after);
    CHECK_CONTAINS(text, L"removed: USB Composite Device  [USB\\VID_1B1C&PID_1B5D\\1]  parent USB\\ROOT_HUB30\\1");
    CHECK_CONTAINS(text, L"added: Realtek USB NIC  [USB\\VID_0BDA&PID_8156\\1]  parent USB\\ROOT_HUB30\\1, status 0x180200A");
}

TEST_CASE(FormatChanges_reports_changed_fields_and_re_arrival) {
    HardwareSnapshot before = Snapshot();
    before.devices[1].lastArrival = L"2026-01-31 14:00:00";
    HardwareSnapshot after = before;
    after.devices[1].status = L"0x1802400 (problem 43)";
    after.devices[1].lastArrival = L"2026-01-31 14:05:08";
    after.devices[1].usbSpeed = L"High Speed (USB 2.0, 480 Mbit/s)";
    const std::wstring text = FormatChanges(before, after);
    CHECK_CONTAINS(text, L"changed: USB Composite Device  [USB\\VID_1B1C&PID_1B5D\\1]");
    CHECK_CONTAINS(text, L"status: 0x180200A -> 0x1802400 (problem 43)");
    CHECK_CONTAINS(text, L"re-arrived: last arrival 2026-01-31 14:00:00 -> 2026-01-31 14:05:08");
    CHECK_CONTAINS(text, L"usb speed: none -> High Speed (USB 2.0, 480 Mbit/s)");
}

TEST_CASE(FormatChanges_reports_display_changes) {
    HardwareSnapshot after = Snapshot();
    after.displays[0].refreshNumerator = 30000;
    after.displays[0].primary = true;
    after.displays.push_back(Display(L"BenQ PD2705UA", L"DISPLAY\\BNQ8050\\1"));
    const std::wstring text = FormatChanges(Snapshot(), after);
    CHECK_CONTAINS(text, L"changed: LG ULTRAFINE  [DISPLAY\\GSM5CBC\\1]");
    CHECK_CONTAINS(text, L"mode: 3840 x 2160 @ 60 Hz -> 3840 x 2160 @ 30 Hz");
    CHECK_CONTAINS(text, L"primary: no -> yes");
    CHECK_CONTAINS(text, L"added: BenQ PD2705UA  [DISPLAY\\BNQ8050\\1]");

    const std::wstring removed = FormatChanges(after, Snapshot());
    CHECK_CONTAINS(removed, L"removed: BenQ PD2705UA  [DISPLAY\\BNQ8050\\1]");
}

TEST_CASE(FormatChanges_reports_port_problems_and_power) {
    HardwareSnapshot after = Snapshot();
    after.usbPortProblems.push_back({ L"USB\\ROOT_HUB30\\1", 2, L"overcurrent" });
    after.power.onAc = false;
    std::wstring text = FormatChanges(Snapshot(), after);
    CHECK_CONTAINS(text, L"new: hub USB\\ROOT_HUB30\\1, port 2: overcurrent");
    CHECK_CONTAINS(text, L"power source: AC power -> battery");

    text = FormatChanges(after, Snapshot());
    CHECK_CONTAINS(text, L"gone: hub USB\\ROOT_HUB30\\1, port 2: overcurrent");
}

// ------------------------------------------------------------------ session numbering

TEST_CASE(NextSessionNumber_starts_at_one) {
    CHECK_EQ(NextSessionNumber({}), 1);
    CHECK_EQ(NextSessionNumber({ L"about-dock-debug-logs.md", L"2026-01-31_14-05-09-250_usb-tree_change.log" }), 1);
}

TEST_CASE(NextSessionNumber_compares_numerically) {
    CHECK_EQ(NextSessionNumber({ L"s1_2026_x.log", L"s2_2026_x.log" }), 3);
    CHECK_EQ(NextSessionNumber({ L"s9_2026_x.log", L"s10_2026_x.log", L"s2_2026_x.log" }), 11);
    CHECK_EQ(NextSessionNumber({ L"S4_2026_x.LOG" }), 5);
}

TEST_CASE(NextSessionNumber_ignores_other_names) {
    CHECK_EQ(NextSessionNumber({ L"session_x.log", L"s_x.log", L"s12a_x.log", L"s500_x.txt", L"s1234567_x.log",
                                 L"s-3_x.log", L"x5_y.log" }),
             1);
}
