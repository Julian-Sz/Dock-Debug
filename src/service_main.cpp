#include "monitor.h"

#include <windows.h>

#include <fstream>
#include <string>

namespace {
constexpr wchar_t ServiceName[] = L"DockDebugMonitor";
SERVICE_STATUS_HANDLE statusHandle = nullptr;
HANDLE stopEvent = nullptr;

void WriteEvent(const std::wstring& message) {
    CreateDirectoryW(L"C:\\ProgramData\\Dock-Debug", nullptr);
    std::wofstream log(L"C:\\ProgramData\\Dock-Debug\\monitor.log", std::ios::app);
    if (log) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        log << now.wYear << L'-' << now.wMonth << L'-' << now.wDay << L' '
            << now.wHour << L':' << now.wMinute << L':' << now.wSecond << L' '
            << message << L'\n';
    }
}

void SetServiceState(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = exitCode;
    status.dwWaitHint = waitHint;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    SetServiceStatus(statusHandle, &status);
}

DWORD WINAPI ServiceControlHandler(DWORD control, DWORD, LPVOID, LPVOID) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 3000);
        SetEvent(stopEvent);
    }
    return NO_ERROR;
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    statusHandle = RegisterServiceCtrlHandlerExW(ServiceName, ServiceControlHandler, nullptr);
    if (statusHandle == nullptr) {
        return;
    }

    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent == nullptr) {
        SetServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    SetServiceState(SERVICE_START_PENDING, NO_ERROR, 3000);
    SetServiceState(SERVICE_RUNNING);
    WriteEvent(L"service started");

    while (WaitForSingleObject(stopEvent, 30000) == WAIT_TIMEOUT) {
        const HardwareSnapshot snapshot = CaptureSnapshot();
        WriteEvent(L"snapshot: displays=" + std::to_wstring(snapshot.displays.size()) +
                   L", devices=" + std::to_wstring(snapshot.devices.size()));
    }

    WriteEvent(L"service stopped");
    SetServiceState(SERVICE_STOPPED);
    CloseHandle(stopEvent);
    stopEvent = nullptr;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(ServiceName), ServiceMain},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        return static_cast<int>(GetLastError());
    }
    return 0;
}
