#include "zip_local.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fails = 0;

static void expect(bool cond, const char* msg)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", msg);
        ++g_fails;
    }
}

static void putU16(std::vector<uint8_t>& b, size_t off, uint16_t v)
{
    b[off] = uint8_t(v);
    b[off + 1] = uint8_t(v >> 8);
}

static void putU32(std::vector<uint8_t>& b, size_t off, uint32_t v)
{
    putU16(b, off, uint16_t(v));
    putU16(b, off + 2, uint16_t(v >> 16));
}

static std::vector<uint8_t> localHeader(uint16_t nameLen, uint16_t extraLen, uint32_t compSize,
                                        const char* name)
{
    std::vector<uint8_t> b(30u + nameLen + extraLen + compSize, 0);
    b[0] = 'P';
    b[1] = 'K';
    b[2] = 3;
    b[3] = 4;
    putU32(b, 18, compSize);
    putU32(b, 22, compSize);
    putU16(b, 26, nameLen);
    putU16(b, 28, extraLen);
    if (name && nameLen)
        std::memcpy(b.data() + 30, name, nameLen);
    return b;
}

int main()
{
    {
        auto b = localHeader(11, 0, 4, "game.lua\0\0\0");
        // name is 8 chars; fix nameLen to 8 and payload "ABCD"
        b = localHeader(8, 0, 4, "game.lua");
        std::memcpy(b.data() + 30 + 8, "ABCD", 4);
        ZipLocalView v;
        expect(readZipLocal(b.data(), b.size(), 0, &v), "small stored entry");
        expect(v.compSize == 4 && v.dataOff == 38 && v.nextPos == b.size(), "span");
        expect(v.nameLen == 8, "name len");
    }
    {
        // compSize 0xFFFFFFFF used to wrap int32 (dataOff + int(compSize)) and pass
        // the old bounds check. Must reject.
        std::vector<uint8_t> b(30, 0);
        b[0] = 'P';
        b[1] = 'K';
        b[2] = 3;
        b[3] = 4;
        putU32(b, 18, 0xFFFFFFFFu);
        putU32(b, 22, 0xFFFFFFFFu);
        ZipLocalView v;
        expect(!readZipLocal(b.data(), b.size(), 0, &v), "zip64 size placeholder");
        expect(!v.valid, "not valid");
    }
    {
        std::vector<uint8_t> b(30, 0);
        b[0] = 'P';
        b[1] = 'K';
        b[2] = 3;
        b[3] = 4;
        putU16(b, 26, 1000); // name claims 1000 bytes, buffer ends at 30
        ZipLocalView v;
        expect(!readZipLocal(b.data(), b.size(), 0, &v), "name past eof");
    }
    {
        ZipLocalView v;
        expect(!readZipLocal(nullptr, 100, 0, &v), "null buffer");
        uint8_t tiny[4] = {0x50, 0x4b, 0x03, 0x04};
        expect(!readZipLocal(tiny, 4, 0, &v), "truncated header");
        expect(!readZipLocal(tiny, 4, 0, nullptr), "null out");
    }
    {
        // Two entries; second header must not be treated as payload of the first.
        auto a = localHeader(4, 0, 2, "a.lu");
        std::memcpy(a.data() + 34, "ZZ", 2);
        auto c = localHeader(4, 0, 1, "b.mf");
        a.insert(a.end(), c.begin(), c.end());
        ZipLocalView v;
        expect(readZipLocal(a.data(), a.size(), 0, &v), "first");
        ZipLocalView v2;
        expect(readZipLocal(a.data(), a.size(), v.nextPos, &v2), "second");
        expect(v2.nextPos == a.size(), "consumed");
    }

    if (g_fails) {
        std::fprintf(stderr, "%d failed\n", g_fails);
        return 1;
    }
    std::puts("ok");
    return 0;
}
