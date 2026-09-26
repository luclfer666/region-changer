#include "UISound.h"

#include "Resources/UI_Sounds/UI_Select.hpp"
#include "Resources/UI_Sounds/ModuleEnable.hpp"
#include "Resources/UI_Sounds/ModuleDisable.hpp"
#include "Resources/UI_Sounds/UI_Open.hpp"
#include "Resources/UI_Sounds/UI_Close.hpp"

#include <Windows.h>
#include <mmsystem.h>
#include <cstdint>
#include <vector>

static constexpr float kVolume = 0.30f;

static void ScaleWav(const unsigned char* src, unsigned int size, float vol,
                      std::vector<unsigned char>& out)
{
    out.assign(src, src + size);
    if (vol >= 0.999f || size <= 44)
        return;

    unsigned int dataOff = 44;
    for (unsigned int i = 36; i + 8 <= size; ++i)
    {
        if (src[i] == 'd' && src[i + 1] == 'a' && src[i + 2] == 't' && src[i + 3] == 'a')
        {
            dataOff = i + 8;
            break;
        }
    }
    if (dataOff >= size)
        return;

    int16_t* samples = reinterpret_cast<int16_t*>(out.data() + dataOff);
    const unsigned int count = (size - dataOff) / 2;
    for (unsigned int i = 0; i < count; ++i)
    {
        const int scaled = static_cast<int>(static_cast<float>(samples[i]) * vol);
        if (scaled > 32767)      samples[i] = 32767;
        else if (scaled < -32768) samples[i] = -32768;
        else                      samples[i] = static_cast<int16_t>(scaled);
    }
}

static std::vector<unsigned char>& ScaledSelect()
{
    static std::vector<unsigned char> v;
    static bool bReady = false;
    if (!bReady)
    {
        ScaleWav(reinterpret_cast<const unsigned char*>(ui_select_data), ui_select_size, kVolume, v);
        bReady = true;
    }
    return v;
}

static std::vector<unsigned char>& ScaledOpen()
{
    static std::vector<unsigned char> v;
    static bool bReady = false;
    if (!bReady)
    {
        ScaleWav(reinterpret_cast<const unsigned char*>(ui_open_data), ui_open_size, kVolume, v);
        bReady = true;
    }
    return v;
}

static std::vector<unsigned char>& ScaledClose()
{
    static std::vector<unsigned char> v;
    static bool bReady = false;
    if (!bReady)
    {
        ScaleWav(reinterpret_cast<const unsigned char*>(ui_close_data), ui_close_size, kVolume, v);
        bReady = true;
    }
    return v;
}

static std::vector<unsigned char>& ScaledToggleOn()
{
    static std::vector<unsigned char> v;
    static bool bReady = false;
    if (!bReady)
    {
        ScaleWav(reinterpret_cast<const unsigned char*>(module_enable_data), module_enable_size, kVolume, v);
        bReady = true;
    }
    return v;
}

static std::vector<unsigned char>& ScaledToggleOff()
{
    static std::vector<unsigned char> v;
    static bool bReady = false;
    if (!bReady)
    {
        ScaleWav(reinterpret_cast<const unsigned char*>(module_disable_data), module_disable_size, kVolume, v);
        bReady = true;
    }
    return v;
}

void PlayUISound(UISound sound)
{
    switch (sound)
    {
    case UISound::Select:
        PlaySoundA(reinterpret_cast<LPCSTR>(ScaledSelect().data()), nullptr, SND_MEMORY | SND_ASYNC);
        break;
    case UISound::ToggleOn:
        PlaySoundA(reinterpret_cast<LPCSTR>(ScaledToggleOn().data()), nullptr, SND_MEMORY | SND_ASYNC);
        break;
    case UISound::ToggleOff:
        PlaySoundA(reinterpret_cast<LPCSTR>(ScaledToggleOff().data()), nullptr, SND_MEMORY | SND_ASYNC);
        break;
    case UISound::Open:
        PlaySoundA(reinterpret_cast<LPCSTR>(ScaledOpen().data()), nullptr, SND_MEMORY | SND_ASYNC);
        break;
    case UISound::Close:
        PlaySoundA(reinterpret_cast<LPCSTR>(ScaledClose().data()), nullptr, SND_MEMORY | SND_ASYNC);
        break;
    }
}
