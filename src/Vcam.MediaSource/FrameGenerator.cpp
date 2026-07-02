#include "pch.h"
#include "Undocumented.h"
#include "Tools.h"
#include "EnumNames.h"
#include "MFTools.h"
#include "FrameGenerator.h"

HRESULT FrameGenerator::EnsureRenderTarget(UINT width, UINT height)
{
	if (!HasD3DManager())
	{
		// create a D2D1 render target from WIC bitmap
		wil::com_ptr_nothrow<ID2D1Factory> d2d1Factory;
		RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, IID_PPV_ARGS(&d2d1Factory)));

		wil::com_ptr_nothrow<IWICImagingFactory> wicFactory;
		RETURN_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wicFactory)));

		RETURN_IF_FAILED(wicFactory->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &_bitmap));

		D2D1_RENDER_TARGET_PROPERTIES props{};
		props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
		props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
		RETURN_IF_FAILED(d2d1Factory->CreateWicBitmapRenderTarget(_bitmap.get(), props, &_renderTarget));

		RETURN_IF_FAILED(CreateRenderTargetResources(width, height));
	}

	_prevTime = MFGetSystemTime();
	_frame = 0;
	return S_OK;
}

const bool FrameGenerator::HasD3DManager() const
{
	return _texture != nullptr;
}

HRESULT FrameGenerator::SetD3DManager(IUnknown* manager, UINT width, UINT height)
{
	// Not used by OBS2MF (MediaStream::SetD3DManager is a no-op to force the CPU path),
	// but kept intact for reference.
	RETURN_HR_IF_NULL(E_POINTER, manager);
	RETURN_HR_IF(E_INVALIDARG, !width || !height);

	RETURN_IF_FAILED(manager->QueryInterface(&_dxgiManager));
	RETURN_IF_FAILED(_dxgiManager->OpenDeviceHandle(&_deviceHandle));

	wil::com_ptr_nothrow<ID3D11Device> device;
	RETURN_IF_FAILED(_dxgiManager->GetVideoService(_deviceHandle, IID_PPV_ARGS(&device)));

	CD3D11_TEXTURE2D_DESC desc
	(
		DXGI_FORMAT_B8G8R8A8_UNORM,
		width,
		height,
		1,
		1,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET
	);
	RETURN_IF_FAILED(device->CreateTexture2D(&desc, nullptr, &_texture));
	wil::com_ptr_nothrow<IDXGISurface> surface;
	RETURN_IF_FAILED(_texture.copy_to(&surface));

	wil::com_ptr_nothrow<ID2D1Factory> d2d1Factory;
	RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, IID_PPV_ARGS(&d2d1Factory)));

	auto props = D2D1::RenderTargetProperties
	(
		D2D1_RENDER_TARGET_TYPE_DEFAULT,
		D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)
	);
	RETURN_IF_FAILED(d2d1Factory->CreateDxgiSurfaceRenderTarget(surface.get(), props, &_renderTarget));

	RETURN_IF_FAILED(CreateRenderTargetResources(width, height));
	return S_OK;
}

// common to CPU & GPU
HRESULT FrameGenerator::CreateRenderTargetResources(UINT width, UINT height)
{
	assert(_renderTarget);
	RETURN_IF_FAILED(_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &_whiteBrush));

	RETURN_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&_dwrite));
	RETURN_IF_FAILED(_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 40, L"", &_textFormat));
	RETURN_IF_FAILED(_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
	RETURN_IF_FAILED(_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
	_width = width;
	_height = height;
	return S_OK;
}

HRESULT FrameGenerator::Generate(IMFSample* sample, REFGUID format, IMFSample** outSample)
{
	UNREFERENCED_PARAMETER(format); // OBS2MF advertises a single NV12 type
	RETURN_HR_IF_NULL(E_POINTER, sample);
	RETURN_HR_IF_NULL(E_POINTER, outSample);
	*outSample = nullptr;

	// Lock the destination NV12 buffer (OBS2MF always runs the CPU path).
	wil::com_ptr_nothrow<IMFMediaBuffer> mediaBuffer;
	RETURN_IF_FAILED(sample->GetBufferByIndex(0, &mediaBuffer));
	wil::com_ptr_nothrow<IMF2DBuffer2> buffer2D;
	RETURN_IF_FAILED(mediaBuffer->QueryInterface(IID_PPV_ARGS(&buffer2D)));

	BYTE* scanline; LONG pitch; BYTE* start; DWORD length;
	RETURN_IF_FAILED(buffer2D->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline, &pitch, &start, &length));

	HRESULT hr = S_OK;
	bool served = false;

	// 1) Try a live NV12 frame from the broker over the pipe.
	obs2mf::FrameHeader fh{};
	if (_pipe.Fetch(fh, _pipeBuf) &&
		fh.fourcc == obs2mf::kFourccNV12 &&
		fh.width == _width && fh.height == _height &&
		_pipeBuf.size() >= (size_t)_width * _height * 3 / 2)
	{
		const BYTE* src = _pipeBuf.data();
		const UINT srcStride = fh.stride ? fh.stride : fh.width;

		// Y plane: _height rows of _width bytes
		for (UINT y = 0; y < _height; y++)
			memcpy(scanline + (size_t)y * pitch, src + (size_t)y * srcStride, _width);

		// UV plane: _height/2 rows of _width bytes, immediately after Y (same pitch)
		const BYTE* srcUV = src + (size_t)srcStride * _height;
		BYTE* dstUV = scanline + (size_t)pitch * _height;
		for (UINT y = 0; y < _height / 2; y++)
			memcpy(dstUV + (size_t)y * pitch, srcUV + (size_t)y * srcStride, _width);

		served = true;
	}

	// 2) Fallback: local "waiting for service" animated pattern.
	if (!served)
		hr = RenderFallbackNV12(scanline, pitch);

	buffer2D->Unlock2D();

	if (SUCCEEDED(hr))
	{
		_frame++;
		sample->AddRef();
		*outSample = sample;
	}
	return hr;
}

HRESULT FrameGenerator::RenderFallbackNV12(BYTE* dst, LONG pitch)
{
	if (!(_renderTarget && _textFormat && _dwrite && _whiteBrush))
		return E_FAIL;

	_renderTarget->BeginDraw();
	_renderTarget->Clear(D2D1::ColorF(0, 0, 0.30f, 1));

	// moving vertical bar -> clearly animated even without a live source
	const float span = (float)(_width ? _width : 1);
	const float x = (float)((_frame * 8) % (UINT)span);
	wil::com_ptr_nothrow<ID2D1SolidColorBrush> bar;
	if (SUCCEEDED(_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.60f, 1.0f, 1), &bar)))
		_renderTarget->FillRectangle(D2D1::Rect(x, 0.f, x + 40.f, (float)_height), bar.get());

	_renderTarget->DrawRectangle(D2D1::Rect(2.f, 2.f, (float)_width - 2.f, (float)_height - 2.f), _whiteBrush.get());

	wchar_t text[256];
	auto len = wsprintf(text, L"OBS2MF\nWaiting for OBS2MF service...\nFrame# %I64u\n%u x %u", _frame, _width, _height);

	wil::com_ptr_nothrow<IDWriteTextLayout> layout;
	if (SUCCEEDED(_dwrite->CreateTextLayout(text, len, _textFormat.get(), (FLOAT)_width, (FLOAT)_height, &layout)))
		_renderTarget->DrawTextLayout(D2D1::Point2F(0, 0), layout.get(), _whiteBrush.get());

	RETURN_IF_FAILED(_renderTarget->EndDraw());

	// Convert the rendered RGB32 (WIC bitmap) into the destination NV12 buffer.
	wil::com_ptr_nothrow<IWICBitmapLock> lock;
	RETURN_IF_FAILED(_bitmap->Lock(nullptr, WICBitmapLockRead, &lock));
	UINT w, h; RETURN_IF_FAILED(lock->GetSize(&w, &h));
	UINT wicStride; RETURN_IF_FAILED(lock->GetStride(&wicStride));
	UINT wicSize; WICInProcPointer wicPointer;
	RETURN_IF_FAILED(lock->GetDataPointer(&wicSize, &wicPointer));

	return RGB32ToNV12(wicPointer, wicSize, wicStride, w, h, dst, (ULONG)((size_t)pitch * h * 3 / 2), pitch);
}
