#pragma once

enum class UISound
{
    Select,
    ToggleOn,
    ToggleOff,
    Open,
    Close,
};

void PlayUISound(UISound sound);
