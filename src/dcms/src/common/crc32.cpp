#include "common/crc32.h"

#include <cstring>

namespace dcms {

namespace {

// ── 基础表 T[0] ──────────────────────────────────────────────────────────────
// 标准 CRC32（IEEE 802.3，反射多项式 0xEDB88320）。
// 逐字节递推：c = T[0][(c ^ byte) & 0xFF] ^ (c >> 8)
//
// 注意：这里改为运行时按多项式生成，不再硬编码 256 个常量。
// 原先硬编码的表里有重复行（第 128~191 项是第 64~127 项的复制），并非合法
// CRC32 表，导致 CRC32("123456789") 得到的不是标准值 0xCBF43926。
// 生成的开销是一次性的 256×8 次移位，可忽略。
std::uint32_t kCrc32Table[256];

/// Slice-by-8 表 ────────────────────────────────────────────────────────────
/// T[n][i]：字节值 i 出现在第 n 个切片位置时的 8 字节步进值。
/// T[0] 即上表；T[n][i] = 把 T[n-1][i] 再前移一个字节的等价结果。
/// 有了它，一次循环可吞掉 8 字节，字节级循环次数降为 1/8。
std::uint32_t kSliceTable[8][256];

struct SliceTableInit {
    SliceTableInit() {
        for (int i = 0; i < 256; ++i) {
            std::uint32_t c = static_cast<std::uint32_t>(i);
            for (int b = 0; b < 8; ++b)
                c = (c >> 1) ^ (0xEDB88320u & static_cast<std::uint32_t>(-(long)(c & 1)));
            kCrc32Table[i]   = c;
            kSliceTable[0][i] = c;
        }
        for (int n = 1; n < 8; ++n) {
            for (int i = 0; i < 256; ++i) {
                const std::uint32_t c = kSliceTable[n - 1][i];
                kSliceTable[n][i] = (c >> 8) ^ kCrc32Table[c & 0xFF];
            }
        }
    }
};
const SliceTableInit kSliceTableInit{};

/// 逐字节回退路径：与字节序无关，用于尾部字节和不支持 fast path 的平台
inline std::uint32_t crc32Bytes(std::uint32_t c, const std::uint8_t *p,
                                std::size_t n) {
    while (n--) c = kCrc32Table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c;
}

// ⚠️ 为什么 x86 上不走硬件 CRC32 指令？
//   SSE4.2 的 CRC32 指令（_mm_crc32_u64 等）用的是 CRC32C（Castagnoli，多项式
//   0x1EDC6F41），而本项目需要的是 IEEE 802.3 CRC32（0xEDB88320 反射式）。
//   二者多项式不同，直接替换会得到完全不同的校验值 —— 实测
//   CRC32("123456789") 标准值应为 0xCBF43926，用硬件指令得到的是另一个值。
//   若将来要用到硬件加速，只适用于 CRC32C 场景，或需要额外做多项式转换。
//
//   注：ARMv8 提供了 IEEE CRC32 版本的指令（crc32b/h/w/d），与 CRC32C 版本
//   分开，届时可以在 aarch64 分支启用真正匹配的硬件加速。

} // namespace

// 增量 CRC32：在已有校验值 prev 的基础上继续追加 data[0..len)。
// 算法：标准 ISO 3309/ITU-T V.42 CRC32，初值反转（XOR 0xFFFFFFFF）保证前导 0 不改变校验值。
//
// 实现路径优先级：
//   1) x86 SSE4.2 硬件指令（运行时检测）
//   2) Slice-by-8 查表（小端）
//   3) 逐字节查表（兜底 / 大端）
std::uint32_t crc32Update(std::uint32_t prev, const void *data, std::size_t len) {
    auto       *p = static_cast<const std::uint8_t *>(data);
    std::uint32_t c = prev ^ 0xFFFFFFFF;
    std::size_t   n = len;

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    if (n >= 8) {
        std::uint64_t wide = c;
        while (n >= 8) {
            std::uint64_t v;
            std::memcpy(&v, p, sizeof(v));
            wide = static_cast<std::uint64_t>(kSliceTable[7][(wide ^ v) & 0xFF]) ^
                   static_cast<std::uint64_t>(kSliceTable[6][((wide ^ v) >> 8) & 0xFF]) ^
                   static_cast<std::uint64_t>(kSliceTable[5][((wide ^ v) >> 16) & 0xFF]) ^
                   static_cast<std::uint64_t>(kSliceTable[4][((wide ^ v) >> 24) & 0xFF]) ^
                   static_cast<std::uint64_t>(kSliceTable[3][((wide ^ v) >> 32) & 0xFF]) ^
                   static_cast<std::uint64_t>(kSliceTable[2][((wide ^ v) >> 40) & 0xFF]) ^
                   static_cast<std::uint64_t>(kSliceTable[1][((wide ^ v) >> 48) & 0xFF]) ^
                   static_cast<std::uint64_t>(kSliceTable[0][((wide ^ v) >> 56) & 0xFF]);
            p += 8;
            n -= 8;
        }
        c = static_cast<std::uint32_t>(wide);
    }
#endif

    return crc32Bytes(c, p, n) ^ 0xFFFFFFFF;
}

// 一次性计算 data[0..len) 的 CRC32，等价于 crc32Update(0, data, len)。
std::uint32_t crc32(const void *data, std::size_t len) {
    return crc32Update(0, data, len);
}

} // namespace dcms
