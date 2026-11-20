#include "BmpWriter.h"

#include <cstdio>
#include <vector>

namespace bmp {

bool WriteGray8(const std::string& path, const uint8_t* pixels,
                uint32_t width, uint32_t height)
{
    if (pixels == nullptr || width == 0 || height == 0 || path.empty()) {
        return false;
    }

    const uint32_t bpp = 24;
    // BMP 每行字节数必须是 4 的倍数
    const uint32_t rowBytes = ((width * bpp + 31) / 32) * 4;
    const uint32_t dataSize = rowBytes * height;
    const uint32_t fileSize = 54 + dataSize;

    std::vector<uint8_t> file((size_t)fileSize, 0);
    uint8_t* p = file.data();

    // ---- 文件头（14 字节）----
    p[0] = 'B';
    p[1] = 'M';
    // 用逐字节写，避免未对齐指针转换（UB，且 ARM 上直接崩）
    const uint32_t fsz = fileSize;
    p[2] = (uint8_t)(fsz);
    p[3] = (uint8_t)(fsz >> 8);
    p[4] = (uint8_t)(fsz >> 16);
    p[5] = (uint8_t)(fsz >> 24);
    p[10] = 54;    // 像素数据偏移

    // ---- 信息头（40 字节）----
    p[14] = 40;
    const uint32_t w = width, h = height;
    p[18] = (uint8_t)(w);       p[19] = (uint8_t)(w >> 8);
    p[20] = (uint8_t)(w >> 16); p[21] = (uint8_t)(w >> 24);
    p[22] = (uint8_t)(h);       p[23] = (uint8_t)(h >> 8);
    p[24] = (uint8_t)(h >> 16); p[25] = (uint8_t)(h >> 24);
    p[26] = 1;                  // planes
    p[28] = (uint8_t)bpp;       // 位深
    p[34] = (uint8_t)(dataSize);
    p[35] = (uint8_t)(dataSize >> 8);
    p[36] = (uint8_t)(dataSize >> 16);
    p[37] = (uint8_t)(dataSize >> 24);

    // ---- 像素：BMP 从下往上存，且是 BGR 顺序 ----
    uint8_t* dst = p + 54;
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t srcY = height - 1 - y;   // 行倒序
        uint8_t* row = dst + (size_t)y * rowBytes;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t v = pixels[(size_t)srcY * width + x];
            row[(size_t)x * 3 + 0] = v;   // B
            row[(size_t)x * 3 + 1] = v;   // G
            row[(size_t)x * 3 + 2] = v;   // R
        }
    }

    FILE* fp = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&fp, path.c_str(), "wb") != 0) fp = nullptr;
#else
    fp = fopen(path.c_str(), "wb");
#endif
    if (!fp) {
        return false;
    }
    const size_t wrote = fwrite(file.data(), 1, file.size(), fp);
    fclose(fp);
    return wrote == file.size();
}

} // namespace bmp
