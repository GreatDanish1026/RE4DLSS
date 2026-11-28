// <product>_Reflex.dll (RD_PRODUCT from version.h): a small REFramework plugin (reframework\plugins) that tells the add-on when the
// game's own frame starts on its game thread: right before the engine's input update (the
// via.ModuleEntry "UpdateHID"), and right after it. The add-on does Reflex's wait there, so it
// happens before the frame's input is read whatever the input device (keyboard and mouse,
// DualSense, Xbox controllers). Latency plan step 2, 2026-11-25.
//
// Only what's needed from REFramework's plugin API (include/reframework/API.h, plugin version
// 1.x) is declared here; the structures' first members are laid out as there.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "version.h"  // RD_PRODUCT

// rdx_overlay.cpp (2.0: this plugin is the whole add-on).
void rdx_OnDrawUi(void* refwModule);
void rd_LogPlugin(bool pre, bool post, bool ui, int apiMinor);

namespace {

struct PluginVersion {
    int major, minor, patch;
    const char* game_name;
};

using EntryCb = void (*)();
using OnEntryFn = bool (*)(const char*, EntryCb);
using LogFn = void (*)(const char*, ...);

struct PluginFunctions {
    void* on_lua_state_created;
    void* on_lua_state_destroyed;
    void* on_present;
    OnEntryFn on_pre_application_entry;
    OnEntryFn on_post_application_entry;
    void* lock_lua;
    void* unlock_lua;
    void* on_device_reset;
    void* on_message;
    LogFn log_error;
    LogFn log_warn;
    LogFn log_info;
    // Plugin API 1.15 (REFramework's include/reframework/API.h).
    void* is_drawing_ui;
    void* create_script_state;
    void* delete_script_state;
    bool (*on_imgui_frame)(void (*)(void*));
    bool (*on_imgui_draw_ui)(void (*)(void*));
};

struct InitializeParam {
    void* reframework_module;
    const PluginVersion* version;
    const PluginFunctions* functions;
};

using Fn = void (*)();
Fn g_before = nullptr, g_after = nullptr;
bool g_looked = false;

// The add-on's entry points (the add-on loads with ReShade, possibly after this plugin).
bool Resolve() {
    if (g_before) return true;
    static DWORD lastTry = 0;
    const DWORD now = GetTickCount();
    if (g_looked && now - lastTry < 1000) return false;  // look again about once a second
    g_looked = true, lastTry = now;
    // 2.0: the add-on is this same DLL (the REFramework plugin).
    HMODULE addon = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Resolve), &addon);
    if (!addon) return false;
    g_before = reinterpret_cast<Fn>(GetProcAddress(addon, RD_PRODUCT "_GameFrameStart"));
    g_after = reinterpret_cast<Fn>(GetProcAddress(addon, RD_PRODUCT "_GameInputRead"));
    return g_before != nullptr;
}

void BeforeUpdateHid() {
    if (Resolve()) g_before();
}

void AfterUpdateHid() {
    if (g_after) g_after();
}

void* g_refw = nullptr;
void DrawUi(void*) { rdx_OnDrawUi(g_refw); }

}  // namespace

extern "C" __declspec(dllexport) void reframework_plugin_required_version(PluginVersion* version) {
    version->major = 1, version->minor = 0, version->patch = 0;
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const InitializeParam* param) {
    if (!param || !param->functions || !param->functions->on_pre_application_entry) return false;
    const bool pre = param->functions->on_pre_application_entry("UpdateHID", &BeforeUpdateHid);
    const bool post = param->functions->on_post_application_entry &&
                      param->functions->on_post_application_entry("UpdateHID", &AfterUpdateHid);
    // 2.0: the settings overlay in REFramework's menu (plugin API 1.15+; older ones lack the field).
    bool ui = false;
    g_refw = param->reframework_module;
    if (param->version && param->version->major == 1 && param->version->minor >= 15 && param->functions->on_imgui_draw_ui)
        ui = param->functions->on_imgui_draw_ui(&DrawUi);
    if (param->functions->log_info)
        param->functions->log_info("[" RD_PRODUCT "] input update callbacks: before %s, after %s; menu %s (plugin API %d.%d)",
                                   pre ? "ok" : "failed", post ? "ok" : "failed", ui ? "ok" : "unavailable",
                                   param->version ? param->version->major : 0, param->version ? param->version->minor : 0);
    rd_LogPlugin(pre, post, ui, param->version ? param->version->minor : -1);
    return pre;
}
