// All DirectShow + <initguid.h> usage is isolated here (PImpl), away from the Media
// Foundation GUID definitions pulled in by framework.h in the rest of the broker.
#include <windows.h>
#include <dshow.h>
#include <initguid.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include "ObsCapture.h"

#pragma comment(lib, "strmiids")
#pragma comment(lib, "ole32")
#pragma comment(lib, "oleaut32")

// Broker-provided log hook (implemented in VCamSample.cpp) so this module stays decoupled.
extern void ObsLog(const wchar_t* msg);
static void LogF(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list a; va_start(a, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a);
    va_end(a);
    ObsLog(buf);
}

// ---- Sample Grabber declarations (qedit.h was removed from the modern SDK, but the
// qedit.dll filter and these GUIDs remain stable on Windows 11) ----------------
interface ISampleGrabberCB : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE*, long) = 0;
};

interface ISampleGrabber : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB*, long) = 0;
};

// Locally named GUIDs to avoid any clash with strmiids.lib on a given SDK.
DEFINE_GUID(SG_CLSID_SampleGrabber, 0xC1F400A0, 0x3F08, 0x11d3, 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37);
DEFINE_GUID(SG_CLSID_NullRenderer,  0xC1F400A4, 0x3F08, 0x11d3, 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37);
DEFINE_GUID(SG_IID_ISampleGrabber,  0x6B652FFF, 0x11FE, 0x4FCE, 0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F);
DEFINE_GUID(SG_IID_ISampleGrabberCB,0x0579154A, 0x2B53, 0x4994, 0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85);
DEFINE_GUID(MST_NV12,               0x3231564E, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

static void FreeMediaType(AM_MEDIA_TYPE& mt)
{
    if (mt.cbFormat) { CoTaskMemFree(mt.pbFormat); mt.cbFormat = 0; mt.pbFormat = nullptr; }
    if (mt.pUnk) { mt.pUnk->Release(); mt.pUnk = nullptr; }
}

static IBaseFilter* FindObsSource()
{
    ICreateDevEnum* devEnum = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum))))
        return nullptr;

    IEnumMoniker* en = nullptr;
    IBaseFilter* found = nullptr;
    if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &en, 0) == S_OK && en)
    {
        IMoniker* m = nullptr;
        while (en->Next(1, &m, nullptr) == S_OK)
        {
            IPropertyBag* bag = nullptr;
            if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))))
            {
                VARIANT v; VariantInit(&v);
                if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR)
                {
                    LogF(L"  dshow device: %s", v.bstrVal);
                    if (!found && wcsstr(v.bstrVal, L"OBS Virtual Camera"))
                        m->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&found));
                }
                VariantClear(&v);
                bag->Release();
            }
            m->Release();
        }
        en->Release();
    }
    devEnum->Release();
    return found;
}

// ---- implementation ----------------------------------------------------------
struct ObsCapture::Impl : public ISampleGrabberCB
{
    unsigned dstW, dstH;
    bool running = false;

    IGraphBuilder* graph = nullptr;
    ICaptureGraphBuilder2* builder = nullptr;
    IBaseFilter* source = nullptr;
    IBaseFilter* grabberFilter = nullptr;
    IBaseFilter* nullRenderer = nullptr;
    ISampleGrabber* grabber = nullptr;
    IMediaControl* control = nullptr;

    std::mutex mtx;
    std::vector<BYTE> native;
    int srcW = 0, srcH = 0;
    std::atomic<unsigned long long> frames{ 0 };

    Impl(unsigned w, unsigned h) : dstW(w), dstH(h) {}

    // IUnknown (owned by ObsCapture; no COM refcounting)
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == SG_IID_ISampleGrabberCB) { *ppv = static_cast<ISampleGrabberCB*>(this); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE* pBuffer, long len) override
    {
        if (!pBuffer || len <= 0) return S_OK;
        std::lock_guard<std::mutex> lock(mtx);
        native.assign(pBuffer, pBuffer + len);
        frames.fetch_add(1);
        return S_OK;
    }

    bool Start()
    {
        if (running) return true;
        if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph))) ||
            FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder))))
        {
            LogF(L"OBS: failed to create DirectShow graph objects"); Stop(); return false;
        }
        builder->SetFiltergraph(graph);

        source = FindObsSource();
        if (!source) { LogF(L"OBS: 'OBS Virtual Camera' not found among DirectShow devices"); Stop(); return false; }
        graph->AddFilter(source, L"OBS Virtual Camera");

        if (FAILED(CoCreateInstance(SG_CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&grabberFilter))) ||
            FAILED(grabberFilter->QueryInterface(SG_IID_ISampleGrabber, (void**)&grabber)))
        {
            LogF(L"OBS: SampleGrabber unavailable"); Stop(); return false;
        }
        AM_MEDIA_TYPE want{};
        want.majortype = MEDIATYPE_Video;
        want.subtype = MST_NV12;
        want.formattype = GUID_NULL;
        grabber->SetMediaType(&want);
        grabber->SetOneShot(FALSE);
        grabber->SetBufferSamples(FALSE);
        grabber->SetCallback(this, 1); // 1 => BufferCB
        graph->AddFilter(grabberFilter, L"Sample Grabber");

        if (FAILED(CoCreateInstance(SG_CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&nullRenderer))))
        {
            LogF(L"OBS: NullRenderer unavailable"); Stop(); return false;
        }
        graph->AddFilter(nullRenderer, L"Null Renderer");

        HRESULT hr = builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, source, grabberFilter, nullRenderer);
        if (FAILED(hr)) { LogF(L"OBS: RenderStream failed 0x%08X (NV12 not connectable)", hr); Stop(); return false; }

        AM_MEDIA_TYPE cmt{};
        if (SUCCEEDED(grabber->GetConnectedMediaType(&cmt)))
        {
            if (cmt.formattype == FORMAT_VideoInfo && cmt.pbFormat)
            {
                auto vih = reinterpret_cast<VIDEOINFOHEADER*>(cmt.pbFormat);
                srcW = vih->bmiHeader.biWidth;
                srcH = abs(vih->bmiHeader.biHeight);
            }
            FreeMediaType(cmt);
        }
        if (srcW <= 0 || srcH <= 0) { LogF(L"OBS: could not determine source size"); Stop(); return false; }

        // Run the graph clock-free: with a reference clock the Null Renderer schedules each
        // sample by its presentation time and stalls after the first frame. SetSyncSource(null)
        // makes samples flow as fast as they arrive from OBS.
        {
            IMediaFilter* mf = nullptr;
            if (SUCCEEDED(graph->QueryInterface(IID_IMediaFilter, (void**)&mf)))
            {
                mf->SetSyncSource(nullptr);
                mf->Release();
            }
        }

        if (FAILED(graph->QueryInterface(IID_IMediaControl, (void**)&control)) || FAILED(control->Run()))
        {
            LogF(L"OBS: graph Run failed"); Stop(); return false;
        }

        running = true;
        LogF(L"OBS: capturing 'OBS Virtual Camera' NV12 %dx%d", srcW, srcH);
        return true;
    }

    void Stop()
    {
        if (control) { control->Stop(); control->Release(); control = nullptr; }
        if (grabber) { grabber->SetCallback(nullptr, 1); grabber->Release(); grabber = nullptr; }
        if (grabberFilter) { grabberFilter->Release(); grabberFilter = nullptr; }
        if (nullRenderer) { nullRenderer->Release(); nullRenderer = nullptr; }
        if (source) { source->Release(); source = nullptr; }
        if (builder) { builder->Release(); builder = nullptr; }
        if (graph) { graph->Release(); graph = nullptr; }
        { std::lock_guard<std::mutex> lock(mtx); native.clear(); srcW = srcH = 0; }
        running = false;
    }

    bool GetFrame(std::vector<unsigned char>& out)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (srcW <= 0 || srcH <= 0) return false;
        const size_t need = (size_t)srcW * srcH * 3 / 2;
        if (native.size() < need) return false;

        const int sw = srcW, sh = srcH;
        const int dw = (int)dstW, dh = (int)dstH;
        const BYTE* sY = native.data();
        const BYTE* sUV = sY + (size_t)sw * sh;
        BYTE* dY = out.data();
        BYTE* dUV = dY + (size_t)dw * dh;

        for (int y = 0; y < dh; y++)
        {
            const BYTE* srow = sY + (size_t)(y * sh / dh) * sw;
            BYTE* drow = dY + (size_t)y * dw;
            for (int x = 0; x < dw; x++) drow[x] = srow[x * sw / dw];
        }
        const int cdw = dw / 2, cdh = dh / 2, csw = sw / 2, csh = sh / 2;
        for (int cy = 0; cy < cdh; cy++)
        {
            const BYTE* srow = sUV + (size_t)(cy * csh / cdh) * sw; // UV stride == Y stride (sw)
            BYTE* drow = dUV + (size_t)cy * dw;
            for (int cx = 0; cx < cdw; cx++)
            {
                int scx = cx * csw / cdw;
                drow[cx * 2]     = srow[scx * 2];
                drow[cx * 2 + 1] = srow[scx * 2 + 1];
            }
        }
        return true;
    }
};

ObsCapture::ObsCapture(unsigned dstW, unsigned dstH) : _impl(new Impl(dstW, dstH)) {}
ObsCapture::~ObsCapture() { _impl->Stop(); delete _impl; }
bool ObsCapture::Start() { return _impl->Start(); }
void ObsCapture::Stop() { _impl->Stop(); }
bool ObsCapture::Running() const { return _impl->running; }
bool ObsCapture::GetFrame(std::vector<unsigned char>& out) { return _impl->GetFrame(out); }
unsigned long long ObsCapture::FramesReceived() const { return _impl->frames.load(); }
