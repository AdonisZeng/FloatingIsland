#include "ImageLoader.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <objidl.h>

namespace fi {

using Microsoft::WRL::ComPtr;

HRESULT ImageLoader::DecodeToBitmap(ID2D1DeviceContext* ctx,
                                    const uint8_t* data, size_t size,
                                    ID2D1Bitmap1** out) {
    if (!ctx || !data || size == 0 || !out) return E_INVALIDARG;

    static ComPtr<IWICImagingFactory2> wic;
    if (!wic) {
        HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory2, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(wic.GetAddressOf()));
        if (FAILED(hr)) return hr;
    }

    // HGlobal 流：fDeleteOnRelease=TRUE，流释放时连带释放内存
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return E_OUTOFMEMORY;
    void* p = ::GlobalLock(mem);
    if (!p) {
        ::GlobalFree(mem);
        return E_FAIL;
    }
    memcpy(p, data, size);
    ::GlobalUnlock(mem);

    ComPtr<IStream> stream;
    HRESULT hr = ::CreateStreamOnHGlobal(mem, TRUE, stream.GetAddressOf());
    if (FAILED(hr)) {
        ::GlobalFree(mem);
        return hr;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    // 用 OnLoad：OnDemand 会把读取推迟到 CopyPixels，而这里的 IStream 随
    // ComPtr 在函数返回时释放（连带 GlobalFree），延迟读流存在踩已释放内存的风险
    hr = wic->CreateDecoderFromStream(stream.Get(), nullptr,
                                      WICDecodeMetadataCacheOnLoad,
                                      decoder.GetAddressOf());
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) return hr;

    ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return hr;

    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    return ctx->CreateBitmapFromWicBitmap(converter.Get(), &props, out);
}

} // namespace fi
