#pragma once
// ============================================================
// BmpWriter.h —— 只管 BMP 文件格式，不管业务语义
//
// 收掉的职责混乱：原来 MapRenderer::SaveBmp 里同时干了
//   「代价值怎么变成黑白」和「BMP 文件怎么写」两件事。
// BMP 的三个坑（行倒序、4 字节对齐、BGR 顺序）现在只在这里出现。
//
// 本文件是纯 ASCII。
// ============================================================
#include <cstdint>
#include <string>

namespace bmp {

// 把 8 位灰度像素写成 24bpp BMP。
// pixels: 行优先，索引 = y*width + x，y=0 是图像顶部。
bool WriteGray8(const std::string& path, const uint8_t* pixels,
                uint32_t width, uint32_t height);

} // namespace bmp
