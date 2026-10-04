// 全屏三角形顶点着色器：无需顶点缓冲，用 SV_VertexID 生成
float4 VS(uint vid : SV_VertexID) : SV_Position
{
    float x = (vid == 1) ? 3.0 : -1.0;
    float y = (vid == 2) ? 3.0 : -1.0;
    return float4(x, y, 0.0, 1.0);
}
