#include "renderer.h"
#include "util/Logger.h"

#include "config/ConfigStore.h"

// fxc 离线编译的字节码（tools/build-shaders.ps1 生成）
#include "shaders/generated/fullscreen_vs.h"
#include "shaders/generated/island_ps.h"

#include <cstring>
#include <iterator>

namespace fi {
namespace {

// 与 island_ps.hlsl 的 cbuffer 布局逐字段对应（16 字节槽对齐）
struct __declspec(align(16)) FrameConstants {
    float viewport[2];
    float time;
    float cornerRadius;
    float capsuleHalf[2];
    float centerY;
    float strokeAlpha;
    float rimStrength;
    float pad0;
    float pad1;
    float pad2;
    float fill[4];
};

static_assert(sizeof(FrameConstants) == 64, "cbuffer layout mismatch");

} // namespace

// ---------------------------------------------------------------------------

HRESULT Renderer::CreateDeviceAndSwapChain(UINT widthPx, UINT heightPx, bool enableDebugLayer) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;   // D2D 互操作必需
    if (enableDebugLayer) flags |= D3D11_CREATE_DEVICE_DEBUG;

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1
    };
    D3D_FEATURE_LEVEL obtained = {};

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, static_cast<UINT>(std::size(levels)),
                                   D3D11_SDK_VERSION,
                                   device_.GetAddressOf(), &obtained, context_.GetAddressOf());

    // 未安装 "Graphics Tools" 可选组件时 DEBUG flag 会直接失败，回退重试
    if (FAILED(hr) && enableDebugLayer) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               levels, static_cast<UINT>(std::size(levels)),
                               D3D11_SDK_VERSION,
                               device_.GetAddressOf(), &obtained, context_.GetAddressOf());
    }
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return hr;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width       = widthPx;
    desc.Height      = heightPx;
    desc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo      = FALSE;
    desc.SampleDesc  = { 1, 0 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling     = DXGI_SCALING_STRETCH;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;   // 与 DWM 的半透明合成契约
    desc.Flags       = 0;

    hr = factory->CreateSwapChainForComposition(device_.Get(), &desc, nullptr,
                                                swapChain_.GetAddressOf());
    if (FAILED(hr)) return hr;

    widthPx_  = widthPx;
    heightPx_ = heightPx;
    return S_OK;
}

HRESULT Renderer::AttachToWindow(HWND hwnd) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) return hr;

    hr = ::DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(dcompDevice_.GetAddressOf()));
    if (FAILED(hr)) return hr;

    hr = dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, dcompTarget_.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = dcompDevice_->CreateVisual(dcompVisual_.GetAddressOf());
    if (FAILED(hr)) return hr;

    // 交换链直接作为 Visual 的内容，之后 Present 即自动生效
    hr = dcompVisual_->SetContent(swapChain_.Get());
    if (FAILED(hr)) return hr;

    hr = dcompTarget_->SetRoot(dcompVisual_.Get());
    if (FAILED(hr)) return hr;

    return dcompDevice_->Commit();
}

HRESULT Renderer::CreateShaders() {
    static_assert(sizeof(g_fullscreen_vs) > 0 && sizeof(g_island_ps) > 0, "empty shader blob");

    HRESULT hr = device_->CreateVertexShader(g_fullscreen_vs, sizeof(g_fullscreen_vs),
                                             nullptr, vertexShader_.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = device_->CreatePixelShader(g_island_ps, sizeof(g_island_ps),
                                    nullptr, pixelShader_.GetAddressOf());
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth      = sizeof(FrameConstants);
    cbDesc.Usage          = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbDesc.MiscFlags      = 0;

    hr = device_->CreateBuffer(&cbDesc, nullptr, constantBuffer_.GetAddressOf());
    if (FAILED(hr)) return hr;

    // 全屏三角形 (-1,-1)(3,-1)(-1,3) 在屏幕空间是逆时针绕序，
    // 默认光栅化状态（顺时针为正面 + 剔除背面）会把它整个剔除——
    // 画面保持全透明但所有 API 调用照常成功，曾导致"窗口在而屏幕无岛"。
    // 全屏三角形无需剔除，必须显式关闭。
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    rsDesc.DepthClipEnable = TRUE;
    hr = device_->CreateRasterizerState(&rsDesc, rasterizerState_.GetAddressOf());
    if (FAILED(hr)) return hr;

    // 内容纹理采样器：线性 + Clamp（不依赖默认采样器状态）
    D3D11_SAMPLER_DESC sDesc = {};
    sDesc.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sDesc.MaxAnisotropy = 1;
    sDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sDesc.MaxLOD  = D3D11_FLOAT32_MAX;
    return device_->CreateSamplerState(&sDesc, samplerState_.GetAddressOf());
}

HRESULT Renderer::CreateBackBufferViews() {
    rtv_.Reset();
    backBuffer_.Reset();

    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer_.GetAddressOf()));
    if (FAILED(hr)) return hr;

    return device_->CreateRenderTargetView(backBuffer_.Get(), nullptr, rtv_.GetAddressOf());
}

HRESULT Renderer::CreateD2DResources() {
    D2D1_FACTORY_OPTIONS options = {};
#if defined(_DEBUG)
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

    HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     __uuidof(ID2D1Factory1), &options,
                                     reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) return hr;

    hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), d2dDevice_.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                         d2dCtx_.GetAddressOf());
    if (FAILED(hr)) return hr;

    // 透明背景上 ClearType 不可用，必须灰度抗锯齿
    d2dCtx_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(dwFactory_.GetAddressOf()));
    if (FAILED(hr)) return hr;

    // 字体族可被用户配置整体替换（空 = 默认）
    const Config& cfg = ConfigStore::Get().Data();
    const wchar_t* kFont = cfg.fontFamily.empty()
        ? L"Segoe UI Variable Display" : cfg.fontFamily.c_str();

    hr = dwFactory_->CreateTextFormat(kFont, nullptr,
                                      DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL,
                                      16.0f, L"zh-CN", titleFormat_.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = dwFactory_->CreateTextFormat(kFont, nullptr,
                                      DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL,
                                      12.0f, L"zh-CN", artistFormat_.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = dwFactory_->CreateTextFormat(kFont, nullptr,
                                      DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL,
                                      11.0f, L"zh-CN", timeFormat_.GetAddressOf());
    if (FAILED(hr)) return hr;

    // 媒体控制图标用 Segoe MDL2 Assets（Win10/11 均内置）
    hr = dwFactory_->CreateTextFormat(L"Segoe MDL2 Assets", nullptr,
                                      DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL,
                                      13.0f, L"zh-CN", iconFormat_.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = d2dCtx_->CreateSolidColorBrush(D2D1::ColorF(0.98f, 0.98f, 0.99f, 1.0f),
                                        brushTitle_.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = d2dCtx_->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.70f, 0.74f, 1.0f),
                                        brushArtist_.GetAddressOf());
    if (FAILED(hr)) return hr;

    // 强调色 #3B82F6 / 图标白 / 次级灰
    hr = d2dCtx_->CreateSolidColorBrush(D2D1::ColorF(0.231f, 0.510f, 0.965f, 1.0f),
                                        brushAccent_.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = d2dCtx_->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f),
                                        brushIcon_.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = d2dCtx_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f),
                                        brushHover_.GetAddressOf());
    if (FAILED(hr)) return hr;

    return d2dCtx_->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.55f, 0.60f, 1.0f),
                                          brushSubText_.GetAddressOf());
}

HRESULT Renderer::EnsureContentTargets() {
    if (!d2dCtx_) return E_NOT_VALID_STATE;
    if (contentBitmap_ && contentW_ == widthPx_ && contentH_ == heightPx_) return S_OK;

    contentBitmap_.Reset();
    contentSrv_.Reset();
    contentTex_.Reset();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = widthPx_;
    td.Height           = heightPx_;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc       = { 1, 0 };
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&td, nullptr, contentTex_.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = device_->CreateShaderResourceView(contentTex_.Get(), nullptr,
                                           contentSrv_.GetAddressOf());
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    hr = contentTex_.As(&surface);
    if (FAILED(hr)) return hr;

    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    hr = d2dCtx_->CreateBitmapFromDxgiSurface(surface.Get(), &props,
                                              contentBitmap_.GetAddressOf());
    if (FAILED(hr)) return hr;

    contentW_ = widthPx_;
    contentH_ = heightPx_;
    return S_OK;
}

HRESULT Renderer::Initialize(HWND hwnd, UINT widthPx, UINT heightPx, bool enableDebugLayer) {
    hwnd_        = hwnd;
    debugLayer_  = enableDebugLayer;
    deviceLost_  = false;

    HRESULT hr = CreateDeviceAndSwapChain(widthPx, heightPx, enableDebugLayer);
    if (FAILED(hr)) {
        LOGE(L"D3D11 设备/交换链创建失败 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    hr = AttachToWindow(hwnd);
    if (FAILED(hr)) {
        LOGE(L"DComp 目标挂接失败 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    hr = CreateShaders();
    if (FAILED(hr)) {
        LOGE(L"着色器创建失败 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    hr = CreateBackBufferViews();
    if (FAILED(hr)) {
        LOGE(L"后备缓冲视图创建失败 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    // D2D 层失败不致命：着色器对采样结果有 (1-c.a) 保护，仍可显示纯胶囊
    hr = CreateD2DResources();
    d2dFailed_ = FAILED(hr);
    if (d2dFailed_) {
        // 此前这里仍去创建内容纹理，EnsureContentTargets 随即返回 E_NOT_VALID_STATE，
        // 让整个 Initialize 失败 -> 程序起不来，与上面的降级承诺相反
        LOGW(L"D2D/DWrite 初始化失败 0x%08lX，降级为纯胶囊", static_cast<unsigned long>(hr));
        contentBitmap_.Reset();
        contentSrv_.Reset();
        contentTex_.Reset();
        contentW_ = 0;
        contentH_ = 0;
        return S_OK;
    }

    LOGI(L"渲染管线初始化完成 %ux%u debugLayer=%d",
         widthPx_, heightPx_, debugLayer_ ? 1 : 0);
    return EnsureContentTargets();
}

HRESULT Renderer::Recover() {
    if (!hwnd_) return E_NOT_VALID_STATE;
    const UINT w = widthPx_;
    const UINT h = heightPx_;
    if (w == 0 || h == 0) return E_NOT_VALID_STATE;
    Discard();   // 内部可能改动尺寸成员，先留档
    return Initialize(hwnd_, w, h, debugLayer_);
}

HRESULT Renderer::Resize(UINT widthPx, UINT heightPx) {
    if (!swapChain_) return E_NOT_VALID_STATE;
    if (widthPx == widthPx_ && heightPx == heightPx_) return S_OK;

    rtv_.Reset();
    backBuffer_.Reset();
    context_->ClearState();

    HRESULT hr = swapChain_->ResizeBuffers(0, widthPx, heightPx, DXGI_FORMAT_UNKNOWN, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) deviceLost_ = true;
    if (FAILED(hr)) return hr;

    widthPx_  = widthPx;
    heightPx_ = heightPx;

    hr = CreateBackBufferViews();
    if (FAILED(hr)) {
        LOGE(L"Resize 后备缓冲视图创建失败 0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    LOGI(L"交换链已调整 %ux%u", widthPx, heightPx);
    return d2dFailed_ ? S_OK : EnsureContentTargets();
}

ID2D1DeviceContext* Renderer::BeginContentDraw() {
    if (d2dFailed_ || !d2dCtx_ || !contentBitmap_) return nullptr;
    d2dCtx_->SetTarget(contentBitmap_.Get());
    d2dCtx_->BeginDraw();
    return d2dCtx_.Get();
}

void Renderer::EndContentDraw() {
    if (d2dFailed_ || !d2dCtx_) return;
    HRESULT hr = d2dCtx_->EndDraw();
    if (FAILED(hr)) {
        LOGW(L"内容层 EndDraw 失败 0x%08lX", static_cast<unsigned long>(hr));
        d2dCtx_->Flush();
        return;
    }
    d2dCtx_->Flush();   // 保证 D3D 侧采样时 D2D 命令已提交
}

float Renderer::MeasureTextWidth(const wchar_t* text, UINT32 len,
                                 IDWriteTextFormat* fmt) const {
    if (!dwFactory_ || !text || len == 0 || !fmt) return 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwFactory_->CreateTextLayout(text, len, fmt,
                                            10000.0f, 200.0f,
                                            layout.GetAddressOf()))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return 0.0f;
    return m.width;
}

HRESULT Renderer::RenderFrame(const ShapeFrame& frame, float timeSeconds) {
    if (!rtv_ || !constantBuffer_) return E_NOT_VALID_STATE;

    FrameConstants cb = {};
    cb.viewport[0]    = static_cast<float>(widthPx_);
    cb.viewport[1]    = static_cast<float>(heightPx_);
    cb.time           = timeSeconds;
    cb.cornerRadius   = frame.cornerRadius;
    cb.capsuleHalf[0] = frame.capsuleHalfW;
    cb.capsuleHalf[1] = frame.capsuleHalfH;
    cb.centerY        = frame.capsuleCenterY;
    cb.strokeAlpha    = 0.0f;
    cb.rimStrength    = frame.rimStrength;
    cb.pad0           = 0.0f;
    cb.pad1           = 0.0f;
    cb.pad2           = 0.0f;
    cb.fill[0]        = frame.fill[0];
    cb.fill[1]        = frame.fill[1];
    cb.fill[2]        = frame.fill[2];
    cb.fill[3]        = frame.fill[3];

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = context_->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &cb, sizeof(cb));
        context_->Unmap(constantBuffer_.Get(), 0);
    } else {
        LOGW(L"常量缓冲 Map 失败 0x%08lX（沿用上一帧）", static_cast<unsigned long>(hr));
    }

    const D3D11_VIEWPORT vp = { 0.0f, 0.0f,
                                static_cast<float>(widthPx_),
                                static_cast<float>(heightPx_),
                                0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(rasterizerState_.Get());

    ID3D11RenderTargetView* targets[1] = { rtv_.Get() };
    context_->OMSetRenderTargets(1, targets, nullptr);

    const float transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context_->ClearRenderTargetView(rtv_.Get(), transparent);

    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);

    ID3D11Buffer* cbs[1] = { constantBuffer_.Get() };
    context_->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[1] = { contentSrv_.Get() };
    context_->PSSetShaderResources(0, 1, srvs);

    ID3D11SamplerState* samplers[1] = { samplerState_.Get() };
    context_->PSSetSamplers(0, 1, samplers);

    context_->Draw(3, 0);

    // 解绑：下一帧该纹理又要被 D2D 用作渲染目标
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    context_->PSSetShaderResources(0, 1, nullSrv);
    context_->OMSetRenderTargets(0, nullptr, nullptr);

    hr = swapChain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // TDR / 驱动更新 / 显卡切换：标记后由上层做一次重建，否则永久黑屏
        deviceLost_ = true;
        LOGE(L"D3D 设备丢失 0x%08lX，需重建管线", static_cast<unsigned long>(hr));
    }
    return hr;
}

void Renderer::Discard() {
    if (context_) context_->ClearState();

    brushArtist_.Reset();
    brushTitle_.Reset();
    artistFormat_.Reset();
    titleFormat_.Reset();
    dwFactory_.Reset();
    contentBitmap_.Reset();
    contentSrv_.Reset();
    contentTex_.Reset();
    d2dCtx_.Reset();
    d2dDevice_.Reset();
    d2dFactory_.Reset();

    dcompVisual_.Reset();
    dcompTarget_.Reset();
    dcompDevice_.Reset();

    constantBuffer_.Reset();
    rasterizerState_.Reset();
    samplerState_.Reset();
    pixelShader_.Reset();
    vertexShader_.Reset();
    rtv_.Reset();
    backBuffer_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
}

} // namespace fi
