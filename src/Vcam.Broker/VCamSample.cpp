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

#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>

using namespace obs2mf;

static constexpr UINT kW = 1280;
static constexpr UINT kH = 720;
static constexpr size_t kFrameBytes = (size_t)kW * kH * 3 / 2;

#define WM_TRAYICON (WM_APP + 1)
enum {
    ID_TOGGLE_CAM = 1001,
    ID_SRC_OBS,
    ID_SRC_TEST,
    ID_SRC_OFF,
    ID_OPENLOG,
    ID_EXIT,
    ID_STATUS,
};

static Logger g_log;
static FramePipeServer g_pipe;
static wil::com_ptr_nothrow<IMFVirtualCamera> g_vcam;
static std::atomic<int> g_source{ SourceTest };
static std::atomic<bool> g_camRunning{ false };
static std::atomic<bool> g_quit{ false };
static std::thread g_producer;
static HWND g_hwnd;
static HINSTANCE g_inst;
static NOTIFYICONDATAW g_nid{};

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

static void FillBlackNV12(std::vector<BYTE>& buf)
{
    memset(buf.data(), 16, (size_t)kW * kH);
    memset(buf.data() + (size_t)kW * kH, 128, (size_t)kW * kH / 2);
}

// ---- OBS capture -------------------------------------------------------------
class ObsCapture
{
    wil::com_ptr_nothrow<IMFSourceReader> _reader;

public:
    bool Ensure()
    {
        if (_reader) return true;

        wil::com_ptr_nothrow<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(&attrs, 1))) return false;
        attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        if (FAILED(MFEnumDeviceSources(attrs.get(), &devices, &count))) return false;

        wil::com_ptr_nothrow<IMFMediaSource> source;
        for (UINT32 i = 0; i < count; i++)
        {
            if (!source)
            {
                WCHAR* name = nullptr;
                UINT32 len = 0;
                if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len)))
                {
                    if (wcsstr(name, L"OBS Virtual Camera"))
                        devices[i]->ActivateObject(IID_PPV_ARGS(&source));
                    CoTaskMemFree(name);
                }
            }
            devices[i]->Release();
        }
        CoTaskMemFree(devices);
        if (!source)
        {
            g_log.Logf(L"OBS Virtual Camera not found among capture devices");
            return false;
        }

        wil::com_ptr_nothrow<IMFAttributes> ra;
        MFCreateAttributes(&ra, 1);
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (FAILED(MFCreateSourceReaderFromMediaSource(source.get(), ra.get(), &_reader)))
        {
            g_log.Logf(L"MFCreateSourceReaderFromMediaSource failed");
            return false;
        }

        wil::com_ptr_nothrow<IMFMediaType> mt;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(mt.get(), MF_MT_FRAME_SIZE, kW, kH);
        if (FAILED(_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt.get())))
        {
            g_log.Logf(L"OBS reader SetCurrentMediaType(NV12 1280x720) failed");
            _reader.reset();
            return false;
        }
        g_log.Logf(L"OBS Virtual Camera opened @ %ux%u NV12", kW, kH);
        return true;
    }

    bool GetFrame(std::vector<BYTE>& out)
    {
        if (!Ensure()) return false;

        DWORD streamIndex = 0, flags = 0;
        LONGLONG ts = 0;
        wil::com_ptr_nothrow<IMFSample> sample;
        HRESULT hr = _reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
        {
            g_log.Logf(L"OBS ReadSample ended (hr=0x%08X flags=0x%08X); will reopen", hr, flags);
            _reader.reset();
            return false;
        }
        if (!sample) return false; // stream tick / no data this cycle

        wil::com_ptr_nothrow<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
        BYTE* p = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (FAILED(buf->Lock(&p, &maxLen, &curLen))) return false;
        bool ok = curLen >= kFrameBytes;
        if (ok) memcpy(out.data(), p, kFrameBytes);
        buf->Unlock();
        return ok;
    }

    void Close() { _reader.reset(); }
};

// ---- producer ----------------------------------------------------------------
static void ProducerThread()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ObsCapture obs;
    std::vector<BYTE> frame(kFrameBytes);
    uint32_t seq = 0;
    LARGE_INTEGER qpf; QueryPerformanceCounter(&qpf);

    while (!g_quit.load())
    {
        int src = g_source.load();
        int reported = src;
        bool paced = true; // whether we still need to sleep to hit ~30fps

        if (src == SourceOBS)
        {
            if (obs.GetFrame(frame))
            {
                paced = false; // ReadSample already paces us to OBS' frame rate
            }
            else
            {
                FillTestPatternNV12(frame, seq); // auto fallback when OBS unavailable
                reported = SourceTest;
            }
        }
        else if (src == SourceTest)
        {
            FillTestPatternNV12(frame, seq);
        }
        else // SourceOff
        {
            FillBlackNV12(frame);
            obs.Close();
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

        if (paced) Sleep(33);
    }
    obs.Close();
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
    AppendMenuW(m, MF_STRING | (s == SourceOff ? MF_CHECKED : 0), ID_SRC_OFF, L"Source: Off (black)");
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
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
        case ID_SRC_OFF:  g_source.store(SourceOff);  g_log.Logf(L"Source -> Off"); break;
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

    g_log.Init(KnownFolderPath(FOLDERID_LocalAppData, L"OBS2MF\\broker.log"));
    g_log.Logf(L"OBS2MF broker %s starting", OBS2MF_VERSION_STRING);

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
    wcscpy_s(g_nid.szTip, L"OBS2MF Camera");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // start frame server + producer, then create the camera
    g_pipe.Start();
    g_producer = std::thread(ProducerThread);
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
