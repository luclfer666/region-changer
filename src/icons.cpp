#include "icons.h"

namespace icons
{
    const char* to_utf8(tab icon)
    {
        switch (icon)
        {
        case tab::aimbot:   return aimbot;
        case tab::visuals:  return visuals;
        case tab::heroes:   return heroes;
        case tab::exploits: return exploits;
        case tab::config:   return config;
        case tab::settings: return settings;
        default:            return aimbot;
        }
    }
}
