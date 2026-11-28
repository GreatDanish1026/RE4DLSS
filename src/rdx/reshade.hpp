// RE4DLSS 2.0: stands in for ReShade's reshade.hpp. The add-on code keeps using ReShade's API
// types, events and objects (third_party/reshade/include), but they are served by this DLL's own
// D3D12 hook layer (src/rdx) instead of ReShade: events come from hooks on the game's D3D12 calls,
// API objects wrap D3D12 directly. Found before ReShade's header through the include path.
#pragma once

#include "reshade_events.hpp"
#include <charconv>
#include <cstdint>

#define RESHADE_API_VERSION 20

namespace rdx {
// Event registry (rdx_events.cpp).
void RegisterEvent(reshade::addon_event ev, void* callback);
void UnregisterEvent(reshade::addon_event ev, void* callback);
// Settings: RE4DLSS.ini next to the game (migrated once from ReShade.ini's section).
bool GetConfig(const char* section, const char* key, char* value, size_t* size);
void SetConfig(const char* section, const char* key, const char* value);
void LogMessage(int level, const char* message);
void RegisterOverlay(const char* title, void (*callback)(reshade::api::effect_runtime*));
}  // namespace rdx

#if defined(IMGUI_VERSION_NUM)
namespace reshade::internal {
// reshade_overlay.hpp's ImGui calls go through ReShade's function table; the overlay isn't drawn
// through ReShade any more (rdx_overlay.cpp sets the table when there is a UI to draw into).
inline HMODULE get_reshade_module_handle(HMODULE = nullptr) { return nullptr; }
inline HMODULE get_current_module_handle(HMODULE initial = nullptr) { return initial; }
}  // namespace reshade::internal
#include "reshade_overlay.hpp"
#endif

namespace reshade {
namespace log {
enum class level { error = 1, warning = 2, info = 3, debug = 4 };
inline void message(level l, const char* msg) { rdx::LogMessage(static_cast<int>(l), msg); }
}  // namespace log

inline bool get_config_value(api::effect_runtime*, const char* section, const char* key, char* value, size_t* value_size) {
    return rdx::GetConfig(section, key, value, value_size);
}
template <typename T>
inline bool get_config_value(api::effect_runtime* runtime, const char* section, const char* key, T& value) {
    char value_string[32];
    size_t value_length = sizeof(value_string) - 1;
    if (!get_config_value(runtime, section, key, value_string, &value_length)) return false;
    return std::from_chars(value_string, value_string + value_length, value).ec == std::errc{};
}
template <>
inline bool get_config_value<bool>(api::effect_runtime* runtime, const char* section, const char* key, bool& value) {
    int value_int = 0;
    if (!get_config_value<int>(runtime, section, key, value_int)) return false;
    value = value_int != 0;
    return true;
}

inline void set_config_value(api::effect_runtime*, const char* section, const char* key, const char* value) {
    rdx::SetConfig(section, key, value);
}
template <typename T>
inline void set_config_value(api::effect_runtime* runtime, const char* section, const char* key, const T& value) {
    char value_string[32] = "";
    std::to_chars(value_string, value_string + sizeof(value_string) - 1, value);
    set_config_value(runtime, section, key, static_cast<const char*>(value_string));
}
template <>
inline void set_config_value<bool>(api::effect_runtime* runtime, const char* section, const char* key, const bool& value) {
    set_config_value<int>(runtime, section, key, value ? 1 : 0);
}

inline bool register_addon(void*, void* = nullptr) { return true; }
inline void unregister_addon(void*, void* = nullptr) {}

template <addon_event ev>
inline void register_event(typename addon_event_traits<ev>::decl callback) {
    rdx::RegisterEvent(ev, reinterpret_cast<void*>(callback));
}
template <addon_event ev>
inline void unregister_event(typename addon_event_traits<ev>::decl callback) {
    rdx::UnregisterEvent(ev, reinterpret_cast<void*>(callback));
}

inline void register_overlay(const char* title, void (*callback)(api::effect_runtime*)) { rdx::RegisterOverlay(title, callback); }
inline void unregister_overlay(const char*, void (*)(api::effect_runtime*)) {}
}  // namespace reshade
