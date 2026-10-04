// 灵动岛形状像素着色器：
//   1. 真圆角矩形 SDF（sdRoundBox）—— 收缩态 cornerRadius == halfH 即标准胶囊，
//      展开态固定小圆角，有平边 + 均匀圆角，不再是超椭圆的橄榄形
//   2. fwidth 亚像素抗锯齿
//   3. 与 D2D 内容纹理（DirectWrite 文字/封面）合成，输出预乘 alpha
//
// 注意：SDF 数学必须与 src/sdf.h 的 CPU 实现逐字一致（命中判定依赖）。
// 修改后运行 tools\build-shaders.ps1 重新生成 generated\*.h。

cbuffer FrameCB : register(b0)
{
    float2 Viewport;
    float  Time;
    float  CornerRadius;
    float2 CapsuleHalf;
    float  CenterY;
    float  StrokeAlpha;   // 预留（描边强度，暂未使用）
    float  RimStrength;   // 边缘高光强度（约 0.06~0.10）
    float  Pad0;
    float  Pad1;
    float  Pad2;
    float4 Fill;
};

Texture2D    ContentTex  : register(t0);
SamplerState LinearClamp : register(s0);

// 真圆角矩形 SDF：r 会被钳制到 min(b.x, b.y) 以内
float sdRoundBox(float2 p, float2 b, float r)
{
    float rr = min(r, min(b.x, b.y));
    float2 q = abs(p) - (b - rr);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rr;
}

float4 PS(float4 pos : SV_Position) : SV_Target
{
    float2 origin = float2(Viewport.x * 0.5, Viewport.y * 0.5 + CenterY);
    float2 p = pos.xy - origin;

    float d = sdRoundBox(p, CapsuleHalf, CornerRadius);

    // 亚像素抗锯齿：用屏幕空间导数把过渡带锁死在 ~1 像素
    float aa   = fwidth(d) * 0.75 + 1e-4;
    float mask = 1.0 - smoothstep(-aa, aa, d);

    // 边缘极淡高光（比旧值 0.20 柔和得多）
    float rim = smoothstep(-2.0, -0.4, d) * (1.0 - smoothstep(-0.6, 0.8, d));
    float3 rimLight = rim * RimStrength;

    // 内容层（D2D 绘制，预乘 alpha），SDF mask 天然充当裁剪
    float2 uv = pos.xy / Viewport;
    float4 c  = ContentTex.Sample(LinearClamp, uv);

    float3 rgb = (Fill.rgb + rimLight) * (1.0 - c.a) + c.rgb;
    rgb *= mask;
    return float4(rgb, mask);
}
