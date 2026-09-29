#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
#include "DeviceNodeItem.h"
#include "../src/report.h"

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

} // namespace

namespace winrt::DockDebug::implementation
{
    void MainWindow::InitializeComponent()
    {
        MainWindowT::InitializeComponent();

        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());

        SetTitleBarIconAsync();
        AppWindow().TitleBar().PreferredHeightOption(Microsoft::UI::Windowing::TitleBarHeightOption::Tall);

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
        m_watcher.Start(m_hwnd, {});

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
        if (message == WM_DEVICECHANGE && (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE ||
                                           wParam == DBT_DEVNODES_CHANGED)) {
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
        if (thunderbolt == 0) {
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
            icon.Glyph(L"");
            icon.FontSize(20);
            icon.VerticalAlignment(VerticalAlignment::Center);
            grid.Children().Append(icon);

            StackPanel text;
            text.VerticalAlignment(VerticalAlignment::Center);
            text.Children().Append(MakeText(display.friendlyName.empty() ? L"Unknown display" : display.friendlyName,
                                            L"BodyStrongTextBlockStyle"));
            auto mode = MakeText(DisplayMode(display) + L" · " + std::to_wstring(display.bitsPerPixel) +
                                     L" bpp · " + display.adapterString,
                                 L"SecondaryCaptionStyle");
            mode.TextTrimming(TextTrimming::CharacterEllipsis);
            text.Children().Append(mode);
            auto id = MakeText(display.deviceString + L" · " + display.deviceName, L"SecondaryCaptionStyle");
            id.TextTrimming(TextTrimming::CharacterEllipsis);
            id.IsTextSelectionEnabled(true);
            text.Children().Append(id);
            Grid::SetColumn(text, 1);
            grid.Children().Append(text);

            StackPanel tags;
            tags.Orientation(Orientation::Horizontal);
            tags.Spacing(6);
            tags.VerticalAlignment(VerticalAlignment::Center);
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
        tree.RootNodes().Clear();

        // Create every node first, then attach each to its parent, so the tree does not depend on
        // parents being enumerated before their children.
        std::map<std::wstring, TreeViewNode> nodes;
        std::vector<DeviceRecord const*> devices;
        for (DeviceRecord const& device : m_snapshot.devices) {
            if (!IsUsbFamily(device)) {
                continue;
            }
            TreeViewNode node;
            node.Content(make<DeviceNodeItem>(hstring{ DeviceLabel(device) },
                                              hstring{ IsThunderbolt(device) ? L"" : L"" },
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

        const std::pair<wchar_t const*, std::wstring const*> fields[] = {
            { L"Status", &device->status },
            { L"Instance ID", &device->instanceId },
            { L"Parent", &device->parentInstanceId },
            { L"Hardware ID", &device->hardwareId },
            { L"Manufacturer", &device->manufacturer },
            { L"Location", &device->location },
            { L"Service", &device->service },
        };
        for (auto const& [label, value] : fields) {
            StackPanel field;
            field.Spacing(2);
            field.Children().Append(MakeText(label, L"SecondaryCaptionStyle"));
            auto text = MakeText(OrDash(*value), L"BodyTextBlockStyle");
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
        if (m_trackingMonitor) {
            const std::wstring signature = DisplaySignature(m_snapshot);
            if (signature != m_previousMonitorSignature) {
                WriteTrackingLog(Tracker::Monitor, TrackingEvent::Change, &m_snapshot);
                m_previousMonitorSignature = signature;
                m_lastLoggedChange = L"display change at " + TimeNow();
            }
        }
        if (m_trackingUsb) {
            const std::wstring signature = UsbSignature(m_snapshot);
            if (signature != m_previousUsbSignature) {
                WriteTrackingLog(Tracker::UsbTree, TrackingEvent::Change, &m_snapshot);
                m_previousUsbSignature = signature;
                m_lastLoggedChange = L"USB tree change at " + TimeNow();
            }
        }
    }

    void MainWindow::OnMonitorToggled(IInspectable const&, RoutedEventArgs const&)
    {
        const bool on = MonitorToggle().IsOn();
        if (on == m_trackingMonitor) {
            return;
        }
        m_trackingMonitor = on;
        if (on) {
            m_previousMonitorSignature = DisplaySignature(m_snapshot);
            WriteTrackingLog(Tracker::Monitor, TrackingEvent::Started, &m_snapshot);
        } else {
            WriteTrackingLog(Tracker::Monitor, TrackingEvent::Stopped, nullptr);
        }
        UpdateTrackingStatus();
    }

    void MainWindow::OnUsbToggled(IInspectable const&, RoutedEventArgs const&)
    {
        const bool on = UsbToggle().IsOn();
        if (on == m_trackingUsb) {
            return;
        }
        m_trackingUsb = on;
        if (on) {
            m_previousUsbSignature = UsbSignature(m_snapshot);
            WriteTrackingLog(Tracker::UsbTree, TrackingEvent::Started, &m_snapshot);
        } else {
            WriteTrackingLog(Tracker::UsbTree, TrackingEvent::Stopped, nullptr);
        }
        UpdateTrackingStatus();
    }

    void MainWindow::UpdateTrackingStatus()
    {
        auto status = TrackingStatus();
        if (!m_hasSnapshot) {
            status.Severity(InfoBarSeverity::Informational);
            status.Title(L"Tracking is off");
            status.Message(L"Waiting for the first hardware snapshot…");
            return;
        }

        const bool running = m_trackingMonitor || m_trackingUsb;
        status.Severity(running ? InfoBarSeverity::Success : InfoBarSeverity::Informational);
        status.Title(running ? L"Tracking is on" : L"Tracking is off");
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
        const bool tracking = unbox_value_or<hstring>(item.Tag(), L"") == L"tracking";
        LivePage().Visibility(tracking ? Visibility::Collapsed : Visibility::Visible);
        TrackingPage().Visibility(tracking ? Visibility::Visible : Visibility::Collapsed);
    }
}
