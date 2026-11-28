// rdx: the add-on's overlay (written for ReShade's) drawn in REFramework's menu.
//
// The add-on's ImGui calls go through ReShade's ImGui function table (reshade_overlay.hpp). Here
// that table points at REFramework's own Dear ImGui, which REFramework exports as cimgui (ig*
// functions from dinput8.dll): same ImGui instance and context as its menu, no ImGui of our own,
// no dependence on which ImGui version REFramework was built with beyond these plain calls.
// REFramework calls rdx_OnDrawUi from its menu (plugin API on_imgui_draw_ui); the overlay sits
// under an "RE4DLSS" header there.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef ImTextureID
#define ImTextureID ImU64
#endif
#include <imgui.h>
#include <reshade.hpp>

#include <cstdarg>

namespace rd {
void Log(const char* fmt, ...);
}

namespace rdx {
extern void (*g_overlay)(reshade::api::effect_runtime*);
}

namespace {

imgui_function_table g_table = {};
bool g_ready = false, g_tried = false;

// cimgui's signatures (by-value ImVec2 / ImVec4, every argument given).
struct Ig {
    const char* (*GetVersion)();
    void (*TextDisabledV)(const char*, va_list);
    void (*TextColoredV)(ImVec4, const char*, va_list);
    bool (*Checkbox)(const char*, bool*);
    void (*SetNextItemWidth)(float);
    void (*SeparatorText)(const char*);
    bool (*Combo)(const char*, int*, const char* const[], int, int);
    void (*BeginDisabled)(bool);
    void (*EndDisabled)();
    void (*SameLine)(float, float);
    bool (*BeginTabBar)(const char*, ImGuiTabBarFlags);
    void (*EndTabBar)();
    bool (*BeginTabItem)(const char*, bool*, ImGuiTabItemFlags);
    void (*EndTabItem)();
    void (*TextUnformatted)(const char*, const char*);
    bool (*SliderFloat)(const char*, float*, float, float, const char*, ImGuiSliderFlags);
    void (*Spacing)();
    void (*SetTooltipV)(const char*, va_list);
    bool (*IsItemHovered)(ImGuiHoveredFlags);
    bool (*IsItemDeactivatedAfterEdit)();
    float (*GetFontSize)();
    bool (*Button)(const char*, ImVec2);
    bool (*CollapsingHeader)(const char*, ImGuiTreeNodeFlags);
} g_ig = {};

template <typename F>
bool Get(HMODULE m, F& fn, const char* name) {
    fn = reinterpret_cast<F>(GetProcAddress(m, name));
    if (!fn) rd::Log("overlay: REFramework doesn't export %s", name);
    return fn != nullptr;
}

// Adapters from ReShade's table signatures to cimgui's.
void TextDisabledV(const char* fmt, va_list a) { g_ig.TextDisabledV(fmt, a); }
void TextColoredV(const ImVec4& c, const char* fmt, va_list a) { g_ig.TextColoredV(c, fmt, a); }
bool Checkbox(const char* l, bool* v) { return g_ig.Checkbox(l, v); }
void SetNextItemWidth(float w) { g_ig.SetNextItemWidth(w); }
void SeparatorText(const char* l) { g_ig.SeparatorText(l); }
bool Combo(const char* l, int* c, const char* const items[], int n, int h) { return g_ig.Combo(l, c, items, n, h); }
void BeginDisabled(bool d) { g_ig.BeginDisabled(d); }
void EndDisabled() { g_ig.EndDisabled(); }
void SameLine(float x, float s) { g_ig.SameLine(x, s); }
bool BeginTabBar(const char* id, ImGuiTabBarFlags f) { return g_ig.BeginTabBar(id, f); }
void EndTabBar() { g_ig.EndTabBar(); }
bool BeginTabItem(const char* l, bool* o, ImGuiTabItemFlags f) { return g_ig.BeginTabItem(l, o, f); }
void EndTabItem() { g_ig.EndTabItem(); }
void TextUnformatted(const char* t, const char* e) { g_ig.TextUnformatted(t, e); }
bool SliderFloat(const char* l, float* v, float a, float b, const char* f, ImGuiSliderFlags fl) { return g_ig.SliderFloat(l, v, a, b, f, fl); }
void Spacing() { g_ig.Spacing(); }
void SetTooltipV(const char* fmt, va_list a) { g_ig.SetTooltipV(fmt, a); }
bool IsItemHovered(ImGuiHoveredFlags f) { return g_ig.IsItemHovered(f); }
bool IsItemDeactivatedAfterEdit() { return g_ig.IsItemDeactivatedAfterEdit(); }
float GetFontSize() { return g_ig.GetFontSize(); }
bool Button(const char* l, const ImVec2& s) { return g_ig.Button(l, s); }

bool Init(HMODULE refw) {
    if (g_tried) return g_ready;
    g_tried = true;
    if (!refw) refw = GetModuleHandleW(L"dinput8.dll");
    if (!refw) return false;
    bool ok = Get(refw, g_ig.GetVersion, "igGetVersion");
    ok &= Get(refw, g_ig.TextDisabledV, "igTextDisabledV");
    ok &= Get(refw, g_ig.TextColoredV, "igTextColoredV");
    ok &= Get(refw, g_ig.Checkbox, "igCheckbox");
    ok &= Get(refw, g_ig.SetNextItemWidth, "igSetNextItemWidth");
    ok &= Get(refw, g_ig.SeparatorText, "igSeparatorText");
    ok &= Get(refw, g_ig.Combo, "igCombo_Str_arr");
    ok &= Get(refw, g_ig.BeginDisabled, "igBeginDisabled");
    ok &= Get(refw, g_ig.EndDisabled, "igEndDisabled");
    ok &= Get(refw, g_ig.SameLine, "igSameLine");
    ok &= Get(refw, g_ig.BeginTabBar, "igBeginTabBar");
    ok &= Get(refw, g_ig.EndTabBar, "igEndTabBar");
    ok &= Get(refw, g_ig.BeginTabItem, "igBeginTabItem");
    ok &= Get(refw, g_ig.EndTabItem, "igEndTabItem");
    ok &= Get(refw, g_ig.TextUnformatted, "igTextUnformatted");
    ok &= Get(refw, g_ig.SliderFloat, "igSliderFloat");
    ok &= Get(refw, g_ig.Spacing, "igSpacing");
    ok &= Get(refw, g_ig.SetTooltipV, "igSetTooltipV");
    ok &= Get(refw, g_ig.IsItemHovered, "igIsItemHovered");
    ok &= Get(refw, g_ig.IsItemDeactivatedAfterEdit, "igIsItemDeactivatedAfterEdit");
    ok &= Get(refw, g_ig.GetFontSize, "igGetFontSize");
    ok &= Get(refw, g_ig.Button, "igButton");
    ok &= Get(refw, g_ig.CollapsingHeader, "igCollapsingHeader_TreeNodeFlags");
    if (!ok) return false;
    g_table.TextDisabledV = &TextDisabledV;
    g_table.TextColoredV = &TextColoredV;
    g_table.Checkbox = &Checkbox;
    g_table.SetNextItemWidth = &SetNextItemWidth;
    g_table.SeparatorText = &SeparatorText;
    g_table.Combo = &Combo;
    g_table.BeginDisabled = &BeginDisabled;
    g_table.EndDisabled = &EndDisabled;
    g_table.SameLine = &SameLine;
    g_table.BeginTabBar = &BeginTabBar;
    g_table.EndTabBar = &EndTabBar;
    g_table.BeginTabItem = &BeginTabItem;
    g_table.EndTabItem = &EndTabItem;
    g_table.TextUnformatted = &TextUnformatted;
    g_table.SliderFloat = &SliderFloat;
    g_table.Spacing = &Spacing;
    g_table.SetTooltipV = &SetTooltipV;
    g_table.IsItemHovered = &IsItemHovered;
    g_table.IsItemDeactivatedAfterEdit = &IsItemDeactivatedAfterEdit;
    g_table.GetFontSize = &GetFontSize;
    g_table.Button = &Button;
    imgui_function_table_instance() = &g_table;
    rd::Log("overlay: in REFramework's menu (its Dear ImGui %s)", g_ig.GetVersion());
    g_ready = true;
    return true;
}

}  // namespace

void rd_LogPlugin(bool pre, bool post, bool ui, int apiMinor) {
    rd::Log("REFramework plugin API 1.%d: input update callbacks %s/%s, menu %s", apiMinor, pre ? "ok" : "failed", post ? "ok" : "failed",
            ui ? "registered" : "unavailable (REFramework too old?)");
}

// REFramework's menu (plugin API on_imgui_draw_ui): REFramework's ImGui frame and window are current.
bool rdx_Blocked = false;

void rdx_OnDrawUi(void* refwModule) {
    if (!Init(static_cast<HMODULE>(refwModule))) return;
    if (!g_ig.CollapsingHeader("RE4DLSS", 0)) return;
    if (rdx_Blocked) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s",
                           "RE4DLSS 1.x is still installed (RE4DLSS.addon64 next to re4.exe, a ReShade add-on).\n"
                           "2.0 replaces it and stays off until it's removed: uninstall 1.x in Fluffy Mod Manager\n"
                           "(or delete RE4DLSS.addon64 and reframework\\plugins\\RE4DLSS_Reflex.dll), then restart the game.");
        return;
    }
    if (rdx::g_overlay) rdx::g_overlay(nullptr);
}
