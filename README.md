# FloatingIsland

Windows 灵动岛 —— 媒体 / 歌词 / 频谱 / 通知聚合悬浮挂件。

原生 Win32 + D3D11 + DirectComposition 实现，无运行时依赖，Release 单文件 exe。

## 功能

| 模块 | 说明 |
| --- | --- |
| Media | GSMTC 读取当前会话：封面、歌名/歌手、播放控制 |
| Lyrics | 网易 / QQ / LRCLIB / JustSolo 多源歌词，含卡拉OK逐字高亮 |
| Spectrum | 音频频谱柱 |
| Clock | 时间日期 |
| CpuMem | CPU / 内存占用 |
| Toast | 系统通知监听（`UserNotificationListener`） |
| Clipboard | 剪贴板文本变更提示 |

岛体形状是真圆角矩形 `sdRoundBox`，在固定 720×240 DIP 视口内形变：

- 收缩态 200×32 DIP，胶囊（圆角 == 半高）
- 展开态 520×120 DIP，圆角矩形

窗口尺寸恒定，形状变化只改 shader 常量，**永不 resize**，因此无闪烁。

## 环境要求

- Windows 10 1803+（`UserNotificationListener` 下限），推荐 Windows 11
- Visual Studio 2022，工具集 **v145**
- Windows SDK **10.0.28000.0**（版本锁定：GSMTC / DComp 头在不同 SDK 间有差异）
- C++20

## 构建

```powershell
# 1. 还原 + 编译 Release（输出 x64\Release\FloatingIsland.exe）
msbuild FloatingIsland.vcxproj /p:Configuration=Release /p:Platform=x64

# 2. 改了 HLSL 必须重新生成字节码，否则运行的是旧着色器
#    注意：不要用 tools\build-shaders.ps1（pwsh 下解析报错），直接调 fxc：
fxc /nologo /T ps_5_0 /E PS /Fh src/shaders/generated/island_ps.h /Vn g_island_ps src/shaders/island_ps.hlsl
```

`src/shaders/generated/*.h` 是**入库**的编译产物，需要随源码一起提交。

## 运行：必须注册 sparse MSIX

GSMTC 与 Toast 监听都强制要求进程拥有 package identity，否则系统直接拒绝。
`packaging/` 是 sparse MSIX 部署件 —— 只赋予身份，不打包 exe：

```powershell
# 前置：开启开发者模式（设置 > 系统 > 开发者选项）
powershell -ExecutionPolicy Bypass -File packaging\register-sparse.ps1
```

注册后**必须从开始菜单启动**（搜 `FloatingIsland`），直接双击 exe 没有包身份。
未注册时程序仍可运行，但媒体与通知模块会降级失效，启动时会有诊断提示。

## 配置

配置持久化在注册表 `HKCU\Software\FloatingIsland`。
`Load()` 启动时全量读入，`Save()` 全量写回（避免逐项同步导致的状态漂移）。

可调项包括主题（深/浅/跟随系统）、背景不透明度、字体、DPI 缩放、目标显示器、
模块显示次序、歌词偏移、提示音、自动隐藏策略、鼠标穿透等。

## 目录结构

```
src/
  main.cpp            窗口 / 消息循环 / 形状形变状态机 / 命中区
  renderer.*          D3D11 + DComp + D2D 渲染
  sdf.h               sdRoundBox（CPU 侧，供 WM_NCHITTEST）
  shaders/
    *.hlsl            着色器源码
    generated/*.h     fxc 编译产物（入库）
  media/              GSMTC、歌词、封面、JustSolo
  notify/             Toast 监听、剪贴板监控
  audio/              频谱分析
  config/             注册表配置存储
  net/                HTTP 客户端
  system/             CPU / 内存
  util/               日志、单例
packaging/            sparse MSIX manifest + 注册脚本
third_party/          nlohmann/json（单头文件）
tools/                着色器构建脚本（见上方注意事项）
```

## 两条纪律

**形状双实现必须逐字一致。** GPU 在 `src/shaders/island_ps.hlsl`（`FrameCB.CornerRadius`），
CPU 在 `src/sdf.h` 的 `fi::sdRoundBox` 供 `WM_NCHITTEST` 判定。改形状必须同步改两处，
否则点击区域与画面会漂移。

**全屏三角形必须 `D3D11_CULL_NONE`。** 用默认剔除会出现「窗口在而屏幕无岛」。

## 已知问题

`vcpkg.json` 声明了 `nlohmann-json`，但构建实际并未接入 vcpkg（无 toolchain 文件、
无 `VcpkgEnabled`），头文件来自 `third_party/`。该清单目前是误导性配置，
可删除或改为真正的 vcpkg 依赖。

## License

[MIT](LICENSE) © 2026 AdonisZeng
