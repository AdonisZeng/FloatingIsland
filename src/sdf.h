#pragma once
// CPU 侧 SDF 实现：必须与 GPU 着色器（src/shaders/island_ps.hlsl）逐字保持数学一致。
// 用途：WM_NCHITTEST 像素级碰撞判定。
//
// 注意：这里的坐标单位是【物理像素】，调用方需自行完成 DPI 换算。
// 当前形状为真圆角矩形（sdRoundBox）：收缩态 cornerRadius == halfH 即为标准胶囊，
// 展开态为固定小圆角的圆角矩形。旧的超椭圆 + 融球实现已退役（见文件底部）。

#include <cmath>
#include <algorithm>

namespace fi {

// 真圆角矩形 SDF（与 shader 中的 sdRoundBox 一致）。
//   (px,py) : 相对形状中心的坐标
//   (bx,by) : 半宽 / 半高
//   r       : 圆角半径，会被钳制到 min(bx,by) 以内
inline float sdRoundBox(float px, float py, float bx, float by, float r) noexcept {
    const float rr = (std::min)((std::min)(r, bx), by);
    const float qx = std::fabs(px) - (bx - rr);
    const float qy = std::fabs(py) - (by - rr);
    const float ax = (std::max)(qx, 0.0f);
    const float ay = (std::max)(qy, 0.0f);
    return std::sqrt(ax * ax + ay * ay) + (std::min)((std::max)(qx, qy), 0.0f) - rr;
}

// ---- 以下为旧实现残留（收缩态鼓包的根因），仅保留供参考，命中判定不再使用 ----

// 超椭圆（Squircle）有向距离场。
//   (px,py) : 相对形状中心的坐标
//   (bx,by) : 半宽 / 半高
//   n       : 超椭圆指数，n=2 椭圆，n=4 标准 squircle，越大越接近矩形
//
// 数学原理：
//   隐式函数 F = ((|x|/bx)^n + (|y|/by)^n)^(1/n) - 1
//   一阶 SDF 近似：d ≈ F / |∇F|
//   在轴向（y=0 或 x=0）上该近似退化为精确距离，边界附近误差 < 1%，足够做抗锯齿与融球。
inline float sdSquircle(float px, float py, float bx, float by, float n) noexcept {
    const float qx = std::fabs(px) / bx;
    const float qy = std::fabs(py) / by;

    // a 做下限保护：中心处 a→0 会让 a^(1/n-1) 爆炸，clamp 后仅为"很负的数"，
    // 对 min / smoothstep / smin 全部饱和，不影响任何可见结果。
    const float a  = (std::max)(std::pow((std::max)(qx, 1e-6f), n) +
                                std::pow((std::max)(qy, 1e-6f), n), 1e-12f);
    const float ai = std::pow(a, 1.0f / n - 1.0f);
    const float d  = std::pow(a, 1.0f / n) - 1.0f;

    const float gx = std::pow((std::max)(qx, 1e-6f), n - 1.0f) * ai / bx;
    const float gy = std::pow((std::max)(qy, 1e-6f), n - 1.0f) * ai / by;

    return d / (std::max)(std::sqrt(gx * gx + gy * gy), 1e-4f);
}

// 多项式平滑最小值（Metaball / 流体融球）
//   smin(a,b,k) = min(a,b) - max(k-|a-b|,0)^2 / (4k)
//   k 越大，两个形状在越远处就开始互相吸引并产生"拉丝"。
inline float smin(float a, float b, float k) noexcept {
    const float h = (std::max)(k - std::fabs(a - b), 0.0f);
    return (std::min)(a, b) - h * h * 0.25f / k;
}

// 圆
inline float sdCircle(float px, float py, float r) noexcept {
    return std::sqrt(px * px + py * py) - r;
}

} // namespace fi
