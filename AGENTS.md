# FloatingIsland — Agent Notes

Win32 / D3D11 / DirectComposition 桌面灵动岛挂件。

## 形状约定（改动前务必读）

岛体形状是**真圆角矩形** `sdRoundBox`，CPU/GPU 双实现必须逐字一致：

* `src/shaders/island_ps.hlsl` — GPU，`FrameCB.CornerRadius`
* `src/sdf.h` `fi::sdRoundBox` — CPU，供 `WM_NCHITTEST` 命中判定

改形状时同步改两处，否则点击区域与画面会漂移。

* 收缩态：`cornerRadius == halfH` → 标准胶囊（半球端头）
* 展开态：固定小圆角 → 圆角矩形（有平边，不是橄榄形）

`ShapeFrame` 已删除旧的 `cornerN / ballRadius / ballX / ballY / blendK`（超椭圆 + 融球）。
`sdf.h` 里的 `sdSquircle / smin / sdCircle` 是历史残留，**仅供参照，不要再调用**。
融球中间鼓包（顶部外凸约 4.65px）就是旧收缩态丑的根因，已随该实现退役。

## 编译着色器

改完 `src/shaders/*.hlsl` 必须重新生成 `src/shaders/generated/*.h`，否则运行的是旧字节码。

**不要用 `tools/build-shaders.ps1`** —— 该脚本在 pwsh 下因中文注释编码问题直接解析报错（`$PSScriptRoot` 为空、"缺少右括号"）。绕过方式，直接调 fxc：

```powershell
& "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe" `
    /nologo /T ps_5_0 /E PS /Fh src/shaders/generated/island_ps.h /Vn g_island_ps src/shaders/island_ps.hlsl
```

VS 会自动生成 `fullscreen_vs.h`，只有 `island_ps.h` 需要手动重编。

## 其它坑

* 全屏三角形必须 `D3D11_CULL_NONE`，默认剔除会「窗口在而屏幕无岛」。
* `FrameConstants` 有 `static_assert(sizeof == 64)`，HLSL cbuffer 字段增删后先核偏移再改 C++ 结构体。
* 窗口尺寸恒定（720x240 DIP），形状变化只改 shader 常量，永不 resize，因此无闪烁。
* D2D/DWrite 初始化失败必须降级为纯形状，不能让 `Initialize` 整体失败。