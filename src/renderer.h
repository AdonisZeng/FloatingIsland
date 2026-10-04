#pragma once
// GPU 渲染管线：D3D11 + DXGI Composition SwapChain + DirectComposition + D2D/DWrite 内容层
//
// 关键设计：
//   * 交换链由 CreateSwapChainForComposition 创建（无 HWND 关联），内容交给 DComp Visual，
//     避免窗口层面的重绘竞争 -> 从根上消除 resize 闪烁。
//   * AlphaMode = PREMULTIPLIED，着色器输出自己做预乘。
//   * 内容（DirectWrite 文字 / 封面）画在独立纹理上，像素着色器用 SDF mask 裁剪合成。
//   * 窗口尺寸恒定，形状变化只改 shader 常量，不动窗口几何。
//   * 着色器为 fxc 离线编译的字节码（tools/build-shaders.ps1），运行时零编译依赖。

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace fi {

// 一帧的形状描述，单位：物理像素，坐标原点为视口中心
// 真圆角矩形：收缩态 cornerRadius == halfH 即为标准胶囊（半球端头），
// 展开态为固定圆角半径的圆角矩形。旧融球字段已删除。
struct ShapeFrame {
    float capsuleHalfW   = 100.0f;
    float capsuleHalfH   = 16.0f;
    float cornerRadius   = 16.0f;  // 会被钳制到 min(halfW, halfH) 以内
    float capsuleCenterY = 0.0f;   // 胶囊中心相对视口中心的 Y 偏移
    float rimStrength    = 0.08f;  // 边缘高光强度（旧值 0.20 太生硬）
    float pad0           = 0.0f;
    float pad1           = 0.0f;
    float pad2           = 0.0f;
    float fill[4]        = { 0.043f, 0.043f, 0.051f, 1.0f };
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer() { Discard(); }

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // hwnd 必须是已创建好的宿主窗口；widthPx/heightPx 为物理像素
    HRESULT Initialize(HWND hwnd, UINT widthPx, UINT heightPx, bool enableDebugLayer);
    HRESULT Resize(UINT widthPx, UINT heightPx);
    HRESULT RenderFrame(const ShapeFrame& frame, float timeSeconds);

    // TDR / 驱动重置 / 显卡切换后 Present 会返回 DXGI_ERROR_DEVICE_REMOVED：
    // 调用方看到 DeviceLost() 后调用 Recover() 重建整套资源
    bool     DeviceLost() const noexcept { return deviceLost_; }
    HRESULT  Recover();

    // 内容层三步曲：Begin -> 用 D2D 画文字/封面 -> End，随后 RenderFrame 自动合成
    ID2D1DeviceContext* BeginContentDraw();
    void                EndContentDraw();

    // 文字资源（生命周期与 Renderer 相同）
    IDWriteTextFormat*    TitleFormat()  const { return titleFormat_.Get(); }
    IDWriteTextFormat*    ArtistFormat() const { return artistFormat_.Get(); }
    IDWriteTextFormat*    TimeFormat()   const { return timeFormat_.Get(); }
    IDWriteTextFormat*    IconFormat()   const { return iconFormat_.Get(); }
    ID2D1SolidColorBrush* TitleBrush()   const { return brushTitle_.Get(); }
    ID2D1SolidColorBrush* ArtistBrush()  const { return brushArtist_.Get(); }
    ID2D1SolidColorBrush* SubTextBrush() const { return brushSubText_.Get(); }
    ID2D1SolidColorBrush* AccentBrush()  const { return brushAccent_.Get(); }
    ID2D1SolidColorBrush* IconBrush()    const { return brushIcon_.Get(); }
    ID2D1SolidColorBrush* HoverBrush()   const { return brushHover_.Get(); }

    ID3D11Device*        Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }

    // 单行文本实际宽度（卡拉OK按比例填充用）
    float MeasureTextWidth(const wchar_t* text, UINT32 len, IDWriteTextFormat* fmt) const;

    void Discard();

private:
    HRESULT CreateDeviceAndSwapChain(UINT widthPx, UINT heightPx, bool enableDebugLayer);
    HRESULT AttachToWindow(HWND hwnd);
    HRESULT CreateShaders();
    HRESULT CreateBackBufferViews();
    HRESULT CreateD2DResources();
    HRESULT EnsureContentTargets();

private:
    Microsoft::WRL::ComPtr<ID3D11Device>            device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>     context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>         swapChain_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>         backBuffer_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>  rtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>      vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>       pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>            constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>   rasterizerState_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>      samplerState_;

    // D2D / DirectWrite 内容层
    Microsoft::WRL::ComPtr<ID2D1Factory1>           d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Device>             d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext>      d2dCtx_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>         contentTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> contentSrv_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1>            contentBitmap_;
    Microsoft::WRL::ComPtr<IDWriteFactory>          dwFactory_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>       titleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>       artistFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>       timeFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat>       iconFormat_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>    brushTitle_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>    brushArtist_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>    brushSubText_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>    brushAccent_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>    brushIcon_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>    brushHover_;

    Microsoft::WRL::ComPtr<IDCompositionDevice>     dcompDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget>     dcompTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual>     dcompVisual_;

    UINT widthPx_    = 0;
    UINT heightPx_   = 0;
    UINT contentW_   = 0;
    UINT contentH_   = 0;
    bool d2dFailed_  = false;
    bool deviceLost_ = false;

    // 用于 Recover()：重建需要原始宿主窗口与调试层开关
    HWND  hwnd_       = nullptr;
    bool  debugLayer_ = false;
};

} // namespace fi
