#pragma once

#include "MainWindow.g.h"
#include "../src/report.h"

#include <map>
#include <string>

namespace winrt::DockDebug::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow() = default;
        void InitializeComponent();

        void OnPaneToggleRequested(Microsoft::UI::Xaml::Controls::TitleBar const&,
                                   Windows::Foundation::IInspectable const&);
        void OnNavigationChanged(Microsoft::UI::Xaml::Controls::NavigationView const&,
                                 Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&);
        void OnRefreshClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnDeviceSelectionChanged(Microsoft::UI::Xaml::Controls::TreeView const&,
                                      Microsoft::UI::Xaml::Controls::TreeViewSelectionChangedEventArgs const&);
        void OnMonitorToggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnUsbToggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenLogFolder(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSystemRefreshClicked(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMarkProblem(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenUsbSettings(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMarkAccelerator(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&,
                               Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);

    private:
        static LRESULT CALLBACK DeviceChangeSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                                     UINT_PTR id, DWORD_PTR data);

        winrt::fire_and_forget SetTitleBarIconAsync();
        void RequestRefresh();
        winrt::fire_and_forget RefreshAsync();
        void ApplySnapshot(HardwareSnapshot snapshot);
        void RebuildDisplays();
        void RebuildDeviceTree();
        void ShowDeviceDetails(DeviceRecord const* device);
        void UpdateLiveStatus();
        void UpdateTrackingStatus();
        void CheckTracking();
        void SetTracking(Tracker tracker, bool on);
        winrt::fire_and_forget RefreshSystemAsync();
        winrt::fire_and_forget MarkProblemAsync();
        void RebuildTopology();
        winrt::fire_and_forget RefreshTrafficAsync();
        void Shutdown();

        HWND m_hwnd = nullptr;
        DeviceWatcher m_watcher;
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{ nullptr };

        HardwareSnapshot m_snapshot;
        bool m_hasSnapshot = false;
        bool m_scanning = false;
        bool m_rescanPending = false;
        bool m_closed = false;

        // Signatures of what is currently rendered, so the tree is only rebuilt when it actually changed.
        std::wstring m_renderedDisplaySignature;
        std::wstring m_renderedUsbSignature;
        std::wstring m_selectedInstanceId;

        bool m_trackingMonitor = false;
        bool m_trackingUsb = false;
        TrackingSession m_session;  // valid while a tracker is on
        TrackerBaseline m_monitorBaseline;
        TrackerBaseline m_usbBaseline;
        std::wstring m_lastLoggedChange;

        bool m_systemLoading = false;
        bool m_marking = false;

        // Topology page: rebuilt when the snapshot changes while it is visible; traffic polled every 2 s.
        std::wstring m_renderedTopologySignature;
        Microsoft::UI::Dispatching::DispatcherQueueTimer m_trafficTimer{ nullptr };
        bool m_trafficLoading = false;
        struct TrafficSample {
            unsigned long long time = 0;  // UTC FILETIME units
            unsigned long long received = 0;
            unsigned long long sent = 0;
        };
        std::map<std::wstring, TrafficSample> m_trafficPrevious;
    };
}

namespace winrt::DockDebug::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
