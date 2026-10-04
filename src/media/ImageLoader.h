#pragma once
// 图片解码：内存字节（PNG/JPG/...）-> D2D 位图（WIC 管线）。
// 用途：网络封面 -> 岛体绘制。解码结果绑定 ctx 所属设备，
// 必须在渲染线程（主线程）调用 CreateBitmap。

#include <d2d1_3.h>
#include <cstdint>
#include <cstddef>

namespace fi {

class ImageLoader {
public:
    // data 尺寸任意（JPEG/PNG/BMP/GIF 首帧）；失败返回失败 HRESULT
    static HRESULT DecodeToBitmap(ID2D1DeviceContext* ctx,
                                  const uint8_t* data, size_t size,
                                  ID2D1Bitmap1** out);
};

} // namespace fi
