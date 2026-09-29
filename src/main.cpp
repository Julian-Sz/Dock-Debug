#include "monitor.h"
#include "report.h"

#include <windows.h>
#include <dbt.h>
#include <commctrl.h>

#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace {
constexpr wchar_t WindowClassName[] = L"DockDebugDashboard";
constexpr UINT LivePageId = 1001;
constexpr UINT TrackingPageId = 1002;
constexpr UINT MonitorStartId = 1101;
constexpr UINT MonitorStopId = 1102;
constexpr UINT UsbStartId = 1103;
constexpr UINT UsbStopId = 1104;
constexpr UINT WM_REFRESH = WM_APP + 1;
constexpr UINT DisplayListId = 1201;
constexpr UINT DeviceTreeId = 1202;
constexpr UINT DeviceDetailId = 1203;

constexpr COLORREF BackgroundColor = RGB(15, 23, 42);
constexpr COLORREF SidebarColor = RGB(17, 30, 52);
constexpr COLORREF PanelColor = RGB(30, 41, 59);
constexpr COLORREF TextColor = RGB(226, 232, 240);
constexpr COLORREF MutedColor = RGB(148, 163, 184);
constexpr COLORREF AccentColor = RGB(45, 212, 191);

struct AppState {
    HWND window = nullptr;
    HWND content = nullptr;
    HWND livePage = nullptr;
    HWND trackingPage = nullptr;
    HWND liveHeading = nullptr;
    HWND displayList = nullptr;
    HWND deviceTree = nullptr;
    HWND deviceDetail = nullptr;
    HWND trackingOutput = nullptr;
    HWND liveNav = nullptr;
    HWND trackingNav = nullptr;
    HWND monitorStart = nullptr;
    HWND monitorStop = nullptr;
    HWND usbStart = nullptr;
    HWND usbStop = nullptr;
    HWND trackingStatus = nullptr;
    HFONT titleFont = nullptr;
    HFONT bodyFont = nullptr;
    DeviceWatcher watcher;
    HardwareSnapshot snapshot;
    HardwareSnapshot pendingSnapshot;
    std::mutex snapshotMutex;
    bool hasPendingSnapshot = false;
    HANDLE stopCaptureEvent = nullptr;
    HANDLE requestCaptureEvent = nullptr;
    std::thread captureThread;
    bool trackingMonitor = false;
    bool trackingUsb = false;
    std::wstring previousMonitorSignature;
    std::wstring previousUsbSignature;
};

void RequestCapture(AppState& state) {
    if (state.requestCaptureEvent != nullptr) {
        SetEvent(state.requestCaptureEvent);
    }
}

void StartCaptureWorker(AppState& state) {
    state.stopCaptureEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    state.requestCaptureEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    state.captureThread = std::thread([&state]() {
        HANDLE events[] = {state.stopCaptureEvent, state.requestCaptureEvent};
        while (true) {
            const DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, 10000);
            if (waitResult == WAIT_OBJECT_0) {
                break;
            }
            if (waitResult == WAIT_OBJECT_0 + 1) {
                ResetEvent(state.requestCaptureEvent);
            }

            HardwareSnapshot captured = CaptureSnapshot();
            {
                std::lock_guard<std::mutex> lock(state.snapshotMutex);
                state.pendingSnapshot = std::move(captured);
                state.hasPendingSnapshot = true;
            }
            PostMessageW(state.window, WM_REFRESH, 0, 0);
        }
    });
    RequestCapture(state);
}

void StopCaptureWorker(AppState& state) {
    if (state.stopCaptureEvent != nullptr) {
        SetEvent(state.stopCaptureEvent);
    }
    if (state.captureThread.joinable()) {
        state.captureThread.join();
    }
    if (state.stopCaptureEvent != nullptr) {
        CloseHandle(state.stopCaptureEvent);
        state.stopCaptureEvent = nullptr;
    }
    if (state.requestCaptureEvent != nullptr) {
        CloseHandle(state.requestCaptureEvent);
        state.requestCaptureEvent = nullptr;
    }
}

HFONT MakeFont(int size, bool bold = false) {
    return CreateFontW(-size, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void SetFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND MakeControl(const wchar_t* className, const wchar_t* text, DWORD style,
                 int x, int y, int width, int height, HWND parent, int id, HINSTANCE instance) {
    return CreateWindowExW(0, className, text, style, x, y, width, height, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
}

void AddListColumn(HWND list, int index, int width, const wchar_t* title) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    column.fmt = LVCFMT_LEFT;
    column.cx = width;
    column.pszText = const_cast<LPWSTR>(title);
    ListView_InsertColumn(list, index, &column);
}

void PopulateLiveControls(AppState& state) {
    ListView_DeleteAllItems(state.displayList);
    for (size_t index = 0; index < state.snapshot.displays.size(); ++index) {
        const DisplayRecord& display = state.snapshot.displays[index];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<LPWSTR>(display.friendlyName.c_str());
        ListView_InsertItem(state.displayList, &item);
        ListView_SetItemText(state.displayList, item.iItem, 1,
                             const_cast<LPWSTR>(display.active ? L"Active" : L"Inactive"));
        const std::wstring mode = DisplayMode(display);
        ListView_SetItemText(state.displayList, item.iItem, 2, const_cast<LPWSTR>(mode.c_str()));
        ListView_SetItemText(state.displayList, item.iItem, 3, const_cast<LPWSTR>(display.adapterString.c_str()));
        ListView_SetItemText(state.displayList, item.iItem, 4, const_cast<LPWSTR>(display.deviceString.c_str()));
    }

    TreeView_DeleteAllItems(state.deviceTree);
    TVINSERTSTRUCTW rootInsert{};
    rootInsert.hParent = TVI_ROOT;
    rootInsert.item.mask = TVIF_TEXT;
    rootInsert.item.pszText = const_cast<LPWSTR>(L"USB / Thunderbolt devices");
    const HTREEITEM root = TreeView_InsertItem(state.deviceTree, &rootInsert);
    std::map<std::wstring, HTREEITEM> treeItems;
    for (const DeviceRecord& device : state.snapshot.devices) {
        if (!IsUsbFamily(device)) {
            continue;
        }
        TVINSERTSTRUCTW insert{};
        const auto parent = treeItems.find(device.parentInstanceId);
        insert.hParent = parent == treeItems.end() ? root : parent->second;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        const std::wstring label = (IsThunderbolt(device) ? L"[TB/USB4] " : L"") + DeviceLabel(device);
        insert.item.pszText = const_cast<LPWSTR>(label.c_str());
        insert.item.lParam = reinterpret_cast<LPARAM>(&device);
        treeItems[device.instanceId] = TreeView_InsertItem(state.deviceTree, &insert);
    }
    TreeView_Expand(state.deviceTree, root, TVE_EXPAND);
    SetWindowTextW(state.deviceDetail, L"Select a USB, USB4, or Thunderbolt device to inspect its details.");
}

bool ConsumePendingSnapshot(AppState& state) {
    std::lock_guard<std::mutex> lock(state.snapshotMutex);
    if (!state.hasPendingSnapshot) {
        return false;
    }
    state.snapshot = std::move(state.pendingSnapshot);
    state.hasPendingSnapshot = false;
    return true;
}

void UpdateTrackingStatus(AppState& state) {
    std::wstring status = L"Monitor tracking: " + std::wstring(state.trackingMonitor ? L"RUNNING" : L"STOPPED")
        + L"    |    USB tree tracking: " + (state.trackingUsb ? L"RUNNING" : L"STOPPED")
        + L"\r\nLogs: " + TrackingDirectory();
    SetWindowTextW(state.trackingStatus, status.c_str());
    EnableWindow(state.monitorStart, !state.trackingMonitor);
    EnableWindow(state.monitorStop, state.trackingMonitor);
    EnableWindow(state.usbStart, !state.trackingUsb);
    EnableWindow(state.usbStop, state.trackingUsb);
}

void CheckTracking(AppState& state) {
    const std::wstring monitorSignature = DisplaySignature(state.snapshot);
    const std::wstring usbSignature = UsbSignature(state.snapshot);
    if (state.trackingMonitor && monitorSignature != state.previousMonitorSignature) {
        WriteTrackingLog(Tracker::Monitor, TrackingEvent::Change, &state.snapshot);
        state.previousMonitorSignature = monitorSignature;
    }
    if (state.trackingUsb && usbSignature != state.previousUsbSignature) {
        WriteTrackingLog(Tracker::UsbTree, TrackingEvent::Change, &state.snapshot);
        state.previousUsbSignature = usbSignature;
    }
}

void SetPage(AppState& state, bool tracking) {
    ShowWindow(state.livePage, tracking ? SW_HIDE : SW_SHOW);
    ShowWindow(state.trackingPage, tracking ? SW_SHOW : SW_HIDE);
    SetWindowTextW(state.liveNav, tracking ? L"  Live View" : L"●  Live View");
    SetWindowTextW(state.trackingNav, tracking ? L"●  Tracking View" : L"  Tracking View");
}

void CreatePages(AppState& state, HINSTANCE instance) {
    state.livePage = MakeControl(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, state.content, 0, instance);
    state.trackingPage = MakeControl(L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, state.content, 0, instance);
    state.liveHeading = MakeControl(L"STATIC", L"Live hardware overview", WS_CHILD | WS_VISIBLE,
                                    24, 20, 800, 28, state.livePage, 0, instance);
    state.displayList = MakeControl(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                    LVS_SHOWSELALWAYS | WS_TABSTOP, 24, 58, 800, 170, state.livePage,
                                    DisplayListId, instance);
    ListView_SetExtendedListViewStyle(state.displayList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    AddListColumn(state.displayList, 0, 190, L"Monitor");
    AddListColumn(state.displayList, 1, 90, L"State");
    AddListColumn(state.displayList, 2, 150, L"Mode");
    AddListColumn(state.displayList, 3, 245, L"Adapter");
    AddListColumn(state.displayList, 4, 300, L"Monitor ID");
    state.deviceTree = MakeControl(WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASBUTTONS |
                                   TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | WS_TABSTOP,
                                   24, 260, 520, 400, state.livePage, DeviceTreeId, instance);
    state.deviceDetail = MakeControl(L"EDIT", L"Select a device to inspect its details.",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                     560, 260, 400, 400, state.livePage, DeviceDetailId, instance);
    SetFont(state.liveHeading, state.titleFont);
    SetFont(state.displayList, state.bodyFont);
    SetFont(state.deviceTree, state.bodyFont);
    SetFont(state.deviceDetail, state.bodyFont);
    state.monitorStart = MakeControl(L"BUTTON", L"Start monitor tracking", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     24, 80, 210, 34, state.trackingPage, MonitorStartId, instance);
    state.monitorStop = MakeControl(L"BUTTON", L"Stop monitor tracking", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    244, 80, 200, 34, state.trackingPage, MonitorStopId, instance);
    state.usbStart = MakeControl(L"BUTTON", L"Start USB tree tracking", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 24, 150, 210, 34, state.trackingPage, UsbStartId, instance);
    state.usbStop = MakeControl(L"BUTTON", L"Stop USB tree tracking", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                244, 150, 200, 34, state.trackingPage, UsbStopId, instance);
    state.trackingStatus = MakeControl(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 24, 220, 800, 50,
                                       state.trackingPage, 0, instance);
    state.trackingOutput = MakeControl(L"EDIT", L"Tracking writes a full diagnostic snapshot when a change is detected.",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                                       24, 290, 800, 250, state.trackingPage, 0, instance);
    SetFont(state.trackingStatus, state.bodyFont);
    SetFont(state.trackingOutput, state.bodyFont);
    SetPage(state, false);
    UpdateTrackingStatus(state);
}

void Layout(AppState& state, int width, int height) {
    MoveWindow(state.content, 224, 72, width - 244, height - 92, TRUE);
    MoveWindow(state.livePage, 0, 0, width - 244, height - 92, TRUE);
    MoveWindow(state.trackingPage, 0, 0, width - 244, height - 92, TRUE);
    MoveWindow(state.liveHeading, 24, 20, width - 292, 28, TRUE);
    MoveWindow(state.displayList, 24, 58, width - 292, 170, TRUE);
    MoveWindow(state.deviceTree, 24, 260, (width - 292) * 55 / 100, height - 328, TRUE);
    MoveWindow(state.deviceDetail, 40 + (width - 292) * 55 / 100, 260,
               (width - 292) * 45 / 100 - 16, height - 328, TRUE);
    MoveWindow(state.trackingStatus, 24, 220, width - 292, 50, TRUE);
    MoveWindow(state.trackingOutput, 24, 290, width - 292, height - 378, TRUE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE: {
        state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        state->window = window;
        state->titleFont = MakeFont(22, true);
        state->bodyFont = MakeFont(14);
        HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;
        state->liveNav = MakeControl(L"BUTTON", L"●  Live View", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                     20, 90, 180, 42, window, LivePageId, instance);
        state->trackingNav = MakeControl(L"BUTTON", L"  Tracking View", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         20, 140, 180, 42, window, TrackingPageId, instance);
        state->content = MakeControl(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 224, 72, 800, 600, window, 0, instance);
        CreatePages(*state, instance);
        state->watcher.Start(window, [state]() { RequestCapture(*state); });
        StartCaptureWorker(*state);
        return 0;
    }
    case WM_SIZE:
        if (state != nullptr) {
            Layout(*state, LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_REFRESH:
        if (state != nullptr) {
            if (ConsumePendingSnapshot(*state)) {
                PopulateLiveControls(*state);
                CheckTracking(*state);
            }
        }
        return 0;
    case WM_COMMAND:
        if (state == nullptr) {
            return 0;
        }
        switch (LOWORD(wParam)) {
        case LivePageId:
            SetPage(*state, false);
            break;
        case TrackingPageId:
            SetPage(*state, true);
            break;
        case MonitorStartId:
            state->trackingMonitor = true;
            state->previousMonitorSignature = DisplaySignature(state->snapshot);
            WriteTrackingLog(Tracker::Monitor, TrackingEvent::Started, &state->snapshot);
            UpdateTrackingStatus(*state);
            break;
        case MonitorStopId:
            state->trackingMonitor = false;
            WriteTrackingLog(Tracker::Monitor, TrackingEvent::Stopped, nullptr);
            UpdateTrackingStatus(*state);
            break;
        case UsbStartId:
            state->trackingUsb = true;
            state->previousUsbSignature = UsbSignature(state->snapshot);
            WriteTrackingLog(Tracker::UsbTree, TrackingEvent::Started, &state->snapshot);
            UpdateTrackingStatus(*state);
            break;
        case UsbStopId:
            state->trackingUsb = false;
            WriteTrackingLog(Tracker::UsbTree, TrackingEvent::Stopped, nullptr);
            UpdateTrackingStatus(*state);
            break;
        }
        return 0;
    case WM_NOTIFY:
        if (state != nullptr) {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->idFrom == DeviceTreeId && header->code == TVN_SELCHANGEDW) {
                const auto* selection = reinterpret_cast<const NMTREEVIEWW*>(lParam);
                const auto* device = reinterpret_cast<const DeviceRecord*>(selection->itemNew.lParam);
                if (device != nullptr) {
                    SetWindowTextW(state->deviceDetail, DeviceDetails(*device).c_str());
                }
            }
        }
        return 0;
    case WM_DEVICECHANGE:
        if (state != nullptr && (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE ||
                                 wParam == DBT_DEVNODES_CHANGED)) {
            RequestCapture(*state);
        }
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, TextColor);
        SetBkColor(dc, PanelColor);
        return reinterpret_cast<LRESULT>(CreateSolidBrush(PanelColor));
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(BackgroundColor);
        FillRect(dc, &client, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, TextColor);
        SelectObject(dc, state->titleFont);
        constexpr wchar_t title[] = L"DOCK DEBUG";
        TextOutW(dc, 224, 22, title, lstrlenW(title));
        SIZE titleSize{};
        GetTextExtentPoint32W(dc, title, lstrlenW(title), &titleSize);
        SelectObject(dc, state->bodyFont);
        SetTextColor(dc, MutedColor);
        constexpr wchar_t subtitle[] = L"Dock diagnostics";
        TextOutW(dc, 224 + titleSize.cx + 12, 27, subtitle, lstrlenW(subtitle));
        HBRUSH sidebar = CreateSolidBrush(SidebarColor);
        RECT sideRect{0, 0, 220, client.bottom};
        FillRect(dc, &sideRect, sidebar);
        DeleteObject(sidebar);
        SetTextColor(dc, AccentColor);
        SelectObject(dc, state->titleFont);
        constexpr wchar_t brand[] = L"Dock-Debug";
        TextOutW(dc, 20, 24, brand, lstrlenW(brand));
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        if (state != nullptr) {
            state->watcher.Stop();
            StopCaptureWorker(*state);
            DeleteObject(state->titleFont);
            DeleteObject(state->bodyFont);
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);

    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = WindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&windowClass);

    AppState state;
    HWND window = CreateWindowExW(0, WindowClassName, L"Dock-Debug",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  1180, 760, nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
