#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <unordered_map>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include "DevLog.h"
#include "RegionChanger.h"
#include "UISound.h"
#include "icons.h"
#include "fontello_data.h"
#include "lucide_icons.h"
#include "LucideIconsCompressed.h"

#include "MinHook.h"

#include "imgui_internal.h"
#include "fonts_data.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef long(__stdcall* Present_t)(IDXGISwapChain*, UINT, UINT);
static Present_t Present_o = nullptr;

static ID3D11Device*           g_pDevice  = nullptr;
static ID3D11DeviceContext*    g_pContext = nullptr;
static ID3D11RenderTargetView* g_pRTV     = nullptr;
static HWND g_hGameWnd = nullptr;

static bool g_bInit = false;
static ImFont* g_pIconFont = nullptr;
static ImFont* g_pLucideFont = nullptr;
static bool g_bFailed = false;
static bool g_bMenuOpen = false;
static float g_fOpenAnim = 0.f;
static ImVec2 g_vAnimCenter = ImVec2(0.f, 0.f);
static bool g_bInsertWasDown = false;
static ULONGLONG g_ullInjectTick = 0;

static ImVec2 g_vMenuPos(-1.f, -1.f);

typedef void(__fastcall* IsRelativeMouseMode_t)(void*, bool);
static IsRelativeMouseMode_t IsRelativeMouseMode_o = nullptr;

static void* g_pInputSystem = nullptr;

static void* GetInputSystem()
{
    if (g_pInputSystem)
        return g_pInputSystem;

    HMODULE hMod = GetModuleHandleA("inputsystem.dll");
    if (!hMod)
        return nullptr;
    auto pfnCreate = reinterpret_cast<void* (*)(const char*, int*)>(
        GetProcAddress(hMod, "CreateInterface"));
    if (!pfnCreate)
        return nullptr;
    g_pInputSystem = pfnCreate("InputSystemVersion001", nullptr);
    return g_pInputSystem;
}

static void __fastcall Hook_IsRelativeMouseMode(void* pInputSystem, bool bActive)
{
    if (pInputSystem)
        g_pInputSystem = pInputSystem;
    if (g_bMenuOpen)
        return IsRelativeMouseMode_o(pInputSystem, false);
    return IsRelativeMouseMode_o(pInputSystem, bActive);
}

static WNDPROC g_WndProc_o = nullptr;

static LRESULT WINAPI GameWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (g_bInit && g_bMenuOpen)
    {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam) == 0)
            if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST)
                return true;
    }

    return CallWindowProcA(g_WndProc_o, hwnd, uMsg, wParam, lParam);
}

static void* FindPattern(HMODULE hMod, const char* szPattern)
{
    if (!hMod)
        return nullptr;

    auto* pDos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    auto* pNt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(hMod) + pDos->e_lfanew);
    if (pNt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

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
            return nullptr;
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
    return nullptr;
}

typedef bool(__fastcall* IsInGame_t)(void*);
static IsInGame_t IsInGame_o = nullptr;
static void* g_pEngineToClient = nullptr;

static bool EngineIsInGame()
{
    if (!g_pEngineToClient)
    {
        HMODULE hEngine = GetModuleHandleA("engine2.dll");
        if (!hEngine)
            return false;
        auto pfnCreate = reinterpret_cast<void* (*)(const char*, int*)>(
            GetProcAddress(hEngine, "CreateInterface"));
        if (!pfnCreate)
            return false;
        g_pEngineToClient = pfnCreate("Source2EngineToClient001", nullptr);
        if (!g_pEngineToClient)
            return false;
    }

    if (!IsInGame_o)
    {
        void* pTarget = FindPattern(GetModuleHandleA("engine2.dll"),
            "48 8B ? ? ? ? ? 48 85 C0 74 15 80 B8 ? ? ? ? ? 75 0C 83 B8 ? ? ? ? 06");
        if (!pTarget)
            return false;
        IsInGame_o = reinterpret_cast<IsInGame_t>(pTarget);
    }

    return IsInGame_o(g_pEngineToClient);
}

static ImVec4 g_vAccentColor = {52.f / 255.f, 255.f / 255.f, 0.f / 255.f, 1.f};

static void ApplyTheme()
{
    ImGuiStyle& st = ImGui::GetStyle();
    const ImVec4& accent = g_vAccentColor;
    const ImVec4 bg     = {0.075f, 0.075f, 0.090f, 0.97f};
    const ImVec4 ctrl   = {0.145f, 0.145f, 0.170f, 1.f};
    const ImVec4 hover  = {0.220f, 0.205f, 0.270f, 1.f};
    const ImVec4 active = {accent.x * 0.55f, accent.y * 0.55f, accent.z * 0.60f, 1.f};

    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]        = bg;
    c[ImGuiCol_ChildBg]         = ImVec4(0.10f, 0.10f, 0.12f, 0.60f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.10f, 0.10f, 0.12f, 0.98f);
    c[ImGuiCol_Border]          = ImVec4(accent.x, accent.y, accent.z, 0.28f);
    c[ImGuiCol_FrameBg]         = ctrl;
    c[ImGuiCol_FrameBgHovered]  = hover;
    c[ImGuiCol_FrameBgActive]   = active;
    c[ImGuiCol_TitleBg]         = ImVec4(0.09f, 0.09f, 0.11f, 1.f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.12f, 0.11f, 0.15f, 1.f);
    c[ImGuiCol_CheckMark]       = accent;
    c[ImGuiCol_SliderGrab]      = accent;
    c[ImGuiCol_SliderGrabActive]= ImVec4(accent.x, accent.y, accent.z, 1.f);
    c[ImGuiCol_Button]          = ctrl;
    c[ImGuiCol_ButtonHovered]   = hover;
    c[ImGuiCol_ButtonActive]    = active;
    c[ImGuiCol_Header]          = ImVec4(accent.x, accent.y, accent.z, 0.25f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(accent.x, accent.y, accent.z, 0.38f);
    c[ImGuiCol_HeaderActive]    = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_Separator]       = ImVec4(accent.x, accent.y, accent.z, 0.22f);
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0.06f, 0.06f, 0.07f, 0.60f);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.30f, 0.28f, 0.38f, 1.f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_ScrollbarGrabActive]  = accent;
    c[ImGuiCol_Text]            = ImVec4(0.92f, 0.92f, 0.95f, 1.f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.45f, 0.45f, 0.50f, 1.f);
}

static void Init(IDXGISwapChain* pSwap)
{
    DEV_LOG("DllMain: first Present — initializing");

    DXGI_SWAP_CHAIN_DESC sd;
    if (FAILED(pSwap->GetDesc(&sd)))
    {
        DEV_ERR("Init: swapchain GetDesc failed");
        g_bFailed = true;
        return;
    }
    g_hGameWnd = sd.OutputWindow;
    DEV_LOG("Init: game window=%p (%dx%d, %d buffers)",
            g_hGameWnd, sd.BufferDesc.Width, sd.BufferDesc.Height, sd.BufferCount);

    ID3D11Device* pDev = nullptr;
    if (FAILED(pSwap->GetDevice(IID_PPV_ARGS(&pDev))) || !pDev)
    {
        DEV_ERR("Init: GetDevice failed");
        g_bFailed = true;
        return;
    }

    ID3D11DeviceContext* pCtx = nullptr;
    pDev->GetImmediateContext(&pCtx);

    ID3D11Texture2D* pBB = nullptr;
    if (FAILED(pSwap->GetBuffer(0, IID_PPV_ARGS(&pBB))) || !pBB)
    {
        DEV_ERR("Init: GetBuffer(0) failed");
        pDev->Release();
        g_bFailed = true;
        return;
    }
    ID3D11RenderTargetView* pRTV = nullptr;
    pDev->CreateRenderTargetView(pBB, nullptr, &pRTV);
    pBB->Release();

    g_pDevice = pDev;
    g_pContext = pCtx;
    g_pRTV = pRTV;
    DEV_LOG("Init: device=%p context=%p rtv=%p", pDev, pCtx, pRTV);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding    = 8.f;
        st.FrameRounding     = 5.f;
        st.ChildRounding     = 6.f;
        st.GrabRounding      = 5.f;
        st.PopupRounding     = 6.f;
        st.ScrollbarRounding = 6.f;
        st.WindowBorderSize  = 1.f;
        st.FrameBorderSize   = 0.f;
        st.WindowPadding     = ImVec2(12.f, 12.f);
        st.FramePadding      = ImVec2(8.f, 4.f);
        st.ItemSpacing       = ImVec2(8.f, 6.f);
        st.ScrollbarSize     = 12.f;
        ApplyTheme();
    }

    {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
        builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
        static ImVector<ImWchar> ranges;
        builder.BuildRanges(&ranges);

        if (!inter.empty())
            io.Fonts->AddFontFromMemoryTTF(inter.data(), (int)inter.size(), 16.f, nullptr, ranges.Data);

        static const ImWchar iconRange[] = {0xE000, 0xF8FF, 0};
        ImFontConfig iconCfg;
        iconCfg.FontDataOwnedByAtlas = false;
        g_pIconFont = io.Fonts->AddFontFromMemoryTTF(fontello_data::ttf, (int)fontello_data::ttf_size,
                                                     16.f, &iconCfg, iconRange);

        g_pLucideFont = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
            LucideIcons_compressed_data_base85, 16.f, nullptr,
            reinterpret_cast<const ImWchar*>(g_LucideIconRanges));
    }

    if (!ImGui_ImplWin32_Init(g_hGameWnd))
    {
        DEV_ERR("Init: ImGui_ImplWin32_Init failed");
        g_bFailed = true;
        return;
    }
    DEV_LOG("Init: ImGui Win32 backend ok (hwnd=%p)", g_hGameWnd);

    if (!ImGui_ImplDX11_Init(pDev, pCtx))
    {
        DEV_ERR("Init: ImGui_ImplDX11_Init failed");
        g_bFailed = true;
        return;
    }
    DEV_LOG("Init: ImGui DX11 backend ok");

    g_WndProc_o = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(g_hGameWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GameWndProc)));
    DEV_LOG("Init: WndProc hooked (orig=%p)", g_WndProc_o);

    {
        void* pTarget = FindPattern(GetModuleHandleA("inputsystem.dll"),
            "48 89 6C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 41 56 48 83 EC ? 0F B6 F2");
        if (pTarget)
        {
            MH_CreateHook(pTarget, &Hook_IsRelativeMouseMode,
                          reinterpret_cast<void**>(&IsRelativeMouseMode_o));
            MH_EnableHook(pTarget);
            DEV_LOG("Hook installed: 'IsRelativeMouseMode' (inputsystem.dll) @ %p", pTarget);
        }
        else
        {
            DEV_WARN("Hook pattern NOT FOUND: 'IsRelativeMouseMode' (inputsystem.dll)");
            DEV_WARN("raw input will stay active while menu is open");
        }
    }

    if (RegionChanger::Init())
        DEV_LOG("RegionChanger: BSend hook ready (client.dll)");
    else
        DEV_WARN("RegionChanger: BSend pattern not found — region spoof disabled");

    g_bInit = true;
    DEV_LOG("Init: OK (%.1fs after inject) — INSERT toggles menu",
            (GetTickCount64() - g_ullInjectTick) / 1000.f);
}

static int g_nLanguage = 0;

static const char* Tr(const char* en, const char* ru)
{
    return g_nLanguage == 1 ? ru : en;
}

static std::unordered_map<ImGuiID, float> g_mapHoverAnim;
static std::unordered_map<ImGuiID, float> g_mapActiveAnim;

static void AnimEase(float& v, float target, float speed)
{
    const float dt = ImGui::GetIO().DeltaTime;
    v = v + (target - v) * (1.f - expf(-speed * dt));
    if (fabsf(v - target) < 0.001f)
        v = target;
}

static bool AnimSelectable(const char* label, bool selected, const ImVec2& sizeArg = ImVec2(0.f, 0.f))
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;

    const ImGuiID id = window->GetID(label);
    const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
    ImVec2 sz(sizeArg.x != 0.f ? sizeArg.x : labelSize.x, sizeArg.y != 0.f ? sizeArg.y : labelSize.y);
    ImVec2 pos = window->DC.CursorPos;
    pos.y += window->DC.CurrLineTextBaseOffset;
    ImGui::ItemSize(sz, 0.f);

    const float minX = pos.x;
    const float maxX = window->WorkRect.Max.x;
    if (sizeArg.x == 0.f)
        sz.x = ImMax(labelSize.x, maxX - minX);

    const ImRect bb(minX, pos.y, minX + sz.x, pos.y + sz.y);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

    float& fHover = g_mapHoverAnim[id];
    AnimEase(fHover, (hovered || held || selected) ? 1.f : 0.f, 18.f);

    ImDrawList* dl = window->DrawList;
    if (fHover > 0.001f)
    {
        const ImU32 uCol = (held && hovered) ? ImGui::GetColorU32(ImGuiCol_HeaderActive)
                          : selected          ? ImGui::GetColorU32(ImGuiCol_HeaderActive)
                                              : ImGui::GetColorU32(ImGuiCol_HeaderHovered);
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(uCol);
        c.w *= fHover;
        dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(c), style.FrameRounding);
    }

    ImGui::RenderTextClipped(pos, ImVec2(ImMin(pos.x + sz.x, window->WorkRect.Max.x), pos.y + sz.y),
                              label, nullptr, &labelSize, style.SelectableTextAlign, &bb);
    return pressed;
}

static bool AnimButtonEx(const char* label, const ImVec2& sizeArg, ImGuiButtonFlags flags = 0)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;

    const ImGuiID id = window->GetID(label);
    const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);

    ImVec2 pos = window->DC.CursorPos;
    if ((flags & ImGuiButtonFlags_AlignTextBaseLine) && style.FramePadding.y < window->DC.CurrLineTextBaseOffset)
        pos.y += window->DC.CurrLineTextBaseOffset - style.FramePadding.y;
    const ImVec2 size = ImGui::CalcItemSize(sizeArg, labelSize.x + style.FramePadding.x * 2.f,
                                            labelSize.y + style.FramePadding.y * 2.f);
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(size, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, flags);

    float& fHover = g_mapHoverAnim[id];
    float& fActive = g_mapActiveAnim[id];
    AnimEase(fHover, (hovered || held) ? 1.f : 0.f, 18.f);
    AnimEase(fActive, held ? 1.f : 0.f, 22.f);

    const ImVec4 base = style.Colors[ImGuiCol_Button];
    const ImVec4 hoverCol = style.Colors[ImGuiCol_ButtonHovered];
    const ImVec4 activeCol = style.Colors[ImGuiCol_ButtonActive];
    const ImVec4 col = ImLerp(ImLerp(base, hoverCol, fHover), activeCol, fActive);

    ImDrawList* dl = window->DrawList;
    dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(col), style.FrameRounding);
    ImGui::RenderTextClipped(bb.Min + style.FramePadding, bb.Max - style.FramePadding, label, nullptr, &labelSize, style.ButtonTextAlign, &bb);
    return pressed;
}

static bool AnimSmallButton(const char* label)
{
    ImGuiContext& g = *GImGui;
    const float backupPaddingY = g.Style.FramePadding.y;
    g.Style.FramePadding.y = 0.f;
    const bool pressed = AnimButtonEx(label, ImVec2(0.f, 0.f), ImGuiButtonFlags_AlignTextBaseLine);
    g.Style.FramePadding.y = backupPaddingY;
    return pressed;
}

static bool AnimToggle(const char* id, bool* pValue)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const ImGuiID gid = window->GetID(id);
    const ImVec2 size(35.f, 20.f);
    ImVec2 pos = window->DC.CursorPos;
    pos.y += window->DC.CurrLineTextBaseOffset;
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(size, 0.f);
    if (!ImGui::ItemAdd(bb, gid))
        return false;

    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);
    if (pressed)
    {
        *pValue = !*pValue;
        PlayUISound(*pValue ? UISound::ToggleOn : UISound::ToggleOff);
    }

    float& fActive = g_mapActiveAnim[gid];
    float& fHover = g_mapHoverAnim[gid];
    AnimEase(fActive, *pValue ? 1.f : 0.f, 16.f);
    AnimEase(fHover, (hovered || held) ? 1.f : 0.f, 24.f);

    ImDrawList* dl = window->DrawList;
    const float round = size.y * 0.5f;

    const ImVec4 offCol = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
    const ImVec4 onCol  = g_vAccentColor;
    const ImVec4 track  = ImLerp(ImLerp(offCol, ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered], fHover), onCol, fActive);
    dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(track), round);

    const float knobR = 7.f;
    const float travel = size.x - knobR * 2.f - 6.f;
    const float knobX = bb.Min.x + knobR + 3.f + travel * fActive;
    const float knobY = bb.GetCenter().y;
    dl->AddCircleFilled(ImVec2(knobX, knobY), knobR, ImGui::GetColorU32(ImGuiCol_Text), 24);

    return pressed;
}

static bool AnimColorSwatch(const char* id, ImVec4* pColor)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const ImGuiID gid = window->GetID(id);
    const ImVec2 size(20.f, 20.f);
    ImVec2 pos = window->DC.CursorPos;
    pos.y += window->DC.CurrLineTextBaseOffset;
    const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    ImGui::ItemSize(size, 0.f);
    if (!ImGui::ItemAdd(bb, gid))
        return false;

    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(bb, gid, &hovered, &held);

    ImDrawList* dl = window->DrawList;
    const float round = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(ImGuiCol_FrameBg), round);
    dl->AddRectFilled(ImVec2(bb.Min.x + 3.f, bb.Min.y + 3.f), ImVec2(bb.Max.x - 3.f, bb.Max.y - 3.f),
                       ImGui::GetColorU32(*pColor), ImMax(0.f, round - 1.f));

    char popupId[64];
    ImFormatString(popupId, IM_ARRAYSIZE(popupId), "##colorpopup_%s", id);
    if (pressed)
        ImGui::OpenPopup(popupId);

    bool bChanged = false;
    ImGui::SetNextWindowPos(ImVec2(bb.Max.x - 130.f, bb.Max.y + 6.f), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 8.f));
    if (ImGui::BeginPopup(popupId))
    {
        ImGui::SetNextItemWidth(110.f);
        if (ImGui::ColorPicker4("##picker", (float*)pColor,
                                ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoAlpha |
                                ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_DisplayRGB))
        {
            bChanged = true;
            ApplyTheme();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    return pressed || bChanged;
}

static const char* kRegionNames[RegionChanger::COUNT] =
{
    "ROW", "Europe", "SE Asia", "S America", "Russia", "Oceania"
};

static const char* kRegionNamesRu[RegionChanger::COUNT] =
{
    "ROW", "Европа", "Юго-Вост. Азия", "Южная Америка", "Россия", "Океания"
};

struct DataCenter { int id; int mode; const char* name; const char* nameRu; };
static const DataCenter kDataCenters[] = {
    { 0,  -1, "Any", "Любой" },
    { 1,   0, "US West", "Запад США" }, { 2, 0, "US East", "Восток США" }, { 22, 0, "US South-West", "Юго-запад США" },
    { 23,  0, "US South-East", "Юго-восток США" }, { 27, 0, "US North-Central", "Северо-центр США" }, { 31, 0, "US South-Central", "Юго-центр США" },
    { 3,   1, "France", "Франция" }, { 45, 1, "England", "Англия" }, { 52, 1, "Germany", "Германия" },
    { 54,  1, "Frankfurt", "Франкфурт" }, { 55, 1, "Stockholm", "Стокгольм" }, { 56, 1, "London", "Лондон" },
    { 8,   1, "Sweden", "Швеция" }, { 9, 1, "Italy", "Италия" }, { 21, 1, "Spain", "Испания" }, { 28, 1, "Poland", "Польша" }, { 44, 1, "Finland", "Финляндия" },
    { 11,  1, "South Africa", "Южная Африка" },
    { 5,   2, "Singapore", "Сингапур" }, { 24, 2, "Hong Kong", "Гонконг" }, { 19, 2, "Japan", "Япония" }, { 39, 2, "South Korea", "Южная Корея" },
    { 10,  3, "South America", "Южная Америка" }, { 14, 3, "Chile", "Чили" }, { 15, 3, "Peru", "Перу" }, { 38, 3, "Argentina", "Аргентина" },
    { 7,   5, "Australia", "Австралия" },
};

static void RenderMenu()
{
    ImGuiIO& io = ImGui::GetIO();

    static const ImVec2 s_vMenuSize(460.f, 355.f);

    if (g_vMenuPos.x < 0.f)
    {
        g_vMenuPos = {io.DisplaySize.x * 0.5f - s_vMenuSize.x * 0.5f,
                      io.DisplaySize.y * 0.5f - s_vMenuSize.y * 0.5f};
        DEV_LOG("Menu: default pos=%.0fx%.0f (screen %.0fx%.0f)",
                g_vMenuPos.x, g_vMenuPos.y, io.DisplaySize.x, io.DisplaySize.y);
    }

    ImGui::SetNextWindowPos(g_vMenuPos, ImGuiCond_Once);
    ImGui::SetNextWindowSize(s_vMenuSize, ImGuiCond_Always);
    ImGui::Begin("dysonbehind", nullptr,
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", Tr("Region Changer", "Смена региона"));
    ImGui::SameLine();
    AnimToggle("##region_changer_toggle", &RegionChanger::g_bEnabled);
    ImGui::SameLine();
    if (AnimSmallButton(Tr("RU", "EN")))
    {
        PlayUISound(UISound::Select);
        g_nLanguage = (g_nLanguage == 1) ? 0 : 1;
        DEV_LOG("Menu: language -> %s", g_nLanguage == 1 ? "RU" : "EN");
    }
    ImGui::SameLine();
    AnimColorSwatch("##accent_color", &g_vAccentColor);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (g_pLucideFont)
    {
        ImGui::PushFont(g_pLucideFont);
        ImGui::TextUnformatted(ICON_LC_GLOBE);
        ImGui::PopFont();
        ImGui::SameLine();
    }
    ImGui::Text("%s", Tr("Region:", "Регион:"));
    ImGui::SameLine(230.f);
    if (g_pLucideFont)
    {
        ImGui::PushFont(g_pLucideFont);
        ImGui::TextUnformatted(ICON_LC_SERVER);
        ImGui::PopFont();
        ImGui::SameLine();
    }
    ImGui::Text("%s", Tr("Server:", "Сервер:"));
    ImGui::Spacing();

    static int s_nPrevRegion = -2;
    static int s_nPrevDataCenter = -2;
    static bool s_bPrevEnabled = false;

    ImGui::BeginDisabled(!RegionChanger::g_bEnabled);

    ImGui::BeginChild("##regions", ImVec2(210.f, 200.f), false);
    for (int i = 0; i < RegionChanger::COUNT; ++i)
    {
        const bool active = (RegionChanger::g_nRegion == i);
        const char* szName = (g_nLanguage == 1) ? kRegionNamesRu[i] : kRegionNames[i];
        if (AnimSelectable(szName, active))
        {
            PlayUISound(UISound::Select);
            RegionChanger::g_nRegion = i;
        }
        if (active)
            ImGui::SetItemDefaultFocus();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##servers", ImVec2(0.f, 200.f), false);
    for (const auto& dc : kDataCenters)
    {
        if (dc.mode != -1 && dc.mode != RegionChanger::g_nRegion)
            continue;
        const bool active = (RegionChanger::g_nDataCenter == dc.id);
        const char* szName = (g_nLanguage == 1) ? dc.nameRu : dc.name;
        if (AnimSelectable(szName, active))
        {
            PlayUISound(UISound::Select);
            RegionChanger::g_nDataCenter = dc.id;
        }
    }
    ImGui::EndChild();

    bool bKnown = false;
    for (const auto& dc : kDataCenters)
        if (dc.id == RegionChanger::g_nDataCenter && (dc.mode == -1 || dc.mode == RegionChanger::g_nRegion))
            bKnown = true;
    if (!bKnown)
        RegionChanger::g_nDataCenter = 0;

    ImGui::EndDisabled();

    if (s_bPrevEnabled != RegionChanger::g_bEnabled)
    {
        s_bPrevEnabled = RegionChanger::g_bEnabled;
        DEV_LOG("Menu: Region Changer %s", RegionChanger::g_bEnabled ? "ON" : "OFF");
    }
    if (s_nPrevRegion != RegionChanger::g_nRegion)
    {
        s_nPrevRegion = RegionChanger::g_nRegion;
        DEV_LOG("Menu: region -> %s (%d)", kRegionNames[RegionChanger::g_nRegion],
                RegionChanger::g_nRegion);
    }
    if (s_nPrevDataCenter != RegionChanger::g_nDataCenter)
    {
        s_nPrevDataCenter = RegionChanger::g_nDataCenter;
        const char* szServer = "Any";
        for (const auto& dc : kDataCenters)
            if (dc.id == RegionChanger::g_nDataCenter) szServer = dc.name;
        DEV_LOG("Menu: server -> %s (%d)", szServer, RegionChanger::g_nDataCenter);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s", Tr("INSERT - close", "INSERT - закрыть"));

    g_vMenuPos = ImGui::GetWindowPos();
    {
        const ImVec2 vSize = ImGui::GetWindowSize();
        g_vAnimCenter = ImVec2(g_vMenuPos.x + vSize.x * 0.5f, g_vMenuPos.y + vSize.y * 0.5f);
    }

    ImGui::End();
}

static void UpdateOpenAnim()
{
    float flDt = ImGui::GetIO().DeltaTime;
    if (flDt < 0.f) flDt = 0.f;
    if (flDt > 1.f / 20.f) flDt = 1.f / 20.f;

    const float flTarget = g_bMenuOpen ? 1.f : 0.f;
    const float flSpeed = g_bMenuOpen ? 14.f : 16.f;

    const float flStep = flSpeed * flDt;
    g_fOpenAnim += (flTarget - g_fOpenAnim) * (flStep > 1.f ? 1.f : flStep);
    if (g_fOpenAnim < 0.f) g_fOpenAnim = 0.f;
    if (g_fOpenAnim > 1.f) g_fOpenAnim = 1.f;
    if (g_fOpenAnim > flTarget - 0.001f && g_fOpenAnim < flTarget + 0.001f)
        g_fOpenAnim = flTarget;
}

static float OpenReveal()
{
    float t = g_fOpenAnim;
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    const float u = 1.f - t;
    return 1.f - u * u * u;
}

static void ApplyOpenTransform(ImDrawData* pDrawData)
{
    if (!pDrawData || g_fOpenAnim == 1.f)
        return;

    const float flReveal = OpenReveal();
    if (flReveal <= 0.f)
        return;

    ImGuiContext& g = *GImGui;
    for (ImGuiWindow* pWindow : g.Windows)
    {
        if (!pWindow->Active || !pWindow->DrawList)
            continue;

        ImDrawList* dl = pWindow->DrawList;

        for (ImDrawVert& v : dl->VtxBuffer)
        {
            v.pos.x = g_vAnimCenter.x + (v.pos.x - g_vAnimCenter.x) * flReveal;
            v.pos.y = g_vAnimCenter.y + (v.pos.y - g_vAnimCenter.y) * flReveal;

            ImU32 a = (v.col & IM_COL32_A_MASK) >> IM_COL32_A_SHIFT;
            a = static_cast<ImU32>(static_cast<float>(a) * flReveal + 0.5f);
            v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
        }

        for (ImDrawCmd& cmd : dl->CmdBuffer)
        {
            cmd.ClipRect.x = g_vAnimCenter.x + (cmd.ClipRect.x - g_vAnimCenter.x) * flReveal;
            cmd.ClipRect.z = g_vAnimCenter.x + (cmd.ClipRect.z - g_vAnimCenter.x) * flReveal;
            cmd.ClipRect.y = g_vAnimCenter.y + (cmd.ClipRect.y - g_vAnimCenter.y) * flReveal;
            cmd.ClipRect.w = g_vAnimCenter.y + (cmd.ClipRect.w - g_vAnimCenter.y) * flReveal;
        }
    }
}

static void SetMenuOpen(bool bOpen)
{
    if (g_bMenuOpen == bOpen)
        return;

    g_bMenuOpen = bOpen;
    ImGui::GetIO().MouseDrawCursor = g_bMenuOpen;
    ShowCursor(!g_bMenuOpen);
    if (IsRelativeMouseMode_o)
        if (void* pInput = GetInputSystem())
            IsRelativeMouseMode_o(pInput, g_bMenuOpen ? false : true);
    PlayUISound(g_bMenuOpen ? UISound::Open : UISound::Close);
}

static bool g_bWasInGame = false;
static bool g_bAutoOpened = false;

static long __stdcall Hook_Present(IDXGISwapChain* pSwap, UINT sync, UINT flags)
{
    if (!g_bInit && !g_bFailed)
        Init(pSwap);

    if (g_bInit)
    {
        const bool bDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (bDown && !g_bInsertWasDown)
        {
            SetMenuOpen(!g_bMenuOpen);
            DEV_LOG("INSERT: menu %s", g_bMenuOpen ? "OPEN" : "CLOSED");
        }
        g_bInsertWasDown = bDown;

        const bool bInGame = EngineIsInGame();
        if (bInGame && !g_bWasInGame && !g_bAutoOpened && !g_bMenuOpen)
        {
            g_bAutoOpened = true;
            SetMenuOpen(true);
            DEV_LOG("Menu: auto-opened on match entry");
        }
        g_bWasInGame = bInGame;
    }

    if (g_bInit)
    {
        UpdateOpenAnim();

        if (g_bMenuOpen || g_fOpenAnim > 0.f)
        {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            RenderMenu();

            ImGui::Render();

            ApplyOpenTransform(ImGui::GetDrawData());

            ID3D11RenderTargetView* pPrevRTV = nullptr;
            g_pContext->OMGetRenderTargets(1, &pPrevRTV, nullptr);
            g_pContext->OMSetRenderTargets(1, &g_pRTV, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_pContext->OMSetRenderTargets(1, pPrevRTV ? &pPrevRTV : nullptr, nullptr);
            if (pPrevRTV) pPrevRTV->Release();
        }
    }

    return Present_o(pSwap, sync, flags);
}

static bool InstallPresentHook()
{
    ID3D11Device* pDev = nullptr;
    ID3D11DeviceContext* pCtx = nullptr;
    IDXGISwapChain* pSwap = nullptr;

    DXGI_RATIONAL sr{ 60, 1 };
    DXGI_MODE_DESC md{};
    md.Width = 100;
    md.Height = 100;
    md.RefreshRate = sr;
    md.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    DXGI_SAMPLE_DESC sd{ 1, 0 };
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc = md;
    scd.SampleDesc = sd;
    scd.OutputWindow = GetForegroundWindow();
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &scd, &pSwap, &pDev, nullptr, &pCtx);

    if (FAILED(hr))
    {
        DEV_ERR("InstallPresentHook: D3D11CreateDeviceAndSwapChain failed (hr=0x%08X)", hr);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(pSwap);
    void* pPresent = vtable[8];

    pSwap->Release();
    pDev->Release();
    pCtx->Release();

    if (MH_CreateHook(pPresent, &Hook_Present,
                      reinterpret_cast<void**>(&Present_o)) != MH_OK)
    {
        DEV_ERR("InstallPresentHook: MH_CreateHook failed (Present @ %p)", pPresent);
        return false;
    }

    DEV_LOG("InstallDXGIHooksFallback: Present hooked via VTable[8] @ %p", pPresent);
    return true;
}

static DWORD WINAPI InitThread(LPVOID)
{
    DEV_LOG("StartCheatThread: dll_dir=%s",
            []() { static char s_dir[MAX_PATH]{}; HMODULE h = nullptr;
                   GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCSTR>(&InitThread), &h);
                   GetModuleFileNameA(h, s_dir, MAX_PATH);
                   if (char* p = strrchr(s_dir, '\\')) *(p + 1) = '\0';
                   return s_dir; }());

    DEV_LOG("StartCheatThread: waiting for game modules...");
    for (int i = 0; i < 300; ++i)
    {
        if (GetModuleHandleA("client.dll") && GetModuleHandleA("inputsystem.dll"))
            break;
        Sleep(100);
    }

    HMODULE hClient = GetModuleHandleA("client.dll");
    HMODULE hInput = GetModuleHandleA("inputsystem.dll");
    if (!hClient)
        DEV_WARN("StartCheatThread: client.dll NOT loaded after 30s wait");
    else
        DEV_LOG("StartCheatThread: client.dll=%p", hClient);
    if (!hInput)
        DEV_WARN("StartCheatThread: inputsystem.dll NOT loaded after 30s wait");
    else
        DEV_LOG("StartCheatThread: inputsystem.dll=%p", hInput);

    if (MH_Initialize() != MH_OK)
    {
        DEV_ERR("StartCheatThread: MH_Initialize failed");
        return 1;
    }
    DEV_LOG("StartCheatThread: MinHook initialized");

    if (!InstallPresentHook())
    {
        DEV_ERR("StartCheatThread: Present hook failed — aborting");
        return 2;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        DEV_ERR("StartCheatThread: MH_EnableHook failed");
        return 3;
    }
    DEV_LOG("StartCheatThread: engine hooks installed — waiting for first Present");

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_ullInjectTick = GetTickCount64();
        DisableThreadLibraryCalls(hModule);

        char szProc[MAX_PATH]{};
        GetModuleFileNameA(nullptr, szProc, MAX_PATH);
        DEV_LOG("DllMain: DLL_PROCESS_ATTACH (host=%s)", strrchr(szProc, '\\') ? strrchr(szProc, '\\') + 1 : szProc);

        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        DEV_LOG("DllMain: DLL_PROCESS_DETACH");

        if (g_WndProc_o && g_hGameWnd)
        {
            SetWindowLongPtrA(g_hGameWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_WndProc_o));
            DEV_LOG("DllMain: WndProc restored");
        }
        if (g_bInit)
        {
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            DEV_LOG("DllMain: ImGui shut down");
        }
        if (g_pRTV) g_pRTV->Release();
        MH_Uninitialize();
    }
    return TRUE;
}
