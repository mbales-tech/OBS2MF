// OBS2MF Broker / Tray application.
//
// Responsibilities:
//   * Host the MFCreateVirtualCamera "OBS2MF Camera" (Session lifetime, current user).
//   * Capture the "OBS Virtual Camera" via IMFSourceReader (NV12 @ 1280x720).
//   * Generate an animated NV12 test pattern (fallback / manual).
//   * Serve the newest frame to the media source (running in the Frame Server) over a
//     named pipe (see Vcam.Common/ipc.h).
//   * Provide a system-tray UI: start/stop camera, switch source, open log, status, exit.
//
// Portions derived from smourier/VCamSample (MIT).

#include "framework.h"
#include "tools.h"
#include "Resource.h"
#include "ObsCapture.h"

#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include <exception>
#include <cstdlib>
#include <intrin.h>
#include <psapi.h>
#include <dbghelp.h>

#pragma comment(lib, "psapi")
#pragma comment(lib, "dbghelp")

using namespace obs2mf;

static constexpr UINT kW = 1280;
static constexpr UINT kH = 720;
static constexpr size_t kFrameBytes = (size_t)kW * kH * 3 / 2;

#define WM_TRAYICON (WM_APP + 1)
enum {
    ID_TOGGLE_CAM = 1001,
    ID_SRC_OBS,
    ID_SRC_TEST,
    ID_AUTOSTART,
    ID_OPENLOG,
    ID_EXIT,
    ID_STATUS,
};

static Logger g_log;
static FramePipeServer g_pipe;
static wil::com_ptr_nothrow<IMFVirtualCamera> g_vcam;
static std::atomic<int> g_source{ SourceOBS };
static std::atomic<bool> g_camRunning{ false };
static std::atomic<bool> g_quit{ false };
static std::thread g_producer;
static std::thread g_telemetry;
static HWND g_hwnd;
static HINSTANCE g_inst;
static NOTIFYICONDATAW g_nid{};

// ---- telemetry counters (read by the health/telemetry thread) ----------------
static std::atomic<unsigned long long> g_framesPublished{ 0 };
static std::atomic<unsigned long long> g_obsFramesRcvd{ 0 };
static std::atomic<int> g_effectiveSource{ SourceNone };
static std::wstring g_dumpBase; // "<logdir>\broker"; crash dumps land next to the log

// ---- diagnostics: health snapshot + crash handling ---------------------------
// A single health line so a long session (or a crash) shows whether the process is
// leaking. Resource growth over hours here is the signal we were previously blind to.
static void LogHealth(const wchar_t* tag)
{
    PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    DWORD usr = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
    g_log.Logf(L"%s priv=%.1fMB ws=%.1fMB gdi=%lu user=%lu handles=%lu | sysUsed=%lu%% sysFree=%lluMB | "
               L"clients=%d cam=%d src=%d obsFrames=%llu published=%llu",
        tag,
        pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0,
        gdi, usr, handles,
        ms.dwMemoryLoad, ms.ullAvailPhys / (1024ull * 1024ull),
        g_pipe.ClientCount(), g_camRunning.load() ? 1 : 0, g_effectiveSource.load(),
        g_obsFramesRcvd.load(), g_framesPublished.load());
}

static void WriteMiniDump(EXCEPTION_POINTERS* ep, const wchar_t* reason)
{
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s-%04u%02u%02u-%02u%02u%02u.dmp",
        g_dumpBase.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        auto type = (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                    MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, type,
            ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(h);
        g_log.Logf(L"FATAL: %s; minidump -> %s", reason, path);
    }
    else
    {
        g_log.Logf(L"FATAL: %s; (minidump write failed, err %lu)", reason, GetLastError());
    }
}

// Top-level SEH filter: catches access violations, stack overflows, and unhandled C++
// exceptions (which surface here as 0xE06D7363). Writes a dump + FATAL line, then ends.
static LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep)
{
    DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    wchar_t reason[160];
    _snwprintf_s(reason, _countof(reason), _TRUNCATE, L"unhandled exception 0x%08X at 0x%p", code, addr);
    WriteMiniDump(ep, reason); // dump first: most important artifact if the heap is corrupt
    LogHealth(L"crash-state:");
    return EXCEPTION_EXECUTE_HANDLER; // terminate the process (no WER dialog)
}

// The CRT failure hooks (terminate/purecall/invalid-parameter) don't hand us
// EXCEPTION_POINTERS, so synthesize a context for the dump, then end the process.
static void CaptureAndDump(const wchar_t* reason)
{
    CONTEXT ctx{}; RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{}; rec.ExceptionCode = 0xE0000001; rec.ExceptionAddress = _ReturnAddress();
    EXCEPTION_POINTERS ep{ &rec, &ctx };
    WriteMiniDump(&ep, reason); // dump first: most important artifact if the heap is corrupt
    LogHealth(L"crash-state:");
}
static void OnTerminate() { CaptureAndDump(L"std::terminate"); TerminateProcess(GetCurrentProcess(), 3); }
static void OnPureCall()  { CaptureAndDump(L"pure virtual call"); TerminateProcess(GetCurrentProcess(), 3); }
static void OnInvalidParam(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
{ CaptureAndDump(L"CRT invalid parameter"); TerminateProcess(GetCurrentProcess(), 3); }

static void InstallCrashHandlers()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(OnUnhandledException);
    std::set_terminate(OnTerminate);
    _set_purecall_handler(OnPureCall);
    _set_invalid_parameter_handler(OnInvalidParam);
}

static void LogEnvironment()
{
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms);
    DWORD major = 0, minor = 0, build = 0;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
    {
        typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        if (auto p = (RtlGetVersionPtr)GetProcAddress(nt, "RtlGetVersion"))
        {
            RTL_OSVERSIONINFOW vi{}; vi.dwOSVersionInfoSize = sizeof(vi);
            if (p(&vi) == 0) { major = vi.dwMajorVersion; minor = vi.dwMinorVersion; build = vi.dwBuildNumber; }
        }
    }
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_log.Logf(L"env: Windows %lu.%lu build %lu | RAM %lluMB total, %lluMB free (%lu%% used) | pid %lu | %s",
        major, minor, build,
        ms.ullTotalPhys / (1024ull * 1024ull), ms.ullAvailPhys / (1024ull * 1024ull), ms.dwMemoryLoad,
        GetCurrentProcessId(), exe);
}

// Periodic health + immediate consumer connect/disconnect logging.
static void TelemetryThread()
{
    int lastClients = 0;
    unsigned sec = 0;
    LogHealth(L"health:");
    while (!g_quit.load())
    {
        for (int i = 0; i < 10 && !g_quit.load(); ++i) Sleep(100); // ~1s, quit-responsive
        if (g_quit.load()) break;

        int clients = g_pipe.ClientCount();
        if (clients != lastClients)
        {
            g_log.Logf(L"consumers streaming: %d (was %d)", clients, lastClients);
            lastClients = clients;
        }
        if (++sec % 60 == 0) LogHealth(L"health:");
    }
}

// ---- test pattern ------------------------------------------------------------
static void RgbToYuv(int r, int g, int b, BYTE& Y, BYTE& U, BYTE& V)
{
    int y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
    Y = (BYTE)std::clamp(y, 0, 255);
    U = (BYTE)std::clamp(u, 0, 255);
    V = (BYTE)std::clamp(v, 0, 255);
}

static void FillTestPatternNV12(std::vector<BYTE>& buf, uint32_t frame)
{
    static const int bars[8][3] = {
        {235,235,235},{235,235,16},{16,235,235},{16,235,16},
        {235,16,235},{235,16,16},{16,16,235},{16,16,16},
    };
    BYTE* Yp = buf.data();
    BYTE* UV = buf.data() + (size_t)kW * kH;
    const int barW = kW / 8;
    const int scroll = (int)(frame * 4);
    for (int by = 0; by < (int)kH / 2; by++)
    {
        for (int bx = 0; bx < (int)kW / 2; bx++)
        {
            int x = bx * 2;
            int idx = ((x + scroll) / barW) % 8;
            BYTE Y, U, V;
            RgbToYuv(bars[idx][0], bars[idx][1], bars[idx][2], Y, U, V);
            size_t px = (size_t)by * 2 * kW + x;
            Yp[px] = Y; Yp[px + 1] = Y; Yp[px + kW] = Y; Yp[px + kW + 1] = Y;
            size_t uv = (size_t)by * kW + x;
            UV[uv] = U; UV[uv + 1] = V;
        }
    }
}

// ---- OBS capture -------------------------------------------------------------
// OBS Virtual Camera is a DirectShow-only softcam (invisible to Media Foundation),
// so it is captured via a DirectShow graph in ObsCapture (ObsCapture.cpp). This hook
// lets that module log through the broker's logger.
void ObsLog(const wchar_t* msg) { g_log.Logf(L"%s", msg); }

// ---- producer ----------------------------------------------------------------
static void ProducerThread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ObsCapture obs(kW, kH);
    std::vector<BYTE> frame(kFrameBytes);
    uint32_t seq = 0;
    int retryDelay = 0; // frames until we retry opening OBS (throttle log spam)
    int lastReported = -1;

    while (!g_quit.load())
    {
        int src = g_source.load();
        int reported = src;

        if (src == SourceOBS)
        {
            if (!obs.Running())
            {
                if (retryDelay <= 0) { if (!obs.Start()) retryDelay = 60; }
                else retryDelay--;
            }
            if (!(obs.Running() && obs.GetFrame(frame)))
            {
                FillTestPatternNV12(frame, seq); // auto fallback (OBS unavailable / no frame yet)
                reported = SourceTest;
            }
        }
        else // SourceTest
        {
            if (obs.Running()) obs.Stop();
            retryDelay = 0; // re-arm immediate retry next time OBS is selected
            FillTestPatternNV12(frame, seq);
        }

        // Log what the consumer is actually getting whenever it changes (OBS going live,
        // auto-falling back to the test pattern, or a manual switch to Test).
        if (reported != lastReported)
        {
            if (src == SourceOBS && reported == SourceOBS)
                g_log.Logf(L"source: OBS Virtual Camera live");
            else if (src == SourceOBS && reported == SourceTest)
                g_log.Logf(L"source: OBS unavailable - serving test pattern (auto-fallback)");
            else
                g_log.Logf(L"source: test pattern");
            lastReported = reported;
        }

        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        FrameHeader hdr{};
        hdr.fourcc = kFourccNV12;
        hdr.width = kW;
        hdr.height = kH;
        hdr.stride = kW;
        hdr.byteLength = (uint32_t)kFrameBytes;
        hdr.sequence = seq++;
        hdr.timestampQpc = (uint64_t)now.QuadPart;
        hdr.sourceKind = (uint32_t)reported;
        hdr.flags = flag_valid;
        g_pipe.Publish(hdr, frame.data());

        g_effectiveSource.store(reported);
        g_obsFramesRcvd.store(obs.FramesReceived());
        g_framesPublished.fetch_add(1);

        Sleep(33);
    }
    obs.Stop();
    CoUninitialize();
}

// ---- virtual camera lifetime -------------------------------------------------
static HRESULT StartCamera()
{
    if (g_camRunning.load()) return S_OK;
    auto clsid = GUID_ToStringW(CLSID_OBS2MFCamera);
    HRESULT hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser,
        OBS2MF_CAMERA_FRIENDLY_NAME,
        clsid.c_str(),
        nullptr, 0, &g_vcam);
    if (FAILED(hr))
    {
        g_log.Logf(L"MFCreateVirtualCamera failed 0x%08X (is Vcam.MediaSource.dll registered in HKLM?)", hr);
        return hr;
    }
    hr = g_vcam->Start(nullptr);
    if (FAILED(hr))
    {
        g_log.Logf(L"IMFVirtualCamera::Start failed 0x%08X", hr);
        g_vcam.reset();
        return hr;
    }
    g_camRunning.store(true);
    g_log.Logf(L"Virtual camera '%s' started", OBS2MF_CAMERA_FRIENDLY_NAME);
    return S_OK;
}

static void StopCamera()
{
    if (!g_camRunning.load()) return;
    if (g_vcam)
    {
        g_vcam->Remove();
        g_vcam.reset();
    }
    g_camRunning.store(false);
    g_log.Logf(L"Virtual camera stopped");
}

// ---- auto-start (per-user HKCU\...\Run; broker runs non-elevated) --------------
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunVal = L"OBS2MF";

static bool IsAutoStartEnabled()
{
    HKEY h; bool on = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &h) == ERROR_SUCCESS)
    {
        DWORD type = 0, sz = 0;
        if (RegQueryValueExW(h, kRunVal, nullptr, &type, nullptr, &sz) == ERROR_SUCCESS && type == REG_SZ)
            on = true;
        RegCloseKey(h);
    }
    return on;
}

static void SetAutoStart(bool enable)
{
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return;
    if (enable)
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring q = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(h, kRunVal, 0, REG_SZ, (const BYTE*)q.c_str(), (DWORD)((q.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(h, kRunVal);
    }
    RegCloseKey(h);
    g_log.Logf(L"Auto-start %s", enable ? L"enabled" : L"disabled");
}

// ---- tray --------------------------------------------------------------------
static void ShowTrayMenu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_camRunning.load() ? MF_CHECKED : 0), ID_TOGGLE_CAM,
        g_camRunning.load() ? L"Camera active (click to stop)" : L"Camera stopped (click to start)");
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    int s = g_source.load();
    AppendMenuW(m, MF_STRING | (s == SourceOBS ? MF_CHECKED : 0), ID_SRC_OBS, L"Source: OBS Virtual Camera");
    AppendMenuW(m, MF_STRING | (s == SourceTest ? MF_CHECKED : 0), ID_SRC_TEST, L"Source: Test pattern");
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    AppendMenuW(m, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    wchar_t st[128];
    wsprintfW(st, L"Consumers streaming: %d", g_pipe.ClientCount());
    AppendMenuW(m, MF_STRING | MF_GRAYED, ID_STATUS, st);
    AppendMenuW(m, MF_STRING, ID_OPENLOG, L"Open log file");
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");

    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == WM_LBUTTONUP)
            ShowTrayMenu(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_TOGGLE_CAM:
            if (g_camRunning.load()) StopCamera(); else StartCamera();
            break;
        case ID_SRC_OBS:  g_source.store(SourceOBS);  g_log.Logf(L"Source -> OBS"); break;
        case ID_SRC_TEST: g_source.store(SourceTest); g_log.Logf(L"Source -> Test"); break;
        case ID_AUTOSTART: SetAutoStart(!IsAutoStartEnabled()); break;
        case ID_OPENLOG:
            ShellExecuteW(nullptr, L"open", g_log.Path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case ID_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    // single instance
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"OBS2MF.Broker.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    g_inst = hInstance;
    WinTraceRegister();

    // Fresh, uniquely-named log per launch under %LOCALAPPDATA%\OBS2MF\logs (old ones pruned),
    // so files never grow unbounded or get overwritten. Crash dumps land in the same folder.
    std::wstring logDir = KnownFolderPath(FOLDERID_LocalAppData, L"OBS2MF\\logs");
    g_log.InitSession(logDir, L"broker", 15);
    g_dumpBase = logDir + L"\\broker";
    InstallCrashHandlers();

    g_log.Logf(L"OBS2MF broker %s starting", OBS2MF_VERSION_STRING);
    LogEnvironment();

    if (FAILED(MFStartup(MF_VERSION)))
    {
        g_log.Logf(L"MFStartup failed");
        return 1;
    }
    winrt::init_apartment();

    // hidden message window
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"OBS2MF_Broker";
    RegisterClassW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"OBS2MF", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);

    // tray icon
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_VCAMSAMPLE));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"OBS2MF Virtual Camera");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // start frame server + producer, then create the camera
    g_pipe.Start();
    g_producer = std::thread(ProducerThread);
    g_telemetry = std::thread(TelemetryThread);
    StartCamera();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // shutdown
    g_quit.store(true);
    if (g_producer.joinable()) g_producer.join();
    if (g_telemetry.joinable()) g_telemetry.join();
    StopCamera();
    g_pipe.Stop();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);

    g_vcam.reset();
    MFShutdown();
    g_log.Logf(L"OBS2MF broker exiting");
    WinTraceUnregister();
    if (mutex) CloseHandle(mutex);
    return 0;
}
