#include "RegionChanger.h"

#include <cstring>

#include <Windows.h>

#include "DevLog.h"
#include "MinHook.h"

static constexpr uint32_t kMsgStartMM = 9010;

static constexpr uint32_t kMsgSetServerRegion = 9129;

static const char* const kRegionNames[] = { "ROW", "Europe", "SE Asia", "S America", "Russia", "Oceania" };

static const char* RegionName(uint32_t mode)
{
    return mode < 6 ? kRegionNames[mode] : "?";
}

struct DataCenterName { int id; const char* name; };
static const DataCenterName kDataCenterNames[] = {
    { 1, "US West" }, { 2, "US East" }, { 22, "US South-West" }, { 23, "US South-East" },
    { 27, "US North-Central" }, { 31, "US South-Central" },
    { 3, "France" }, { 45, "England" }, { 52, "Germany" }, { 54, "Frankfurt" },
    { 55, "Stockholm" }, { 56, "London" }, { 8, "Sweden" }, { 9, "Italy" },
    { 21, "Spain" }, { 28, "Poland" }, { 44, "Finland" }, { 11, "South Africa" },
    { 5, "Singapore" }, { 24, "Hong Kong" }, { 19, "Japan" }, { 39, "South Korea" },
    { 10, "South America" }, { 14, "Chile" }, { 15, "Peru" }, { 38, "Argentina" },
    { 7, "Australia" },
};

static const char* DataCenterNameOf(int id)
{
    for (const auto& dc : kDataCenterNames)
        if (dc.id == id) return dc.name;
    return "?";
}

static bool ReadVarint(const uint8_t* p, const uint8_t* end, uint64_t* out, const uint8_t** next)
{
    uint64_t v = 0;
    int shift = 0;
    const uint8_t* cur = p;
    while (cur < end && shift < 64)
    {
        const uint8_t b = *cur++;
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0)
        {
            *out = v;
            *next = cur;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool WriteVarint1(uint8_t* p, uint32_t v)
{
    if (v > 0x7F) return false;
    *p = static_cast<uint8_t>(v);
    return true;
}

static void* FindPattern(HMODULE hMod, const char* szPattern)
{
    if (!hMod)
    {
        DEV_ERR("FindPattern: module not loaded");
        return nullptr;
    }

    auto* pDos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        DEV_ERR("FindPattern: bad DOS header");
        return nullptr;
    }
    auto* pNt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(hMod) + pDos->e_lfanew);
    if (pNt->Signature != IMAGE_NT_SIGNATURE)
    {
        DEV_ERR("FindPattern: bad NT header");
        return nullptr;
    }

    const uintptr_t uStart = reinterpret_cast<uintptr_t>(hMod) + pNt->OptionalHeader.BaseOfCode;
    const uintptr_t uEnd = uStart + pNt->OptionalHeader.SizeOfCode;

    uint8_t pat[256];
    bool    mask[256];
    int     nLen = 0;

    const char* p = szPattern;
    while (*p && nLen < 256)
    {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?')
        {
            pat[nLen] = 0;
            mask[nLen] = false;
            ++nLen;
            while (*p == '?') ++p;
            continue;
        }
        const auto HexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = HexVal(p[0]);
        const int lo = (p[1]) ? HexVal(p[1]) : 0;
        if (hi < 0 || lo < 0)
        {
            DEV_ERR("FindPattern: bad hex in pattern");
            return nullptr;
        }
        pat[nLen] = static_cast<uint8_t>((hi << 4) | lo);
        mask[nLen] = true;
        ++nLen;
        p += 2;
    }

    for (uintptr_t u = uStart; u + nLen <= uEnd; ++u)
    {
        bool ok = true;
        for (int i = 0; i < nLen; ++i)
        {
            if (mask[i] && *reinterpret_cast<uint8_t*>(u + i) != pat[i])
            {
                ok = false;
                break;
            }
        }
        if (ok)
            return reinterpret_cast<void*>(u);
    }

    DEV_ERR("FindPattern: NOT FOUND (%d bytes, .text %llu-%llu)",
            nLen, static_cast<unsigned long long>(uStart), static_cast<unsigned long long>(uEnd));
    return nullptr;
}

static bool PatchRegionInBody(uint8_t* body, size_t bodySize, uint32_t newRegion)
{
    if (!body || bodySize < 4)
    {
        DEV_WARN("RegionSpoof: empty/short body (size=%zu)", bodySize);
        return false;
    }
    if (newRegion > 0x7F)
    {
        DEV_WARN("RegionSpoof: region value %u out of varint range", newRegion);
        return false;
    }

    const uint8_t* end = body + bodySize;
    uint8_t* p = body;

    while (p < end)
    {
        uint64_t tag = 0;
        const uint8_t* next = nullptr;
        if (!ReadVarint(p, end, &tag, &next))
            break;
        const uint32_t field = static_cast<uint32_t>(tag >> 3);
        const uint32_t wt = static_cast<uint32_t>(tag & 7);
        p = const_cast<uint8_t*>(next);

        if (wt == 0)
        {
            uint64_t dummy = 0;
            if (!ReadVarint(p, end, &dummy, &next)) break;
            p = const_cast<uint8_t*>(next);
        }
        else if (wt == 1)
        {
            if (p + 8 > end) break;
            p += 8;
        }
        else if (wt == 5)
        {
            if (p + 4 > end) break;
            p += 4;
        }
        else if (wt == 2)
        {
            uint64_t len = 0;
            if (!ReadVarint(p, end, &len, &next)) break;
            p = const_cast<uint8_t*>(next);
            if (p + len > end) break;

            if (field == 3)
            {
                uint8_t* mi = p;
                uint8_t* miEnd = p + static_cast<size_t>(len);
                while (mi < miEnd)
                {
                    uint64_t t2 = 0;
                    const uint8_t* n2 = nullptr;
                    if (!ReadVarint(mi, miEnd, &t2, &n2)) break;
                    const uint32_t f2 = static_cast<uint32_t>(t2 >> 3);
                    const uint32_t w2 = static_cast<uint32_t>(t2 & 7);
                    uint8_t* afterTag = const_cast<uint8_t*>(n2);

                    if (w2 == 0)
                    {
                        uint64_t val = 0;
                        const uint8_t* n3 = nullptr;
                        if (!ReadVarint(afterTag, miEnd, &val, &n3)) break;

                        if (f2 == 8)
                        {
                            if (n3 == afterTag + 1)
                            {
                                const uint32_t old = static_cast<uint32_t>(val);
                                if (WriteVarint1(afterTag, newRegion))
                                {
                                    DEV_LOG("RegionSpoof: region %s -> %s", RegionName(old), RegionName(newRegion));
                                    return true;
                                }
                            }
                            else
                            {
                                DEV_WARN("RegionSpoof: region varint multi-byte, skip");
                            }
                            return false;
                        }
                        mi = const_cast<uint8_t*>(n3);
                    }
                    else if (w2 == 2)
                    {
                        uint64_t l2 = 0;
                        if (!ReadVarint(afterTag, miEnd, &l2, &n2)) break;
                        mi = const_cast<uint8_t*>(n2) + static_cast<size_t>(l2);
                    }
                    else if (w2 == 1)
                    {
                        mi = afterTag + 8;
                    }
                    else if (w2 == 5)
                    {
                        mi = afterTag + 4;
                    }
                    else
                    {
                        break;
                    }
                }
                DEV_WARN("RegionSpoof: match_info present but region field not found");
                return false;
            }

            p += static_cast<size_t>(len);
        }
        else
        {
            break;
        }
    }
    DEV_WARN("RegionSpoof: match_info field not found in body");
    return false;
}

static bool PatchStartMatchmakingBuffer(void* buf, unsigned int size, uint32_t region)
{
    if (!buf || size < 12)
        return false;

    auto* p = static_cast<uint8_t*>(buf);
    uint32_t type = 0, hdrSize = 0;
    memcpy(&type, p, 4);
    memcpy(&hdrSize, p + 4, 4);

    if ((type & 0x7FFFFFFFu) != kMsgStartMM)
        return false;

    if (8u + hdrSize >= size)
    {
        DEV_WARN("RegionSpoof: StartMatchmaking buffer too small (hdr=%u size=%u)", hdrSize, size);
        return false;
    }

    uint8_t* body = p + 8 + hdrSize;
    const size_t bodySize = size - 8u - hdrSize;
    return PatchRegionInBody(body, bodySize, region);
}

typedef unsigned char(__fastcall* BSend_t)(void*, unsigned int, void*, unsigned int);
static BSend_t BSend_o = nullptr;

static unsigned char __fastcall Hook_BSend(void* self, unsigned int msgType, void* buf, unsigned int size)
{
    const uint32_t id = msgType & 0x7FFFFFFFu;
    if (RegionChanger::g_bEnabled && id == kMsgStartMM && buf && size)
    {
        if (!PatchStartMatchmakingBuffer(buf, size, static_cast<uint32_t>(RegionChanger::g_nRegion)))
            DEV_WARN("RegionSpoof: StartMatchmaking (9010) patch FAILED (size=%u)", size);
    }

    if (RegionChanger::g_bEnabled && RegionChanger::g_nDataCenter > 0
        && id == kMsgSetServerRegion && buf && size >= 8)
    {
        auto* p = static_cast<uint8_t*>(buf);
        uint32_t hdrSize = 0;
        memcpy(&hdrSize, p + 4, 4);
        if (8u + hdrSize < size)
        {
            uint8_t* body = p + 8 + hdrSize;
            const size_t bodySize = size - 8u - hdrSize;
            if (bodySize >= 2 && body[bodySize - 2] == 0x20)
            {
                const int old = body[bodySize - 1];
                body[bodySize - 1] = static_cast<uint8_t>(RegionChanger::g_nDataCenter);
                DEV_LOG("RegionSpoof: server %s -> %s", DataCenterNameOf(old),
                        DataCenterNameOf(RegionChanger::g_nDataCenter));
            }
            else
            {
                DEV_WARN("RegionSpoof: SetServerRegion (9129) unexpected layout (size=%zu)", bodySize);
            }
        }
    }

    return BSend_o(self, msgType, buf, size);
}

namespace RegionChanger
{

bool Init()
{
    HMODULE hClient = GetModuleHandleA("client.dll");
    if (!hClient)
    {
        DEV_ERR("RegionChanger: client.dll not loaded");
        return false;
    }
    DEV_LOG("RegionChanger: scanning client.dll (base=%p)", hClient);

    void* pTarget = FindPattern(hClient,
        "48 89 5C 24 ? 48 89 6C 24 ? 56 41 56 41 57 48 83 EC ? "
        "8B DA 48 8B F1 8B 89 54 02 00 00 0F BA F3 1F");

    if (!pTarget)
    {
        DEV_ERR("RegionChanger: BSend pattern NOT FOUND — game updated?");
        return false;
    }
    DEV_LOG("CBasePattern: Hook::BSend -> %p", pTarget);

    if (MH_CreateHook(pTarget, &Hook_BSend,
                      reinterpret_cast<void**>(&BSend_o)) != MH_OK)
    {
        DEV_ERR("RegionChanger: MH_CreateHook failed for BSend");
        return false;
    }

    if (MH_EnableHook(pTarget) != MH_OK)
    {
        DEV_ERR("RegionChanger: MH_EnableHook failed for BSend");
        return false;
    }

    DEV_LOG("Hook installed: 'Hook::BSend' (client.dll) @ %p", pTarget);
    return true;
}

}
