#pragma once

#include <stddef.h>
#include <stdint.h>

// One ZIP local-file header, bounds-checked against the buffer.
// compSize is kept as uint32 so a 0xFFFFFFFF ZIP64 placeholder cannot wrap a
// signed int and walk off the allocation (that showed up as a wild write).
struct ZipLocalView {
    bool valid = false;
    uint16_t method = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    size_t nameOff = 0;
    size_t nameLen = 0;
    size_t dataOff = 0;
    size_t nextPos = 0;
};

inline bool readZipLocal(const uint8_t* raw, size_t size, size_t pos, ZipLocalView* out)
{
    if (!out)
        return false;
    *out = {};
    if (!raw || pos > size || size - pos < 30)
        return false;

    const uint8_t* h = raw + pos;
    const uint32_t magic = uint32_t(h[0]) | (uint32_t(h[1]) << 8) | (uint32_t(h[2]) << 16)
                           | (uint32_t(h[3]) << 24);
    if (magic != 0x04034b50u)
        return false;

    auto u16 = [&](int off) -> uint32_t {
        return uint32_t(h[off]) | (uint32_t(h[off + 1]) << 8);
    };
    auto u32 = [&](int off) -> uint32_t { return u16(off) | (u16(off + 2) << 16); };

    const uint32_t compSize = u32(18);
    const uint32_t nameLen = u16(26);
    const uint32_t extraLen = u16(28);
    const uint64_t dataOff = uint64_t(pos) + 30u + uint64_t(nameLen) + uint64_t(extraLen);
    if (dataOff > uint64_t(size))
        return false;
    const uint64_t end = dataOff + uint64_t(compSize);
    if (end < dataOff || end > uint64_t(size))
        return false;

    out->valid = true;
    out->method = static_cast<uint16_t>(u16(8));
    out->compSize = compSize;
    out->uncompSize = u32(22);
    out->nameOff = pos + 30;
    out->nameLen = nameLen;
    out->dataOff = static_cast<size_t>(dataOff);
    out->nextPos = static_cast<size_t>(end);
    return true;
}
