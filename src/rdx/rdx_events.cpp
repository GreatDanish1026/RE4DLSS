// rdx: the event registry (ReShade's register_event), settings and logging.

#include "rdx.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rd {
void Log(const char* fmt, ...);
}

namespace rdx {

std::atomic<void*> g_events[kMaxEvents][kMaxCallbacks];
std::atomic<bool> g_eventUsed[kMaxEvents];

void RegisterEvent(addon_event ev, void* callback) {
    const int e = static_cast<int>(ev);
    if (e < 0 || e >= kMaxEvents || !callback) return;
    for (auto& slot : g_events[e]) {
        void* expected = nullptr;
        if (slot.compare_exchange_strong(expected, callback)) {
            g_eventUsed[e] = true;
            return;
        }
    }
    Log("too many callbacks for event %d", e);
}

void UnregisterEvent(addon_event ev, void* callback) {
    const int e = static_cast<int>(ev);
    if (e < 0 || e >= kMaxEvents) return;
    for (auto& slot : g_events[e]) {
        void* expected = callback;
        slot.compare_exchange_strong(expected, nullptr);
    }
}

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rd::Log("rdx: %s", buf);
}

void LogMessage(int, const char* message) { rd::Log("%s", message ? message : ""); }

std::wstring ExeDirW() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n < MAX_PATH ? n : 0);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

// ---------------------------------------------------------------- settings

// RE4DLSS.ini next to the game. 1.x kept its settings in ReShade.ini's [RE4DLSS] section: that
// section is copied over the first time (when RE4DLSS.ini doesn't exist yet).
std::mutex g_iniMutex;
std::wstring g_ini;

const std::wstring& Ini() {
    if (!g_ini.empty()) return g_ini;
    const std::wstring dir = ExeDirW();
    g_ini = dir + L"RE4DLSS.ini";
    if (GetFileAttributesW(g_ini.c_str()) != INVALID_FILE_ATTRIBUTES) return g_ini;
    const std::wstring old = dir + L"ReShade.ini";
    std::vector<wchar_t> buf(65536);
    const DWORD n = GetPrivateProfileSectionW(L"RE4DLSS", buf.data(), static_cast<DWORD>(buf.size()), old.c_str());
    if (n > 0) {
        WritePrivateProfileSectionW(L"RE4DLSS", buf.data(), g_ini.c_str());
        Log("settings copied from ReShade.ini's [RE4DLSS] section to RE4DLSS.ini");
    } else {
        // An empty file, so the migration isn't tried again.
        if (FILE* f = _wfopen(g_ini.c_str(), L"w")) fclose(f);
    }
    return g_ini;
}

std::wstring Wide(const char* s) {
    if (!s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

bool GetConfig(const char* section, const char* key, char* value, size_t* size) {
    std::lock_guard<std::mutex> lock(g_iniMutex);
    const std::wstring& ini = Ini();
    static const wchar_t kMissing[] = L"\x01rdx-missing\x01";
    std::vector<wchar_t> buf(4096);
    GetPrivateProfileStringW(Wide(section).c_str(), Wide(key).c_str(), kMissing, buf.data(), static_cast<DWORD>(buf.size()),
                             ini.c_str());
    if (!wcscmp(buf.data(), kMissing)) return false;
    const int n = WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, nullptr, 0, nullptr, nullptr);  // includes the null
    if (!size) return true;
    if (!value) {
        *size = n;
        return true;
    }
    if (*size == 0) return true;
    std::string s(n > 0 ? n : 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, s.data(), n, nullptr, nullptr);
    const size_t len = strlen(s.c_str());
    const size_t copy = len < *size - 1 ? len : *size - 1;
    memcpy(value, s.c_str(), copy);
    value[copy] = '\0';
    *size = copy;  // as ReShade: the length written (without the null)
    return true;
}

void SetConfig(const char* section, const char* key, const char* value) {
    std::lock_guard<std::mutex> lock(g_iniMutex);
    WritePrivateProfileStringW(Wide(section).c_str(), Wide(key).c_str(), value ? Wide(value).c_str() : nullptr, Ini().c_str());
}

// ---------------------------------------------------------------- overlay

void (*g_overlay)(effect_runtime*) = nullptr;
void RegisterOverlay(const char*, void (*callback)(effect_runtime*)) { g_overlay = callback; }

// ---------------------------------------------------------------- private data

void PrivateData::Get(const uint8_t guid[16], uint64_t* data) const {
    std::lock_guard<std::mutex> lock(m_);
    for (const auto& [g, v] : v_)
        if (!memcmp(&g, guid, 16)) {
            *data = v;
            return;
        }
    *data = 0;
}

void PrivateData::Set(const uint8_t guid[16], uint64_t data) {
    std::lock_guard<std::mutex> lock(m_);
    for (auto& [g, v] : v_)
        if (!memcmp(&g, guid, 16)) {
            v = data;
            return;
        }
    GUID g;
    memcpy(&g, guid, 16);
    v_.emplace_back(g, data);
}

}  // namespace rdx
