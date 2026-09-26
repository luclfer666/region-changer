#pragma once

#include <cstdint>

namespace RegionChanger
{
    enum Region : int
    {
        ROW      = 0,
        EUROPE   = 1,
        SE_ASIA  = 2,
        S_AMERICA= 3,
        RUSSIA   = 4,
        OCEANIA  = 5,
        COUNT    = 6,
    };

    inline bool  g_bEnabled    = false;
    inline int   g_nRegion     = RUSSIA;

    inline int   g_nDataCenter = 0;

    bool Init();
}
