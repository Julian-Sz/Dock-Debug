#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "DeviceNodeItem.h"
#include "../src/report.h"
#include "../src/system.h"
#include "../src/traffic.h"

#include <dbt.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace {

constexpr UINT_PTR DeviceChangeSubclassId = 1;

Style LookupStyle(wchar_t const* key) {
    return Application::Current().Resources().Lookup(box_value(key)).as<Style>();
}

TextBlock MakeText(std::wstring const& text, wchar_t const* style) {
    TextBlock block;
    block.Text(text);
    block.Style(LookupStyle(style));
    return block;
}

Border MakeTag(std::wstring const& text, wchar_t const* borderStyle, wchar_t const* textStyle) {
    Border tag;
    tag.Style(LookupStyle(borderStyle));
    tag.Child(MakeText(text, textStyle));
    return tag;
}

std::wstring TimeNow() {
    wchar_t buffer[64]{};
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, nullptr, nullptr, buffer, ARRAYSIZE(buffer)) == 0) {
        return {};
    }
    return buffer;
}

std::wstring Count(size_t count, std::wstring const& noun) {
    return std::to_wstring(count) + L" " + noun + (count == 1 ? L"" : L"s");
}

// Same design as the app icon (tools/make-icon.ps1): Feather Icons "zap" (MIT, see THIRD-PARTY-NOTICES.md)
// on a rounded blue tile. TitleBar crashes when given a PathIconSource, so it gets an SVG image instead.
constexpr wchar_t TitleBarIconSvg[] =
    L"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
    L"<defs><linearGradient id='tile' x1='0' y1='0' x2='1' y2='1'>"
    L"<stop offset='0' stop-color='#3AA0FF'/><stop offset='1' stop-color='#0054C4'/></linearGradient></defs>"
    L"<rect x='1.5' y='1.5' width='21' height='21' rx='5' fill='url(#tile)'/>"
    L"<polygon transform='translate(3.6 3.6) scale(0.7)' points='13 2 3 14 12 14 11 22 21 10 12 10 13 2'"
    L" fill='#fff' stroke='#fff' stroke-width='2' stroke-linejoin='round'/></svg>";

std::wstring OrDash(std::wstring const& value) {
    return value.empty() ? L"—" : value;
}

// Segoe Fluent Icons glyph for a device in the tree.
std::wstring DeviceGlyph(DeviceRecord const& device) {
    if (IsThunderbolt(device)) {
        return L"";  // lightning
    }
    if (!device.behindUsb) {
        return L"";  // USB
    }
    static const std::pair<wchar_t const*, wchar_t const*> glyphs[] = {
        { L"Net", L"" },        // Ethernet
        { L"Camera", L"" },
        { L"Image", L"" },
        { L"Keyboard", L"" },
        { L"Mouse", L"" },
        { L"MEDIA", L"" },      // speakers
        { L"AudioEndpoint", L"" },
        { L"DiskDrive", L"" },  // hard drive
        { L"Bluetooth", L"" },
    };
    for (auto const& [className, glyph] : glyphs) {
        if (device.className == className) {
            return glyph;
        }
    }
    return L"";  // generic device
}

// A colored tag for DeviceRecord::usbSpeed: blue SuperSpeed+ (10+ Gbit/s), green SuperSpeed (5 Gbit/s),
// gray USB 2 and lower. Null when the speed is unknown.
Border SpeedTag(std::wstring const& speed) {
    std::wstring text;
    wchar_t const* border = L"TagBorderStyle";
    wchar_t const* foreground = L"TagTextStyle";
    if (speed.rfind(L"SuperSpeed+", 0) == 0) {
        text = L"10+ Gbit/s";
        border = L"AccentTagBorderStyle";
        foreground = L"AccentTagTextStyle";
    } else if (speed.rfind(L"SuperSpeed", 0) == 0) {
        text = L"5 Gbit/s";
        border = L"SuccessTagBorderStyle";
        foreground = L"SuccessTagTextStyle";
    } else if (speed.rfind(L"High Speed", 0) == 0) {
        text = L"480 Mbit/s";
    } else if (speed.rfind(L"Full Speed", 0) == 0) {
        text = L"12 Mbit/s";
    } else if (speed.rfind(L"Low Speed", 0) == 0) {
        text = L"1.5 Mbit/s";
    } else {
        return nullptr;
    }
    Border tag = MakeTag(text, border, foreground);
    tag.MinWidth(88);  // same width for all, so the tags line up
    tag.Child().as<TextBlock>().HorizontalAlignment(HorizontalAlignment::Center);
    ToolTipService::SetToolTip(tag, box_value(L"Negotiated speed of this connection (its maximum, not its current use): " + speed));
    return tag;
}

// "12.3 MB/s" from bytes per second.
std::wstring FormatBytesPerSecond(double bytes) {
    wchar_t text[32]{};
    if (bytes >= 1e6) {
        swprintf_s(text, L"%.1f MB/s", bytes / 1e6);
    } else {
        swprintf_s(text, L"%.0f kB/s", bytes / 1e3);
    }
    return text;
}

bool IsUsb4Router(DeviceRecord const& device) {
    std::wstring id = device.instanceId;
    std::transform(id.begin(), id.end(), id.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    return id.rfind(L"USB4\\", 0) == 0 && id.find(L"VIRTUAL_POWER_PDO") == std::wstring::npos;
}

// Joins the non-empty parts with " · ".
std::wstring JoinDots(std::initializer_list<std::wstring> parts) {
    std::wstring result;
    for (std::wstring const& part : parts) {
        if (!part.empty()) {
            result += (result.empty() ? L"" : L" · ") + part;
        }
    }
    return result;
}

// A card with label / value rows, like a group in the Settings app.
Border MakeInfoCard(std::vector<std::pair<std::wstring, std::wstring>> const& rows) {
    Grid grid;
    grid.ColumnSpacing(24);
    grid.RowSpacing(8);
    ColumnDefinition labels;
    labels.Width(GridLength{ 1, GridUnitType::Auto });
    ColumnDefinition values;
    values.Width(GridLength{ 1, GridUnitType::Star });
    grid.ColumnDefinitions().Append(labels);
    grid.ColumnDefinitions().Append(values);
    for (size_t i = 0; i < rows.size(); ++i) {
        grid.RowDefinitions().Append(RowDefinition{});
        auto label = MakeText(rows[i].first, L"BodyTextBlockStyle");
        Grid::SetRow(label, static_cast<int32_t>(i));
        grid.Children().Append(label);
        auto value = MakeText(OrDash(rows[i].second), L"SecondaryBodyStyle");
        value.TextWrapping(TextWrapping::Wrap);
        value.IsTextSelectionEnabled(true);
        Grid::SetRow(value, static_cast<int32_t>(i));
        Grid::SetColumn(value, 1);
        grid.Children().Append(value);
    }
    Border card;
    card.Style(LookupStyle(L"CardBorderStyle"));
    card.Child(grid);
    return card;
}

} // namespace

namespace winrt::DockDebug::implementation
{
    void MainWindow::InitializeComponent()
    {
        MainWindowT::InitializeComponent();

        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());

        SetTitleBarIconAsync();

        this->try_as<::IWindowNative>()->get_WindowHandle(&m_hwnd);
        const UINT dpi = GetDpiForWindow(m_hwnd);

        // Taskbar / Alt+Tab icon from the embedded resource (app.rc); WinUI does not set one itself.
        for (auto [kind, metric] : { std::pair{ ICON_BIG, SM_CXICON }, std::pair{ ICON_SMALL, SM_CXSMICON } }) {
            const int size = GetSystemMetricsForDpi(metric, dpi);
            if (HANDLE icon = LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_SHARED)) {
                SendMessageW(m_hwnd, WM_SETICON, kind, reinterpret_cast<LPARAM>(icon));
            }
        }
        AppWindow().Resize({ MulDiv(1180, dpi, 96), MulDiv(760, dpi, 96) });
        if (auto presenter = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>()) {
            presenter.PreferredMinimumWidth(MulDiv(760, dpi, 96));
            presenter.PreferredMinimumHeight(MulDiv(520, dpi, 96));
        }

        RootNavigation().SelectedItem(RootNavigation().MenuItems().GetAt(0));
        LogFolderText().Text(TrackingDirectory());
        UpdateTrackingStatus();

        // DeviceWatcher registers for USB and monitor interface notifications on our HWND;
        // the subclass turns the resulting WM_DEVICECHANGE messages into an immediate rescan.
        SetWindowSubclass(m_hwnd, &MainWindow::DeviceChangeSubclass, DeviceChangeSubclassId,
                          reinterpret_cast<DWORD_PTR>(this));
        m_watcher.Start(m_hwnd);

        m_timer = DispatcherQueue().CreateTimer();
        m_timer.Interval(std::chrono::seconds(10));
        m_timer.Tick([weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get()) {
                self->RequestRefresh();
            }
        });
        m_timer.Start();

        Closed([this](auto&&, auto&&) { Shutdown(); });

        RequestRefresh();
    }

    winrt::fire_and_forget MainWindow::SetTitleBarIconAsync()
    {
        auto strong = get_strong();
        Windows::Storage::Streams::InMemoryRandomAccessStream stream;
        Windows::Storage::Streams::DataWriter writer(stream);
        writer.WriteString(TitleBarIconSvg);
        co_await writer.StoreAsync();
        writer.DetachStream();
        stream.Seek(0);

        Microsoft::UI::Xaml::Media::Imaging::SvgImageSource svg;
        ImageIconSource icon;
        icon.ImageSource(svg);
        AppTitleBar().IconSource(icon);
        co_await svg.SetSourceAsync(stream);
    }

    LRESULT CALLBACK MainWindow::DeviceChangeSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                                      UINT_PTR, DWORD_PTR data)
    {
        if ((message == WM_DEVICECHANGE && (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE ||
                                            wParam == DBT_DEVNODES_CHANGED)) ||
            message == WM_DISPLAYCHANGE) {
            reinterpret_cast<MainWindow*>(data)->RequestRefresh();
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void MainWindow::Shutdown()
    {
        m_closed = true;
        if (m_timer) {
            m_timer.Stop();
        }
        if (m_trafficTimer) {
            m_trafficTimer.Stop();
        }
        m_watcher.Stop();
        RemoveWindowSubclass(m_hwnd, &MainWindow::DeviceChangeSubclass, DeviceChangeSubclassId);
    }

    // ------------------------------------------------------------------ snapshots

    void MainWindow::RequestRefresh()
    {
        if (m_closed) {
            return;
        }
        if (m_scanning) {
            // Device changes arrive in bursts; collapse them into one follow-up scan.
            m_rescanPending = true;
            return;
        }
        RefreshAsync();
    }

    winrt::fire_and_forget MainWindow::RefreshAsync()
    {
        auto strong = get_strong();
        auto dispatcher = DispatcherQueue();
        m_scanning = true;
        ScanProgress().IsActive(true);
        RefreshButton().IsEnabled(false);

        co_await winrt::resume_background();
        HardwareSnapshot snapshot = CaptureSnapshot();
        co_await wil::resume_foreground(dispatcher);

        m_scanning = false;
        if (m_closed) {
            co_return;
        }
        ScanProgress().IsActive(false);
        RefreshButton().IsEnabled(true);
        ApplySnapshot(std::move(snapshot));

        if (m_rescanPending) {
            m_rescanPending = false;
            RefreshAsync();
        }
    }

    void MainWindow::ApplySnapshot(HardwareSnapshot snapshot)
    {
        m_snapshot = std::move(snapshot);
        m_hasSnapshot = true;
        LastUpdatedText().Text(L"Updated " + TimeNow());

        if (DisplaySignature(m_snapshot) != m_renderedDisplaySignature) {
            RebuildDisplays();
        }
        if (UsbSignature(m_snapshot) != m_renderedUsbSignature) {
            RebuildDeviceTree();
        }
        if (TopologyPage().Visibility() == Visibility::Visible) {
            RebuildTopology();
        }
        UpdateLiveStatus();

        MonitorToggle().IsEnabled(true);
        UsbToggle().IsEnabled(true);
        CheckTracking();
        UpdateTrackingStatus();
    }

    // ------------------------------------------------------------------ live view

    void MainWindow::UpdateLiveStatus()
    {
        const auto thunderbolt = std::count_if(m_snapshot.devices.begin(), m_snapshot.devices.end(),
                                               [](DeviceRecord const& device) { return IsThunderbolt(device); });
        std::wstring message = Count(m_snapshot.displays.size(), L"display") + L" · " +
                               Count(m_snapshot.devices.size(), L"tracked device") + L" · " +
                               Count(static_cast<size_t>(thunderbolt), L"Thunderbolt/USB4 device");
        if (!m_snapshot.usbPortProblems.empty()) {
            LiveStatus().Severity(InfoBarSeverity::Error);
            LiveStatus().Title(m_snapshot.usbPortProblems.size() == 1 ? L"A USB port reports a problem"
                                                                     : L"USB ports report problems");
            for (UsbPortProblem const& problem : m_snapshot.usbPortProblems) {
                message += L" · port " + std::to_wstring(problem.port) + L" of " + problem.hubInstanceId + L": " + problem.status;
            }
        } else if (m_snapshot.power.drainingOnAc) {
            LiveStatus().Severity(InfoBarSeverity::Warning);
            LiveStatus().Title(L"The battery drains although the computer is on AC power");
            message += L" · " + m_snapshot.power.summary;
        } else if (thunderbolt == 0) {
            LiveStatus().Severity(InfoBarSeverity::Warning);
            LiveStatus().Title(L"No Thunderbolt or USB4 devices detected");
        } else {
            LiveStatus().Severity(InfoBarSeverity::Informational);
            LiveStatus().Title(L"Connected hardware");
        }
        LiveStatus().Message(message);
    }

    void MainWindow::RebuildDisplays()
    {
        m_renderedDisplaySignature = DisplaySignature(m_snapshot);
        auto children = DisplayPanel().Children();
        children.Clear();

        if (m_snapshot.displays.empty()) {
            children.Append(MakeText(L"Windows did not report any displays.", L"SecondaryBodyStyle"));
            return;
        }

        for (DisplayRecord const& display : m_snapshot.displays) {
            Grid grid;
            grid.ColumnSpacing(16);
            for (GridUnitType unit : { GridUnitType::Auto, GridUnitType::Star, GridUnitType::Auto }) {
                ColumnDefinition column;
                column.Width(GridLength{ 1, unit });
                grid.ColumnDefinitions().Append(column);
            }

            FontIcon icon;
            icon.Glyph(display.connection.rfind(L"Internal", 0) == 0 ? L"" : L"");  // laptop / monitor
            icon.FontSize(20);
            icon.VerticalAlignment(VerticalAlignment::Center);
            grid.Children().Append(icon);

            StackPanel text;
            text.VerticalAlignment(VerticalAlignment::Center);
            text.Children().Append(MakeText(DisplayLabel(display), L"BodyStrongTextBlockStyle"));
            auto mode = MakeText(JoinDots({ DisplayMode(display),
                                            display.active ? std::to_wstring(display.bitsPerPixel) + L" bpp" : L"",
                                            display.scalePercent ? std::to_wstring(display.scalePercent) + L" % scaling" : L"",
                                            display.connection, display.adapterName }),
                                 L"SecondaryCaptionStyle");
            mode.TextTrimming(TextTrimming::CharacterEllipsis);
            text.Children().Append(mode);
            auto id = MakeText(JoinDots({ display.sourceName,
                                          display.edidId.empty() ? L"" : L"EDID " + display.edidId,
                                          display.edidSerial.empty() ? L"" : L"S/N " + display.edidSerial,
                                          display.monitorInstanceId }),
                               L"SecondaryCaptionStyle");
            id.TextTrimming(TextTrimming::CharacterEllipsis);
            id.IsTextSelectionEnabled(true);
            text.Children().Append(id);
            Grid::SetColumn(text, 1);
            grid.Children().Append(text);

            StackPanel tags;
            tags.Orientation(Orientation::Horizontal);
            tags.Spacing(6);
            tags.VerticalAlignment(VerticalAlignment::Center);
            if (display.advancedColorEnabled) {
                tags.Children().Append(MakeTag(L"HDR", L"TagBorderStyle", L"TagTextStyle"));
            }
            if (display.primary) {
                tags.Children().Append(MakeTag(L"Primary", L"AccentTagBorderStyle", L"AccentTagTextStyle"));
            }
            tags.Children().Append(display.active
                ? MakeTag(L"Active", L"SuccessTagBorderStyle", L"SuccessTagTextStyle")
                : MakeTag(L"Inactive", L"TagBorderStyle", L"TagTextStyle"));
            Grid::SetColumn(tags, 2);
            grid.Children().Append(tags);

            Border card;
            card.Style(LookupStyle(L"CardBorderStyle"));
            card.Child(grid);
            children.Append(card);
        }
    }

    void MainWindow::RebuildDeviceTree()
    {
        const bool firstBuild = m_renderedUsbSignature.empty();
        m_renderedUsbSignature = UsbSignature(m_snapshot);
        auto tree = DeviceTree();

        // Keep the user's expanded branches across rebuilds.
        std::set<std::wstring> expanded;
        std::function<void(Windows::Foundation::Collections::IVector<TreeViewNode> const&)> collect =
            [&](auto const& nodes) {
                for (TreeViewNode const& node : nodes) {
                    if (node.IsExpanded()) {
                        if (auto item = node.Content().try_as<DockDebug::DeviceNodeItem>()) {
                            expanded.insert(std::wstring(item.InstanceId()));
                        }
                    }
                    collect(node.Children());
                }
            };
        collect(tree.RootNodes());
        // Deselect before removing the nodes: clearing RootNodes while one of them is the selected node
        // corrupts TreeView's internal state and crashes the process a few rebuilds later (0xC0000005 /
        // 0xC0000374 inside Microsoft.UI.Xaml.Controls.dll). m_selectedInstanceId restores it below.
        tree.SelectedNode(nullptr);
        tree.RootNodes().Clear();

        // Create every node first, then attach each to its parent, so the tree does not depend on
        // parents being enumerated before their children.
        std::map<std::wstring, TreeViewNode> nodes;
        std::vector<DeviceRecord const*> devices;
        for (DeviceRecord const& device : m_snapshot.devices) {
            if (!IsUsbFamily(device) && !device.behindUsb) {
                continue;
            }
            TreeViewNode node;
            node.Content(make<DeviceNodeItem>(hstring{ DeviceLabel(device) }, hstring{ DeviceGlyph(device) },
                                              hstring{ device.instanceId }));
            nodes.emplace(device.instanceId, node);
            devices.push_back(&device);
        }
        for (DeviceRecord const* device : devices) {
            TreeViewNode node = nodes.at(device->instanceId);
            const auto parent = nodes.find(device->parentInstanceId);
            if (parent != nodes.end() && parent->first != device->instanceId) {
                parent->second.Children().Append(node);
            } else {
                tree.RootNodes().Append(node);
            }
        }
        for (auto const& [instanceId, node] : nodes) {
            if (node.Children().Size() > 0) {
                node.IsExpanded(firstBuild || expanded.count(instanceId) > 0);
            }
        }
        DeviceTreeEmpty().Visibility(devices.empty() ? Visibility::Visible : Visibility::Collapsed);

        const auto selected = nodes.find(m_selectedInstanceId);
        if (selected == nodes.end()) {
            ShowDeviceDetails(nullptr);
            return;
        }
        tree.SelectedNode(selected->second);
        for (DeviceRecord const* device : devices) {
            if (device->instanceId == m_selectedInstanceId) {
                ShowDeviceDetails(device);
            }
        }
    }

    void MainWindow::OnDeviceSelectionChanged(TreeView const&, TreeViewSelectionChangedEventArgs const& args)
    {
        if (args.AddedItems().Size() == 0) {
            return;
        }
        const auto node = args.AddedItems().GetAt(0).try_as<TreeViewNode>();
        const auto item = node ? node.Content().try_as<DockDebug::DeviceNodeItem>() : nullptr;
        if (!item) {
            return;
        }
        m_selectedInstanceId = item.InstanceId();
        const auto device = std::find_if(m_snapshot.devices.begin(), m_snapshot.devices.end(),
                                         [&](DeviceRecord const& record) { return record.instanceId == m_selectedInstanceId; });
        ShowDeviceDetails(device == m_snapshot.devices.end() ? nullptr : &*device);
    }

    void MainWindow::ShowDeviceDetails(DeviceRecord const* device)
    {
        auto children = DeviceDetailsPanel().Children();
        children.Clear();
        if (device == nullptr) {
            auto placeholder = MakeText(L"Select a USB, USB4, or Thunderbolt device to inspect its details.",
                                        L"SecondaryBodyStyle");
            placeholder.TextWrapping(TextWrapping::Wrap);
            children.Append(placeholder);
            return;
        }

        auto title = MakeText(DeviceLabel(*device), L"SubtitleTextBlockStyle");
        title.TextWrapping(TextWrapping::Wrap);
        title.IsTextSelectionEnabled(true);
        children.Append(title);

        StackPanel tags;
        tags.Orientation(Orientation::Horizontal);
        tags.Spacing(6);
        if (IsThunderbolt(*device)) {
            tags.Children().Append(MakeTag(L"Thunderbolt / USB4", L"AccentTagBorderStyle", L"AccentTagTextStyle"));
        }
        if (!device->className.empty()) {
            tags.Children().Append(MakeTag(device->className, L"TagBorderStyle", L"TagTextStyle"));
        }
        if (tags.Children().Size() > 0) {
            children.Append(tags);
        }

        const std::pair<wchar_t const*, std::wstring> fields[] = {
            { L"Status", device->status },
            { L"USB connection", JoinDots({ device->usbPort, device->usbSpeed, device->usbCapability,
                                            device->usbPortStatus.empty() ? L"" : L"port status: " + device->usbPortStatus }) },
            { L"Power", JoinDots({ device->powerState,
                                   device->powerSaving.empty() ? L"" : L"turn off to save power: " + device->powerSaving }) },
            { L"Driver", JoinDots({ device->driverProvider, device->driverVersion, device->driverDate }) },
            { L"Firmware revision", device->firmwareRevision },
            { L"Last connected", device->lastArrival },
            { L"Last removed", device->lastRemoval },
            { L"Instance ID", device->instanceId },
            { L"Parent", device->parentInstanceId },
            { L"Hardware ID", device->hardwareId },
            { L"Manufacturer", device->manufacturer },
            { L"Location", device->location },
            { L"Service", device->service },
        };
        for (auto const& [label, value] : fields) {
            StackPanel field;
            field.Spacing(2);
            field.Children().Append(MakeText(label, L"SecondaryCaptionStyle"));
            auto text = MakeText(OrDash(value), L"BodyTextBlockStyle");
            text.TextWrapping(TextWrapping::Wrap);
            text.IsTextSelectionEnabled(true);
            field.Children().Append(text);
            children.Append(field);
        }
    }

    void MainWindow::OnRefreshClicked(IInspectable const&, RoutedEventArgs const&)
    {
        RequestRefresh();
    }

    // ------------------------------------------------------------------ tracking

    void MainWindow::CheckTracking()
    {
        if (m_trackingMonitor && DisplaySignature(m_snapshot) != DisplaySignature(m_monitorBaseline.snapshot)) {
            const std::wstring file = WriteTrackingLog(m_session, Tracker::Monitor, TrackingEvent::Change, &m_snapshot,
                                                       &m_monitorBaseline);
            m_monitorBaseline = { file, m_snapshot };
            m_lastLoggedChange = L"display change at " + TimeNow();
        }
        if (m_trackingUsb && UsbSignature(m_snapshot) != UsbSignature(m_usbBaseline.snapshot)) {
            const std::wstring file = WriteTrackingLog(m_session, Tracker::UsbTree, TrackingEvent::Change, &m_snapshot,
                                                       &m_usbBaseline);
            m_usbBaseline = { file, m_snapshot };
            m_lastLoggedChange = L"USB tree change at " + TimeNow();
        }
    }

    void MainWindow::SetTracking(Tracker tracker, bool on)
    {
        bool& tracking = tracker == Tracker::Monitor ? m_trackingMonitor : m_trackingUsb;
        const bool other = tracker == Tracker::Monitor ? m_trackingUsb : m_trackingMonitor;
        if (on == tracking) {
            return;
        }
        if (on && !other) {
            m_session = StartTrackingSession();
        }
        tracking = on;
        TrackerBaseline& baseline = tracker == Tracker::Monitor ? m_monitorBaseline : m_usbBaseline;
        if (on) {
            baseline = { WriteTrackingLog(m_session, tracker, TrackingEvent::Started, &m_snapshot, nullptr), m_snapshot };
        } else {
            WriteTrackingLog(m_session, tracker, TrackingEvent::Stopped, nullptr, nullptr);
        }
        UpdateTrackingStatus();
    }

    void MainWindow::OnMonitorToggled(IInspectable const&, RoutedEventArgs const&)
    {
        SetTracking(Tracker::Monitor, MonitorToggle().IsOn());
    }

    void MainWindow::OnUsbToggled(IInspectable const&, RoutedEventArgs const&)
    {
        SetTracking(Tracker::UsbTree, UsbToggle().IsOn());
    }

    void MainWindow::OnMarkProblem(IInspectable const&, RoutedEventArgs const&)
    {
        MarkProblemAsync();
    }

    void MainWindow::OnMarkAccelerator(Input::KeyboardAccelerator const&, Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        args.Handled(true);
        MarkProblemAsync();
    }

    winrt::fire_and_forget MainWindow::MarkProblemAsync()
    {
        if (!m_trackingMonitor && !m_trackingUsb) {
            RootNavigation().SelectedItem(RootNavigation().MenuItems().GetAt(1));  // the Tracking page explains it
            MarkerStatus().Text(L"Switch on tracking first, then mark the problem.");
            co_return;
        }
        if (m_marking) {
            co_return;
        }
        auto strong = get_strong();
        auto dispatcher = DispatcherQueue();
        m_marking = true;
        const std::wstring note{ MarkerNote().Text() };
        UpdateTrackingStatus();

        // A fresh snapshot: if something changed, its change file is written first and the marker after it.
        co_await winrt::resume_background();
        HardwareSnapshot snapshot = CaptureSnapshot();
        co_await wil::resume_foreground(dispatcher);

        m_marking = false;
        if (m_closed) {
            co_return;
        }
        ApplySnapshot(std::move(snapshot));
        if (!m_trackingMonitor && !m_trackingUsb) {
            co_return;  // tracking was switched off in the meantime
        }
        const std::wstring file = WriteMarker(m_session, note, m_snapshot);
        MarkerNote().Text(L"");
        MarkerStatus().Text(file.empty() ? L"Could not write the marker file." : L"Marked at " + TimeNow() + L" · " + file);
        m_lastLoggedChange = L"problem marker at " + TimeNow();
        UpdateTrackingStatus();
    }

    void MainWindow::UpdateTrackingStatus()
    {
        const bool tracking = m_trackingMonitor || m_trackingUsb;
        MarkerButton().IsEnabled(tracking && !m_marking);
        LiveMarkerButton().Visibility(tracking ? Visibility::Visible : Visibility::Collapsed);
        LiveMarkerButton().IsEnabled(!m_marking);
        if (!tracking) {
            MarkerStatus().Text(L"Switch on tracking to mark problems.");
        } else if (std::wstring_view(MarkerStatus().Text()).rfind(L"Switch on tracking", 0) == 0) {
            MarkerStatus().Text(L"A marker saves a fresh snapshot and the recent Windows events into the current session.");
        }

        auto status = TrackingStatus();
        if (!m_hasSnapshot) {
            status.Severity(InfoBarSeverity::Informational);
            status.Title(L"Tracking is off");
            status.Message(L"Waiting for the first hardware snapshot…");
            return;
        }

        const bool running = m_trackingMonitor || m_trackingUsb;
        status.Severity(running ? InfoBarSeverity::Success : InfoBarSeverity::Informational);
        status.Title(running ? L"Tracking is on · session s" + std::to_wstring(m_session.number) : L"Tracking is off");
        std::wstring message = std::wstring(L"Monitor tracking: ") + (m_trackingMonitor ? L"on" : L"off") +
                               L" · USB tree tracking: " + (m_trackingUsb ? L"on" : L"off");
        if (!m_lastLoggedChange.empty()) {
            message += L" · Last logged " + m_lastLoggedChange;
        }
        status.Message(message);
    }

    void MainWindow::OnOpenLogFolder(IInspectable const&, RoutedEventArgs const&)
    {
        ShellExecuteW(nullptr, L"open", TrackingDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ------------------------------------------------------------------ navigation

    void MainWindow::OnPaneToggleRequested(TitleBar const&, IInspectable const&)
    {
        RootNavigation().IsPaneOpen(!RootNavigation().IsPaneOpen());
    }

    void MainWindow::OnNavigationChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& args)
    {
        const auto item = args.SelectedItem().try_as<NavigationViewItem>();
        if (!item) {
            return;
        }
        const hstring page = unbox_value_or<hstring>(item.Tag(), L"live");
        LivePage().Visibility(page == L"live" ? Visibility::Visible : Visibility::Collapsed);
        TrackingPage().Visibility(page == L"tracking" ? Visibility::Visible : Visibility::Collapsed);
        SystemPage().Visibility(page == L"system" ? Visibility::Visible : Visibility::Collapsed);
        TopologyPage().Visibility(page == L"topology" ? Visibility::Visible : Visibility::Collapsed);
        if (page == L"system") {
            RefreshSystemAsync();
        }
        if (page == L"topology") {
            m_renderedTopologySignature.clear();
            RebuildTopology();
            if (!m_trafficTimer) {
                m_trafficTimer = DispatcherQueue().CreateTimer();
                m_trafficTimer.Interval(std::chrono::seconds(2));
                m_trafficTimer.Tick([weak = get_weak()](auto&&, auto&&) {
                    if (auto self = weak.get()) {
                        self->RefreshTrafficAsync();
                    }
                });
            }
            m_trafficPrevious.clear();
            RefreshTrafficAsync();
            m_trafficTimer.Start();
        } else if (m_trafficTimer) {
            m_trafficTimer.Stop();
        }
    }

    void MainWindow::OnOpenUsbSettings(IInspectable const&, RoutedEventArgs const&)
    {
        ShellExecuteW(nullptr, L"open", L"ms-settings:usb", nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ------------------------------------------------------------------ topology page

    void MainWindow::RebuildTopology()
    {
        const std::wstring signature = UsbSignature(m_snapshot) + DisplaySignature(m_snapshot);
        if (signature == m_renderedTopologySignature) {
            return;
        }
        m_renderedTopologySignature = signature;

        std::map<std::wstring, DeviceRecord const*> byId;
        std::map<std::wstring, std::vector<DeviceRecord const*>> children;
        for (DeviceRecord const& device : m_snapshot.devices) {
            byId[device.instanceId] = &device;
        }
        for (DeviceRecord const& device : m_snapshot.devices) {
            if (byId.count(device.parentInstanceId) && device.parentInstanceId != device.instanceId) {
                children[device.parentInstanceId].push_back(&device);
            }
        }
        auto makeCard = [](UIElement const& content) {
            Border card;
            card.Style(LookupStyle(L"CardBorderStyle"));
            card.MinHeight(0);
            card.Child(content);
            return card;
        };

        // --- USB4 / Thunderbolt: host routers (parents of a USB4 root router) with their router chains.
        auto routers = RoutersPanel().Children();
        routers.Clear();
        std::vector<DeviceRecord const*> hostRouters;
        for (DeviceRecord const& device : m_snapshot.devices) {
            if (IsUsb4Router(device) && !byId.count(device.parentInstanceId)) {
                continue;
            }
            if (IsUsb4Router(device)) {
                DeviceRecord const* parent = byId[device.parentInstanceId];
                if (!IsUsb4Router(*parent) && std::find(hostRouters.begin(), hostRouters.end(), parent) == hostRouters.end()) {
                    hostRouters.push_back(parent);
                }
            }
        }
        for (DeviceRecord const* host : hostRouters) {
            StackPanel rows;
            rows.Spacing(6);
            rows.Children().Append(MakeText(DeviceLabel(*host), L"BodyStrongTextBlockStyle"));
            std::function<void(DeviceRecord const&, int)> addRouter = [&](DeviceRecord const& router, int depth) {
                const bool root = router.instanceId.find(L"ROOT_DEVICE_ROUTER") != std::wstring::npos;
                StackPanel row;
                row.Orientation(Orientation::Horizontal);
                row.Spacing(10);
                row.Margin(ThicknessHelper::FromLengths(depth * 24.0, 0, 0, 0));
                FontIcon icon;
                icon.Glyph(root ? L"" : L"");  // this computer / connected device
                icon.FontSize(16);
                row.Children().Append(icon);
                row.Children().Append(MakeText(root ? L"This computer's USB4 router" : DeviceLabel(router), L"BodyTextBlockStyle"));
                if (!router.firmwareVersion.empty()) {
                    auto firmware = MakeText(L"firmware " + router.firmwareVersion, L"SecondaryCaptionStyle");
                    firmware.VerticalAlignment(VerticalAlignment::Center);
                    row.Children().Append(firmware);
                }
                if (router.status.find(L"problem") != std::wstring::npos) {
                    row.Children().Append(MakeTag(router.status, L"CriticalTagBorderStyle", L"CriticalTagTextStyle"));
                }
                rows.Children().Append(row);
                for (DeviceRecord const* child : children[router.instanceId]) {
                    if (IsUsb4Router(*child)) {
                        addRouter(*child, depth + 1);
                    }
                }
            };
            for (DeviceRecord const* child : children[host->instanceId]) {
                if (IsUsb4Router(*child)) {
                    addRouter(*child, 0);
                }
            }
            auto note = MakeText(L"Link bandwidth and lanes: see Settings › Bluetooth & devices › USB › USB4 hubs and devices.",
                                 L"SecondaryCaptionStyle");
            note.TextWrapping(TextWrapping::Wrap);
            rows.Children().Append(note);
            routers.Append(makeCard(rows));
        }
        if (hostRouters.empty()) {
            auto none = MakeText(L"No USB4 / Thunderbolt connection manager found. Computers with an older Thunderbolt 3 "
                                 L"controller do not show their Thunderbolt connection here; their docks still appear "
                                 L"under USB connections.", L"SecondaryBodyStyle");
            none.TextWrapping(TextWrapping::Wrap);
            routers.Append(none);
        }

        // --- USB connections: one card per USB controller (a root of the USB / behind-USB set).
        auto inUsbView = [&](DeviceRecord const& device) {
            // USB4 routers and helper devices are shown above; host routers too.
            if (_wcsnicmp(device.instanceId.c_str(), L"USB4\\", 5) == 0 ||
                std::find(hostRouters.begin(), hostRouters.end(), &device) != hostRouters.end()) {
                return false;
            }
            return IsUsbFamily(device) || device.behindUsb;
        };
        std::function<size_t(DeviceRecord const&)> countUsbBelow = [&](DeviceRecord const& device) {
            size_t count = 0;
            for (DeviceRecord const* child : children[device.instanceId]) {
                count += (child->usbPort.empty() ? 0 : 1) + countUsbBelow(*child);
            }
            return count;
        };
        auto usb = UsbTopologyPanel().Children();
        usb.Clear();
        for (DeviceRecord const& root : m_snapshot.devices) {
            if (!inUsbView(root) || (byId.count(root.parentInstanceId) && inUsbView(*byId[root.parentInstanceId]))) {
                continue;
            }
            StackPanel rows;
            rows.Spacing(6);
            std::function<void(DeviceRecord const&, int)> addRow = [&](DeviceRecord const& device, int depth) {
                Grid row;
                row.ColumnSpacing(12);
                for (GridUnitType unit : { GridUnitType::Star, GridUnitType::Auto, GridUnitType::Auto }) {
                    ColumnDefinition column;
                    column.Width(GridLength{ 1, unit });
                    row.ColumnDefinitions().Append(column);
                }
                // Icon, name (shortened with "..." when space runs out), then the notes that must stay visible.
                Grid name;
                name.ColumnSpacing(10);
                name.Margin(ThicknessHelper::FromLengths(depth * 24.0, 0, 0, 0));
                for (GridUnitType unit : { GridUnitType::Auto, GridUnitType::Auto, GridUnitType::Auto }) {
                    ColumnDefinition column;
                    column.Width(GridLength{ 1, unit });
                    name.ColumnDefinitions().Append(column);
                }
                name.ColumnDefinitions().GetAt(1).Width(GridLength{ 1, GridUnitType::Star });
                FontIcon icon;
                icon.Glyph(DeviceGlyph(device));
                icon.FontSize(16);
                name.Children().Append(icon);
                auto label = MakeText(DeviceLabel(device), depth == 0 ? L"BodyStrongTextBlockStyle" : L"BodyTextBlockStyle");
                label.TextTrimming(TextTrimming::CharacterEllipsis);
                label.TextWrapping(TextWrapping::NoWrap);
                ToolTipService::SetToolTip(label, box_value(DeviceLabel(device)));
                Grid::SetColumn(label, 1);
                name.Children().Append(label);
                StackPanel notes;
                notes.Orientation(Orientation::Horizontal);
                notes.Spacing(8);
                const size_t below = countUsbBelow(device);
                if (below > 0 && !device.usbPort.empty()) {
                    auto shared = MakeText(L"shared by " + std::to_wstring(below) + (below == 1 ? L" device" : L" devices"),
                                           L"SecondaryCaptionStyle");
                    shared.VerticalAlignment(VerticalAlignment::Center);
                    notes.Children().Append(shared);
                }
                if (device.usbCapability.find(L"below") != std::wstring::npos) {
                    notes.Children().Append(MakeTag(L"below its capability", L"CautionTagBorderStyle", L"CautionTagTextStyle"));
                }
                if (device.status.find(L"problem") != std::wstring::npos) {
                    notes.Children().Append(MakeTag(L"problem", L"CriticalTagBorderStyle", L"CriticalTagTextStyle"));
                }
                Grid::SetColumn(notes, 2);
                name.Children().Append(notes);
                row.Children().Append(name);

                if (!device.usbPort.empty()) {
                    auto port = MakeText(device.usbPort, L"SecondaryCaptionStyle");
                    port.VerticalAlignment(VerticalAlignment::Center);
                    Grid::SetColumn(port, 1);
                    row.Children().Append(port);
                }
                if (Border speed = SpeedTag(device.usbSpeed)) {
                    Grid::SetColumn(speed, 2);
                    row.Children().Append(speed);
                }
                rows.Children().Append(row);
                for (DeviceRecord const* child : children[device.instanceId]) {
                    if (inUsbView(*child)) {
                        addRow(*child, depth + 1);
                    }
                }
            };
            addRow(root, 0);
            usb.Append(makeCard(rows));
        }

        // --- Displays: estimated uncompressed data rate per active monitor, and the sum.
        auto displays = DisplayBandwidthPanel().Children();
        displays.Clear();
        double total = 0;  // external displays only: a built-in panel never runs through a dock
        size_t external = 0;
        std::vector<std::pair<std::wstring, std::wstring>> displayRows;
        for (DisplayRecord const& display : m_snapshot.displays) {
            if (!display.active) {
                continue;
            }
            if (display.connection.rfind(L"Internal", 0) != 0) {
                total += DisplayBandwidthGbps(display);
                ++external;
            }
            displayRows.push_back({ DisplayLabel(display),
                                    JoinDots({ DisplayMode(display), display.connection, OrDash(DisplayBandwidthText(display)) }) });
        }
        if (displayRows.empty()) {
            displays.Append(MakeText(L"No active displays.", L"SecondaryBodyStyle"));
        } else {
            if (external > 0) {
                displayRows.push_back({ L"External displays", L"≈ " + FormatGbps(total) + L" uncompressed in total (" +
                                                                  std::to_wstring(external) + (external == 1 ? L" display)" : L" displays)") });
            }
            displays.Append(MakeInfoCard(displayRows));
        }
    }

    winrt::fire_and_forget MainWindow::RefreshTrafficAsync()
    {
        if (m_trafficLoading) {
            co_return;
        }
        auto strong = get_strong();
        auto dispatcher = DispatcherQueue();
        m_trafficLoading = true;
        co_await winrt::resume_background();
        const std::vector<TrafficCounter> counters = ReadTrafficCounters();
        const unsigned long long now = UtcNow();
        co_await wil::resume_foreground(dispatcher);
        m_trafficLoading = false;
        if (m_closed || TopologyPage().Visibility() != Visibility::Visible) {
            co_return;
        }

        auto viaUsb = [&](TrafficCounter const& counter) {
            if (counter.detail == L"bus: USB") {
                return true;
            }
            for (DeviceRecord const& device : m_snapshot.devices) {
                if (device.behindUsb && ((counter.kind == L"Drive" && _wcsicmp(device.instanceId.c_str(), counter.instanceId.c_str()) == 0) ||
                                         (counter.kind == L"Network adapter" && device.friendlyName == counter.name))) {
                    return true;
                }
            }
            return false;
        };

        auto panel = TrafficPanel().Children();
        panel.Clear();
        std::vector<std::pair<std::wstring, std::wstring>> rows;
        for (bool usbFirst : { true, false }) {
            for (TrafficCounter const& counter : counters) {
                if (viaUsb(counter) != usbFirst) {
                    continue;
                }
                std::wstring rate = L"measuring…";
                const auto previous = m_trafficPrevious.find(counter.key);
                if (previous != m_trafficPrevious.end() && now > previous->second.time) {
                    const double seconds = static_cast<double>(now - previous->second.time) / UtcSecond;
                    const double in = counter.received >= previous->second.received ? (counter.received - previous->second.received) / seconds : 0;
                    const double out = counter.sent >= previous->second.sent ? (counter.sent - previous->second.sent) / seconds : 0;
                    rate = counter.kind == L"Drive"
                               ? L"read " + FormatBytesPerSecond(in) + L" · write " + FormatBytesPerSecond(out)
                               : L"↓ " + FormatGbps(in * 8 / 1e9) + L" · ↑ " + FormatGbps(out * 8 / 1e9);
                }
                if (counter.kind == L"Network adapter" && !counter.connected) {
                    rate.clear();
                }
                rows.push_back({ counter.name + (usbFirst ? L" · through USB / Thunderbolt" : L""),
                                 JoinDots({ counter.kind, counter.detail, rate }) });
                m_trafficPrevious[counter.key] = { now, counter.received, counter.sent };
            }
        }
        if (rows.empty()) {
            panel.Append(MakeText(L"No network adapters or drives found.", L"SecondaryBodyStyle"));
        } else {
            panel.Append(MakeInfoCard(rows));
        }
    }

    // ------------------------------------------------------------------ system page

    void MainWindow::OnSystemRefreshClicked(IInspectable const&, RoutedEventArgs const&)
    {
        RefreshSystemAsync();
    }

    winrt::fire_and_forget MainWindow::RefreshSystemAsync()
    {
        if (m_systemLoading) {
            co_return;
        }
        auto strong = get_strong();
        auto dispatcher = DispatcherQueue();
        m_systemLoading = true;
        SystemProgress().IsActive(true);
        SystemRefreshButton().IsEnabled(false);

        co_await winrt::resume_background();
        const SystemInfo info = CaptureSystemInfo();
        const unsigned long long now = UtcNow();
        const WindowsEventList events = ReadWindowsEvents(now - 24 * 60 * 60 * UtcSecond, now, 100);
        const ReliabilityHistory history = ReadReliabilityHistory(30);
        co_await wil::resume_foreground(dispatcher);

        m_systemLoading = false;
        if (m_closed) {
            co_return;
        }
        SystemProgress().IsActive(false);
        SystemRefreshButton().IsEnabled(true);
        SystemUpdatedText().Text(L"Updated " + TimeNow());

        PowerHint().IsOpen(info.usbSelectiveSuspendOn);
        DrainHint().IsOpen(info.power.drainingOnAc);
        ComputerPanel().Children().Clear();
        ComputerPanel().Children().Append(MakeInfoCard({
            { L"Manufacturer", info.manufacturer },
            { L"Model", info.model },
            { L"BIOS", info.biosVersion },
        }));
        PowerPanel().Children().Clear();
        PowerPanel().Children().Append(MakeInfoCard({
            { L"Power source", info.power.summary },
            { L"Power plan", info.powerPlan },
            { L"Sleep", info.sleepModel },
            { L"USB selective suspend", info.usbSelectiveSuspendAc + L" (AC power) · " + info.usbSelectiveSuspendDc + L" (battery)" },
            { L"PCI Express link power saving", info.pcieLinkPowerAc + L" (AC power) · " + info.pcieLinkPowerDc + L" (battery)" },
        }));

        // Reliability history: grouped cards per category; long routine categories are cut short.
        auto reliability = ReliabilityPanel().Children();
        reliability.Clear();
        size_t crashDays = 0;
        std::wstring crashKinds;
        std::wstring category;
        size_t shownInCategory = 0;
        size_t hiddenInCategory = 0;
        auto flushHidden = [&] {
            if (hiddenInCategory > 0) {
                reliability.Append(MakeText(std::to_wstring(hiddenInCategory) + L" more (all of them are in the session info file)",
                                            L"SecondaryCaptionStyle"));
            }
            hiddenInCategory = 0;
        };
        for (HistoryEntry const& entry : history.entries) {
            const bool crash = entry.category.rfind(L"Blue screen", 0) == 0 || entry.category.rfind(L"Kernel live dump", 0) == 0;
            if (crash) {
                crashDays = std::max(crashDays, entry.days);
                crashKinds += (crashKinds.empty() ? L"" : L", ") + entry.title.substr(0, entry.title.find(L' '));
            }
            if (entry.category != category) {
                flushHidden();
                category = entry.category;
                shownInCategory = 0;
                auto header = MakeText(category, L"BodyStrongTextBlockStyle");
                header.Margin(ThicknessHelper::FromLengths(1, reliability.Size() == 0 ? 0 : 12, 0, 4));
                reliability.Append(header);
            }
            if (!crash && ++shownInCategory > 8) {
                ++hiddenInCategory;
                continue;
            }
            auto title = MakeText(entry.title, L"BodyTextBlockStyle");
            title.TextWrapping(TextWrapping::Wrap);
            title.IsTextSelectionEnabled(true);
            auto when = MakeText(JoinDots({ std::to_wstring(entry.count) + (entry.count == 1 ? L" report" : L" reports") +
                                                L" on " + std::to_wstring(entry.days) + (entry.days == 1 ? L" day" : L" days"),
                                            entry.count == 1 ? entry.last : entry.first + L" – " + entry.last, entry.detail }),
                                 L"SecondaryCaptionStyle");
            when.TextWrapping(TextWrapping::Wrap);
            when.IsTextSelectionEnabled(true);
            StackPanel content;
            content.Spacing(2);
            content.Children().Append(title);
            content.Children().Append(when);
            Border card;
            card.Style(LookupStyle(L"CardBorderStyle"));
            card.MinHeight(0);
            card.Margin(ThicknessHelper::FromLengths(0, 0, 0, 4));
            card.Child(content);
            reliability.Append(card);
        }
        flushHidden();
        if (history.entries.empty()) {
            reliability.Append(MakeText(L"Nothing recorded in the last 30 days.", L"SecondaryBodyStyle"));
        }
        if (!history.error.empty()) {
            auto error = MakeText(L"Could not read: " + history.error, L"SecondaryBodyStyle");
            error.TextWrapping(TextWrapping::Wrap);
            reliability.Append(error);
        }
        CrashHint().IsOpen(!crashKinds.empty());
        CrashHint().Message(L"Stop codes " + crashKinds + L", on up to " + std::to_wstring(crashDays) +
                            L" days. Blue screens and driver hangs of the graphics or USB drivers often go together with "
                            L"monitor and dock dropouts. Details below and in the session info file of every recording.");

        auto list = EventsPanel().Children();
        list.Clear();
        if (!events.error.empty()) {
            auto error = MakeText(L"Could not read: " + events.error, L"SecondaryBodyStyle");
            error.TextWrapping(TextWrapping::Wrap);
            list.Append(error);
        }
        if (events.events.empty()) {
            list.Append(MakeText(L"No relevant events in the last 24 hours.", L"SecondaryBodyStyle"));
        }
        // Newest first.
        for (auto event = events.events.rbegin(); event != events.events.rend(); ++event) {
            StackPanel header;
            header.Orientation(Orientation::Horizontal);
            header.Spacing(8);
            if (event->level == L"Error" || event->level == L"Critical") {
                header.Children().Append(MakeTag(event->level, L"CriticalTagBorderStyle", L"CriticalTagTextStyle"));
            } else if (event->level == L"Warning") {
                header.Children().Append(MakeTag(event->level, L"CautionTagBorderStyle", L"CautionTagTextStyle"));
            }
            auto source = MakeText(event->timeText.substr(0, 19) + L" · " + event->provider + L" · " +
                                       std::to_wstring(event->id),
                                   L"SecondaryCaptionStyle");
            source.VerticalAlignment(VerticalAlignment::Center);
            header.Children().Append(source);

            auto message = MakeText(event->message, L"BodyTextBlockStyle");
            message.TextWrapping(TextWrapping::Wrap);
            message.MaxLines(3);
            message.IsTextSelectionEnabled(true);

            StackPanel content;
            content.Spacing(4);
            content.Children().Append(header);
            content.Children().Append(message);
            Border card;
            card.Style(LookupStyle(L"CardBorderStyle"));
            card.MinHeight(0);
            card.Child(content);
            list.Append(card);
        }
        if (events.omitted > 0) {
            list.Append(MakeText(std::to_wstring(events.omitted) + L" older events not shown.", L"SecondaryCaptionStyle"));
        }
    }
}
