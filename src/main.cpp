// PhoneLinkLock
//
// Launches the Windows "Phone Link" app, forces its window fullscreen and
// on top, strips the close/minimize/system-menu controls, and (optionally)
// blocks a few keyboard escape hatches -- until UI Automation detects text
// in the window indicating the phone has finished linking, at which point
// it hands normal control back.
//
// Runs as a WIN32-subsystem (no console) background process. See README.md
// for how to register it as a per-user autostart "daemon" via Task
// Scheduler, which is the correct Windows analogue here: a real SYSTEM
// service cannot touch your desktop's windows (Session 0 isolation).

#include <windows.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cwctype>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

namespace {

// ---------------------------------------------------------------------
// Configuration -- adjust these to taste.
// ---------------------------------------------------------------------

// Window-title / process-name fragments used to find the Phone Link window.
const wchar_t* kTitleFragment1  = L"Phone Link";
const wchar_t* kTitleFragment2  = L"Your Phone";
const wchar_t* kProcessName1    = L"PhoneExperienceHost.exe";
const wchar_t* kProcessName2    = L"YourPhone.exe";

// Protocol used to launch Phone Link. Falls back to the Start Menu app id
// if the protocol handler isn't registered.
const wchar_t* kLaunchProtocol  = L"ms-phone:";

// Substrings (lower-cased) we look for in the UI tree to decide the phone
// is linked. Phone Link's exact wording varies by version/region, so you
// will likely want to watch it run once and tune this list.
const std::vector<std::wstring> kLinkedKeywords = {
    L"is linked",
    L"successfully linked",
    L"your phone is connected",
    L"device connected",
};

const int  kWatchdogIntervalMs   = 1500;  // re-apply lock / find window
const int  kUiaPollIntervalMs    = 2000;  // scan UI tree for "linked" text
const int  kUiaMaxDepth          = 6;     // how deep to walk the UI tree
const bool kBlockEscapeHotkeys   = true;  // block Win+D / Win+M / Alt+F4 while locked

std::atomic<bool> g_locked{true};
std::atomic<bool> g_shuttingDown{false};
std::atomic<HWND> g_targetWindow{nullptr};
HHOOK g_keyboardHook = nullptr;
HHOOK g_mouseHook = nullptr;
NOTIFYICONDATAW g_trayIcon{};
HWND g_messageWindow = nullptr;

const wchar_t* kWindowClassName = L"PhoneLinkLockMessageWindow";
const UINT WM_TRAYICON = WM_APP + 1;

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                    [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// Launches Phone Link. Uses CreateProcessAsUserW into the active console session
// to ensure it works even if this app is launched as SYSTEM (e.g. by a service).
bool LaunchPhoneLink() {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId != 0xFFFFFFFF) {
        HANDLE hUserToken = nullptr;
        if (WTSQueryUserToken(sessionId, &hUserToken)) {
            HANDLE hPrimaryToken = nullptr;
            if (DuplicateTokenEx(hUserToken, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &hPrimaryToken)) {
                void* envBlock = nullptr;
                if (CreateEnvironmentBlock(&envBlock, hPrimaryToken, FALSE)) {
                    STARTUPINFOW si = { sizeof(si) };
                    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
                    PROCESS_INFORMATION pi = {};
                    
                    wchar_t cmd[] = L"explorer.exe shell:AppsFolder\\Microsoft.YourPhone_8wekyb3d8bbwe!App";
                    
                    if (CreateProcessAsUserW(hPrimaryToken, nullptr, cmd, nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, envBlock, nullptr, &si, &pi)) {
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                        DestroyEnvironmentBlock(envBlock);
                        CloseHandle(hPrimaryToken);
                        CloseHandle(hUserToken);
                        return true;
                    }
                    DestroyEnvironmentBlock(envBlock);
                }
                CloseHandle(hPrimaryToken);
            }
            CloseHandle(hUserToken);
        }
    }

    // Fallback: standard ShellExecute (works fine if launched by the normal user)
    HINSTANCE result = ShellExecuteW(nullptr, L"open", kLaunchProtocol,
                                      nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) > 32) {
        return true;
    }
    
    result = ShellExecuteW(nullptr, L"open",
                            L"shell:AppsFolder\\Microsoft.YourPhone_8wekyb3d8bbwe!App",
                            nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

// Automatically installs the executable to %LocalAppData%\PhoneLinkLock if it's
// not already running from there.
void InstallIfNeeded() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring currentExe = exePath;

    wchar_t* localAppDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppDataPath))) {
        std::wstring targetDir = std::wstring(localAppDataPath) + L"\\PhoneLinkLock";
        
        // Find filename
        size_t lastSlash = currentExe.find_last_of(L"\\/");
        std::wstring fileName = (lastSlash != std::wstring::npos) ? currentExe.substr(lastSlash + 1) : L"PhoneLinkLock.exe";
        std::wstring targetExe = targetDir + L"\\" + fileName;
        
        CoTaskMemFree(localAppDataPath);

        // If we're not running from the target directory, install and respawn
        if (ToLower(currentExe) != ToLower(targetExe)) {
            CreateDirectoryW(targetDir.c_str(), nullptr);
            if (CopyFileW(currentExe.c_str(), targetExe.c_str(), FALSE)) {
                // Launch the newly installed copy
                ShellExecuteW(nullptr, L"open", targetExe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                ExitProcess(0);
            }
        }
    }
}

struct FindWindowContext {
    HWND found = nullptr;
};

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<FindWindowContext*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 255);
    std::wstring wtitle(title);

    bool titleMatches = (wtitle.find(kTitleFragment1) != std::wstring::npos) ||
                        (wtitle.find(kTitleFragment2) != std::wstring::npos);

    bool processMatches = false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            wchar_t exeName[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, exeName, &size)) {
                std::wstring path(exeName);
                processMatches = (path.find(kProcessName1) != std::wstring::npos) ||
                                 (path.find(kProcessName2) != std::wstring::npos);
            }
            CloseHandle(hProc);
        }
    }

    if (titleMatches || processMatches) {
        ctx->found = hwnd;
        return FALSE; // stop enumerating
    }
    return TRUE;
}

HWND FindPhoneLinkWindow() {
    FindWindowContext ctx;
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// Strips the system menu / minimize / maximize / resize border so the
// window can't be minimized, resized, or closed via its own chrome, then
// pins it fullscreen and topmost.
void ApplyLockStyle(HWND hwnd) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CAPTION);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle |= WS_EX_TOPMOST;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfoW(monitor, &mi);

    SetWindowPos(hwnd, HWND_TOPMOST,
                 mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_SHOWWINDOW | SWP_FRAMECHANGED);

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }

    // Force "extreme top" by bypassing Windows foreground restrictions
    HWND hForeground = GetForegroundWindow();
    if (hForeground != hwnd && hForeground != nullptr) {
        DWORD fgThread = GetWindowThreadProcessId(hForeground, nullptr);
        DWORD myThread = GetCurrentThreadId();
        if (fgThread != myThread && fgThread != 0) {
            AttachThreadInput(myThread, fgThread, TRUE);
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
            AttachThreadInput(myThread, fgThread, FALSE);
        } else {
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
        }
    } else {
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
    }
}

// Restores normal window chrome once the phone is linked.
void ApplyUnlockStyle(HWND hwnd) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style |= (WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CAPTION);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle &= ~WS_EX_TOPMOST;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
}

void KillExplorer() {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[] = L"taskkill.exe /F /IM explorer.exe";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void StartExplorer() {
    // Don't start it if it's already running (taskbar exists)
    if (FindWindowW(L"Shell_TrayWnd", nullptr)) return;

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[] = L"explorer.exe";
    // We launch explorer.exe. No CREATE_NO_WINDOW because it's a GUI app.
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

// ---------------------------------------------------------------------
// UI Automation: walk the window's UI tree looking for "linked" text.
// ---------------------------------------------------------------------

bool ElementTreeContainsLinkedText(IUIAutomation* uia, IUIAutomationElement* element, int depth) {
    if (!element || depth > kUiaMaxDepth) return false;

    BSTR name = nullptr;
    if (SUCCEEDED(element->get_CurrentName(&name)) && name) {
        std::wstring lower = ToLower(name);
        SysFreeString(name);
        for (const auto& kw : kLinkedKeywords) {
            if (lower.find(kw) != std::wstring::npos) return true;
        }
    }

    IUIAutomationTreeWalker* walker = nullptr;
    uia->get_ControlViewWalker(&walker);
    if (!walker) return false;

    IUIAutomationElement* child = nullptr;
    walker->GetFirstChildElement(element, &child);
    bool found = false;
    while (child && !found) {
        found = ElementTreeContainsLinkedText(uia, child, depth + 1);
        IUIAutomationElement* next = nullptr;
        walker->GetNextSiblingElement(child, &next);
        child->Release();
        child = next;
    }
    walker->Release();
    return found;
}

bool CheckIfLinked(HWND hwnd) {
    IUIAutomation* uia = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IUIAutomation, reinterpret_cast<void**>(&uia)))) {
        return false;
    }
    IUIAutomationElement* root = nullptr;
    bool linked = false;
    if (SUCCEEDED(uia->ElementFromHandle(hwnd, &root)) && root) {
        linked = ElementTreeContainsLinkedText(uia, root, 0);
        root->Release();
    }
    uia->Release();
    return linked;
}

// ---------------------------------------------------------------------
// Keyboard hook: block a few common ways to escape the locked window.
// ---------------------------------------------------------------------

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_locked.load()) {
        auto* kbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        
        // KILL SWITCH: If the user hits ESCAPE, unlock and exit immediately.
        if (wParam == WM_KEYDOWN && kbd->vkCode == VK_ESCAPE) {
            PostMessageW(g_messageWindow, WM_DESTROY, 0, 0);
            return 1;
        }

        // Block all other keyboard input while the screen is locked
        return 1;
    }
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_locked.load()) {
        // Block scrolling and horizontal scrolling (two-finger swipes on trackpad)
        if (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL) {
            return 1;
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------
// Background threads
// ---------------------------------------------------------------------

void WatchdogThread() {
    while (!g_shuttingDown.load()) {
        HWND hwnd = g_targetWindow.load();

        if (!hwnd || !IsWindow(hwnd)) {
            // Window not found (or was closed). If we're still supposed to
            // be locked, relaunch Phone Link and keep looking.
            if (g_locked.load()) {
                StartExplorer();
                LaunchPhoneLink();
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                hwnd = FindPhoneLinkWindow();
                g_targetWindow.store(hwnd);
                KillExplorer();
            }
        }

        if (hwnd && IsWindow(hwnd) && g_locked.load()) {
            ApplyLockStyle(hwnd); // re-assert in case the app or OS reset it
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // check more aggressively than 1.5s
    }
}

void UiaPollThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!g_shuttingDown.load()) {
        if (g_locked.load()) {
            HWND hwnd = g_targetWindow.load();
            if (hwnd && IsWindow(hwnd) && CheckIfLinked(hwnd)) {
                g_locked.store(false);
                ApplyUnlockStyle(hwnd);
                StartExplorer();
                // Update tray icon tooltip to reflect the unlocked state.
                wcscpy_s(g_trayIcon.szTip, L"PhoneLinkLock - phone linked, unlocked");
                Shell_NotifyIconW(NIM_MODIFY, &g_trayIcon);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kUiaPollIntervalMs));
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------
// Tray icon + hidden message window (so the process is visible/quittable
// without a console or taskbar entry).
// ---------------------------------------------------------------------

void AddTrayIcon(HWND hwnd) {
    g_trayIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_trayIcon.hWnd = hwnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayIcon.szTip, L"PhoneLinkLock - waiting for phone to link");
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON:
            // Tray menu exit option removed to prevent quitting the lock app.
            return 0;
        case WM_DESTROY:
            g_shuttingDown.store(true);
            StartExplorer();
            if (HWND hwnd = g_targetWindow.load()) {
                ApplyUnlockStyle(hwnd);
            }
            Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
            if (g_keyboardHook) UnhookWindowsHookEx(g_keyboardHook);
            if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Auto-install to LocalAppData if not already there.
    InstallIfNeeded();

    // Single-instance guard.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\PhoneLinkLock_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kWindowClassName;
    RegisterClassW(&wc);

    // Message-only-ish hidden window: needed as a target for tray-icon
    // messages and to host the keyboard hook's message loop.
    g_messageWindow = CreateWindowExW(0, kWindowClassName, L"PhoneLinkLock",
                                       0, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    AddTrayIcon(g_messageWindow);

    LaunchPhoneLink();
    Sleep(2000);
    g_targetWindow.store(FindPhoneLinkWindow());

    if (kBlockEscapeHotkeys) {
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hInstance, 0);
        KillExplorer();
    }

    if (HWND hwnd = g_targetWindow.load()) {
        ApplyLockStyle(hwnd);
    }

    std::thread watchdog(WatchdogThread);
    std::thread uiaPoll(UiaPollThread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_shuttingDown.store(true);
    if (watchdog.joinable()) watchdog.join();
    if (uiaPoll.joinable()) uiaPoll.join();

    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
