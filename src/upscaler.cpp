// DLSS, or AMD FSR 3.1 / FSR 4, in place of FSR 2's accumulate pass in Resident Evil 4
// (docs/FINDINGS.md, docs/FSR.md).
//
// The game runs AMD FSR 2.1 as compute passes. Before FSR proper it converts its own inputs:
//   CS_UpscalingDepthTranslation (csd2efdab0): depth -> r32f copy (u0), reverse-Z;
//   CS_UpscalingVelocityTranslate (cs5469503e): velocity -> rg16f (u0): (v - fProjSpaceJitter) *
//     (0.5, -0.5), the UV offset from the current to the previous position, y down;
// and FSR's luminance pyramid (csaa8d773e) reads the render-size colour (t0). The accumulate pass
// (cs226d3330) writes the output-size image (u0) that RCAS (cs1cfde08a) then sharpens into the
// image the game's post-processing reads. So DLSS replaces accumulate: same output texture, its
// inputs taken from the passes above in the same command list, and FSR's cbFSR2 (root CBV b0)
// gives the sizes, the jitter (fJitter, render pixels) and the frame index (0 after a reset).
// RCAS still runs, with the game's sharpness setting. The render size is the game's FSR 2 quality.
// FSR 3.1/4 (AMD's FidelityFX runtime) takes the same inputs plus FSR 2's reactive mask
// (cs1e379886 u0) and the camera from cbFSR2.
//
// Frame generation (framegen.cpp) gets the same depth and motion vectors at that pass, whoever
// upscales, and the camera from RE Engine's SceneInfo constant buffer: the root constant buffer
// whose screenSize (byte 368) is the render size, seen in the frame before FSR 2 runs.
//
// The picture before the HUD (for frame generation): the game copies its finished scene
// (ps792d66db's target) into the texture it draws the HUD into, then its final pass (ps9d1f6421,
// RenoDX's HDR output when installed) writes that texture to the back buffer. Right after that
// pass, the scene is copied into the HUD texture again and the same draw is repeated, with all the
// game's (and RenoDX's) state, into a render target of ours: the screen without the HUD.
//
// NGX records its own compute state and descriptor heaps on the game's command list, so the
// game's bindings are tracked per command list and put back afterwards (as in VillageDLSS).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>

#include "common.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../third_party/reshade/utils/descriptor_tracking.hpp"
#include "dlss.hpp"
#include "fsr.hpp"
#include "guide_cs.h"       // kGuideCs: guide.hlsl (Ray Reconstruction guides from the G-buffer)
#include "guide_view_cs.h"  // kGuideViewCs: guide_view.hlsl (guide debug view)
#include "version.h"

using namespace reshade::api;

namespace rd {
namespace {

constexpr uint32_t kReactiveCs = 0x1e379886;    // FSR 2 reactive mask (u0 r8), before the others
constexpr uint32_t kDepthCs = 0xd2efdab0;       // CS_UpscalingDepthTranslation
constexpr uint32_t kVelocityCs = 0x5469503e;    // CS_UpscalingVelocityTranslate
constexpr uint32_t kLumaCs = 0xaa8d773e;        // FSR 2 luminance pyramid (t0 = render-size colour)
constexpr uint32_t kAccumulateCs = 0x226d3330;  // FSR 2 accumulate (u0 = output-size image)
constexpr uint32_t kFinalPs = 0x9d1f6421;       // the scene + HUD texture -> back buffer
constexpr int kToggleHotkey = VK_F9;

// cbFSR2 (FSR 2.1), as 32-bit words.
constexpr int kRenderSize = 0;    // int2
constexpr int kDisplaySize = 2;   // int2
constexpr int kFrameIndex = 7;    // uint
constexpr int kJitter = 10;       // float2, render pixels
constexpr int kDeviceToViewDepth = 16;  // float4 (FSR 2's depth -> view depth from near/far)
constexpr int kPreExposure = 28;  // float
constexpr int kTanHalfFov = 29;   // float
constexpr int kCbWords = 36;      // 144 bytes

// ---------------------------------------------------------------- settings

enum class Tech { Auto = 0, Dlss = 1, Fsr = 2 };

struct Settings {
    bool enabled = true;  // F9; remembered across launches
    int upscaler = 0;     // Tech: Auto = DLSS where NGX runs (RTX), else FSR
    bool fsrReactive = true;  // give FSR 3/4 the game's FSR 2 reactive mask
    int preset = dlss::kPresetAuto;  // DLSS model: Automatic, or NVSDK_NGX_DLSS_Hint_Render_Preset_* (0 = NVIDIA's)
    bool flipJitterX = false, flipJitterY = false;  // testing: jitter sign
    int resolution = 0;   // kResolutions: 0 = the game's FSR 2 setting (RE4DLSS.lua sets the others)
    // DLSS Ray Reconstruction in place of Super Resolution while the game ray traces (ini
    // RayReconstruction; experimental, off by default). rrPreset: NVSDK_NGX_RayReconstruction_Hint_Render_Preset_*
    // (ini RrModel, 0 = NVIDIA's default). rrDenoise (ini RrDenoise): which of the game's ray tracing
    // denoise passes are skipped: 0 none, 1 all but the spatial one, 2 all.
    // guideView: debug view of one guide (not saved).
    bool rr = false;
    int rrPreset = 0;
    int rrDenoise = 0;
    int guideView = 0;
} g_set;

// Render resolutions offered: the engine's FSR 2 quality (via.render.UpscalingInterface
// UpscalingQuality, set by reframework/autorun/RE4DLSS.lua): 0 native, 1 Quality, 2 Balanced,
// 3 Performance, 4 Ultra Performance. -1: the game's own setting.
struct Resolution {
    const char* name;
    int quality;
};
const Resolution kResolutions[] = {{"Game's FSR 2 setting", -1},          {"Native (100%): DLAA / FSR native AA", 0},
                                   {"Quality (67%)", 1},                  {"Balanced (59%)", 2},
                                   {"Performance (50%)", 3},              {"Ultra Performance (33%)", 4}};
constexpr int kResolutionCount = sizeof(kResolutions) / sizeof(kResolutions[0]);

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::get_config_value(nullptr, RD_PRODUCT, "RenderResolution", g_set.resolution);
    if (g_set.resolution < 0 || g_set.resolution >= kResolutionCount) g_set.resolution = 0;
    reshade::get_config_value(nullptr, RD_PRODUCT, "DlssModel", g_set.preset);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Upscaler", g_set.upscaler);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FsrReactiveMask", g_set.fsrReactive);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
    reshade::get_config_value(nullptr, RD_PRODUCT, "RayReconstruction", g_set.rr);
    reshade::get_config_value(nullptr, RD_PRODUCT, "RrModel", g_set.rrPreset);
    if (g_set.rrPreset != 0 && (g_set.rrPreset < 4 || g_set.rrPreset > 6)) g_set.rrPreset = 0;
    reshade::get_config_value(nullptr, RD_PRODUCT, "RrDenoise", g_set.rrDenoise);
    if (g_set.rrDenoise < 0 || g_set.rrDenoise > 2) g_set.rrDenoise = 0;
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "RenderResolution", g_set.resolution);
    reshade::set_config_value(nullptr, RD_PRODUCT, "DlssModel", g_set.preset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Upscaler", g_set.upscaler);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FsrReactiveMask", g_set.fsrReactive);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
    reshade::set_config_value(nullptr, RD_PRODUCT, "RayReconstruction", g_set.rr);
    reshade::set_config_value(nullptr, RD_PRODUCT, "RrModel", g_set.rrPreset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "RrDenoise", g_set.rrDenoise);
}

std::mutex g_statusMutex;
std::string g_lastError, g_lastLogged, g_mode;
Tech g_active = Tech::Auto;  // the backend that ran last
std::atomic<uint64_t> g_presents{0}, g_lastRun{0};  // present count, and when an upscaler last ran
const char* TechName(int t) { return t == 1 ? "DLSS" : t == 2 ? "FSR 3.1 / FSR 4" : "Auto"; }
uint64_t g_replaced = 0;

void Fail(const std::string& why) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (why != g_lastLogged) Log("Upscaler: %s", why.c_str());
    g_lastLogged = why;
    g_lastError = why;
}

// ---------------------------------------------------------------- per command list bindings

struct StageState {
    pipeline_layout layout = {0};
    std::vector<descriptor_table> tables;
    struct Root {
        shader_stage stages;
        descriptor_type type;
        buffer_range range;
    };
    std::map<uint32_t, Root> roots;
    struct Constants {
        shader_stage stages;
        std::vector<uint32_t> values;
    };
    std::map<uint32_t, Constants> constants;

    void Layout(pipeline_layout l) {
        if (l == layout) return;
        layout = l;  // a new root signature resets every binding
        tables.clear();
        roots.clear();
        constants.clear();
    }
};

// FSR's inputs, gathered from the passes before accumulate in the same command list.
struct Chain {
    resource depth = {0}, motion = {0}, color = {0}, reactive = {0};
};

struct __declspec(uuid("4c2a9e17-6b3d-4f81-a5c0-9d2e7b1f3a64")) ListState {
    pipeline pipe = {0};
    pipeline_stage pipeStages = pipeline_stage::all;
    StageState compute, graphics;
    Chain chain;
    // Frame generation: the last SceneInfo of the render size bound on this list.
    float scene[kSceneInfoBytes / 4];
    bool sceneValid = false;
    buffer_range checked[8] = {};  // constant buffers looked at on this list (most are bound many times)
    uint32_t checkedNext = 0;
    // The picture before the HUD: the render target bound, the last copy between two textures of
    // the same size and format (scene -> HUD texture) with their states since, and the final pass.
    resource_view rtv = {0};
    resource copySrc = {0}, copyDst = {0};
    resource_usage srcState = resource_usage::undefined, dstState = resource_usage::undefined;
    bool hudPending = false;
    uint32_t hudVertices = 0, hudInstances = 0;
};

// SceneInfo search: the render size it must have (from the last cbFSR2), and the last one found on
// any list (when the scene is drawn on another list than FSR 2's passes).
std::atomic<uint32_t> g_sceneW{0}, g_sceneH{0};
std::mutex g_sceneMutex;
float g_lastScene[kSceneInfoBytes / 4];
bool g_lastSceneValid = false;
uint64_t g_scenesFound = 0;
constexpr uint32_t kScreenSizeFloat = 92;  // byte 368

ListState* State(command_list* cl) { return cl->get_private_data<ListState>(); }

StageState* Stage(ListState* s, shader_stage stages) {
    if ((stages & shader_stage::all_compute) != 0) return &s->compute;
    if ((stages & shader_stage::all_graphics) != 0) return &s->graphics;
    return nullptr;
}

void OnInitList(command_list* cl) { cl->create_private_data<ListState>(); }

// ---------------------------------------------------------------- reading constant buffers
//
// Constant buffers are read on the CPU when the pass is recorded. Usually they're in upload memory
// and can be mapped. Some systems (seen on a Radeon with FSR 4) keep them in GPU-only memory,
// filled by a copy from upload memory: once that's seen, buffer copies are noted, and such a
// buffer is read from the upload buffer it was copied from.

std::atomic<bool> g_trackCopies{false};
struct CopySource {
    uint64_t size;
    resource src;
    uint64_t srcOffset;
};
std::mutex g_copyMutex;
std::map<uint64_t, std::map<uint64_t, CopySource>> g_copies;  // destination buffer -> its offset -> source
std::map<uint64_t, bool> g_copySources;                        // the source buffers seen
size_t g_copyCount = 0;

void NoteCopy(resource src, uint64_t srcOffset, resource dst, uint64_t dstOffset, uint64_t size) {
    std::lock_guard<std::mutex> lock(g_copyMutex);
    if (g_copyCount > 65536) {
        g_copies.clear();
        g_copySources.clear();
        g_copyCount = 0;
    }
    if (g_copies[dst.handle].insert_or_assign(dstOffset, CopySource{size, src, srcOffset}).second) ++g_copyCount;
    g_copySources[src.handle] = true;
}

bool OnCopyBuffer(command_list*, resource src, uint64_t srcOffset, resource dst, uint64_t dstOffset, uint64_t size) {
    if (g_trackCopies.load(std::memory_order_relaxed)) NoteCopy(src, srcOffset, dst, dstOffset, size);
    return false;
}

void RtOnBarrier(uint32_t count, const resource* res, const resource_usage* after);
void RtOnDestroyResource(device* dev, resource r);
void RtDestroy(device* dev);

// A destroyed buffer mustn't be mapped later: forget copies into it, and all of them if it was a source.
// (Also Ray Reconstruction's record of the game's ray tracing textures.)
void OnDestroyBuffer(device* dev, resource r) {
    RtOnDestroyResource(dev, r);
    if (!g_trackCopies.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_copyMutex);
    if (g_copySources.count(r.handle)) {
        g_copies.clear();
        g_copySources.clear();
        g_copyCount = 0;
    } else if (auto it = g_copies.find(r.handle); it != g_copies.end()) {
        g_copyCount -= it->second.size();
        g_copies.erase(it);
    }
}

bool CopySourceOf(const buffer_range& r, uint64_t bytes, resource* src, uint64_t* srcOffset) {
    std::lock_guard<std::mutex> lock(g_copyMutex);
    const auto b = g_copies.find(r.buffer.handle);
    if (b == g_copies.end()) return false;
    auto it = b->second.upper_bound(r.offset);
    if (it == b->second.begin()) return false;
    --it;
    const uint64_t end = it->second.size == UINT64_MAX ? UINT64_MAX : it->first + it->second.size;
    if (r.offset + bytes > end) return false;
    *src = it->second.src;
    *srcOffset = it->second.srcOffset + (r.offset - it->first);
    return true;
}

// Reads `bytes` of a buffer: mapped when the CPU can, else through the copy that filled it.
// *how says which (or why neither worked).
bool ReadBuffer(device* dev, const buffer_range& r, uint32_t bytes, void* out, std::string* how) {
    const resource_desc d = dev->get_resource_desc(r.buffer);
    char msg[200];
    void* data = nullptr;
    if (d.heap != memory_heap::default_ && dev->map_buffer_region(r.buffer, r.offset, bytes, map_access::read_only, &data) && data) {
        memcpy(out, data, bytes);
        dev->unmap_buffer_region(r.buffer);
        if (how) {
            snprintf(msg, sizeof(msg), "mapped (memory heap %d)", static_cast<int>(d.heap));
            *how = msg;
        }
        return true;
    }
    g_trackCopies = true;
    resource src = {0};
    uint64_t srcOffset = 0;
    if (CopySourceOf(r, bytes, &src, &srcOffset)) {
        const resource_desc sd = dev->get_resource_desc(src);
        data = nullptr;
        if (sd.heap != memory_heap::default_ && dev->map_buffer_region(src, srcOffset, bytes, map_access::read_only, &data) && data) {
            memcpy(out, data, bytes);
            dev->unmap_buffer_region(src);
            if (how) {
                snprintf(msg, sizeof(msg), "memory heap %d, read from the buffer copied into it (heap %d)",
                         static_cast<int>(d.heap), static_cast<int>(sd.heap));
                *how = msg;
            }
            return true;
        }
        if (how) {
            snprintf(msg, sizeof(msg), "memory heap %d can't be read, nor the buffer copied into it (heap %d)",
                     static_cast<int>(d.heap), static_cast<int>(sd.heap));
            *how = msg;
        }
        return false;
    }
    if (how) {
        snprintf(msg, sizeof(msg), "memory heap %d (buffer size %llu) can't be read, and no copy into it was seen",
                 static_cast<int>(d.heap), static_cast<unsigned long long>(d.buffer.size));
        *how = msg;
    }
    return false;
}

void OnBindRts(command_list* cl, uint32_t count, const resource_view* rtvs, resource_view) {
    if (ListState* s = State(cl)) s->rtv = count ? rtvs[0] : resource_view{0};
}

bool OnCopy(command_list* cl, resource src, resource dst) {
    ListState* s = State(cl);
    if (!s) return false;
    device* dev = cl->get_device();
    const resource_desc a = dev->get_resource_desc(src), b = dev->get_resource_desc(dst);
    if (a.type == resource_type::buffer && b.type == resource_type::buffer) {
        if (g_trackCopies.load(std::memory_order_relaxed)) NoteCopy(src, 0, dst, 0, b.buffer.size);
        return false;
    }
    if (a.type == resource_type::texture_2d && b.type == resource_type::texture_2d && a.texture.width == b.texture.width &&
        a.texture.height == b.texture.height && a.texture.format == b.texture.format &&
        a.texture.format == format::r16g16b16a16_float) {
        s->copySrc = src, s->copyDst = dst;
        s->srcState = resource_usage::copy_source, s->dstState = resource_usage::copy_dest;
    }
    return false;
}

void DrawHudless(command_list* cl, ListState& s);

void OnBarrier(command_list* cl, uint32_t count, const resource* res, const resource_usage* before, const resource_usage* after) {
    ListState* s = State(cl);
    if (!s) return;
    if (s->hudPending) {  // the game's final pass is recorded: ours goes right after it
        s->hudPending = false;
        DrawHudless(cl, *s);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (res[i] == s->copySrc) s->srcState = after[i];
        if (res[i] == s->copyDst) s->dstState = after[i];
    }
    RtOnBarrier(count, res, after);
    (void)before;
}

// Is this constant buffer SceneInfo (screenSize = the render size, 1 / screenSize after it)? Keeps
// a copy on the list if so. Each buffer range is looked at once per list.
void CheckSceneInfo(command_list* cl, ListState& s, const buffer_range& r) {
    const uint32_t w = g_sceneW.load(), h = g_sceneH.load();
    if (!w || !r.buffer.handle) return;
    for (const buffer_range& c : s.checked)
        if (c.buffer.handle == r.buffer.handle && c.offset == r.offset) return;
    s.checked[s.checkedNext++ % 8] = r;
    device* dev = cl->get_device();
    const resource_desc d = dev->get_resource_desc(r.buffer);
    // SceneInfo is written by the CPU every frame: upload memory, or GPU-only memory filled by a copy
    // (read through it, once such buffers were seen).
    if (d.type != resource_type::buffer || r.offset + kSceneInfoBytes > d.buffer.size) return;
    if (d.heap == memory_heap::default_ && !g_trackCopies.load(std::memory_order_relaxed)) return;
    float f[kSceneInfoBytes / 4];
    if (!ReadBuffer(dev, r, kSceneInfoBytes, f, nullptr)) return;
    // Right size, a view-projection matrix, unit view axes (transposeViewInvMat, floats 28-39) and
    // a near plane: another buffer with the same screen size but zero matrices exists too.
    const auto axis = [&](int c) { return f[28 + c] * f[28 + c] + f[32 + c] * f[32 + c] + f[36 + c] * f[36 + c]; };
    const bool match = f[kScreenSizeFloat] == static_cast<float>(w) && f[kScreenSizeFloat + 1] == static_cast<float>(h) &&
                       std::fabs(f[kScreenSizeFloat + 2] * w - 1) < 1e-3f && std::fabs(f[kScreenSizeFloat + 3] * h - 1) < 1e-3f &&
                       std::fabs(f[0]) + std::fabs(f[5]) > 1e-3f && std::fabs(axis(0) - 1) < 1e-2f &&
                       std::fabs(axis(1) - 1) < 1e-2f && f[98] > 0 &&
                       // projection x scale (view x axis . viewProj column 0) positive: not the mirrored
                       // second view the game also draws at this size
                       f[28] * f[0] + f[32] * f[4] + f[36] * f[8] > 0.05f;
    if (!match) return;
    memcpy(s.scene, f, kSceneInfoBytes);
    s.sceneValid = true;
    std::lock_guard<std::mutex> lock(g_sceneMutex);
    memcpy(g_lastScene, s.scene, kSceneInfoBytes);
    g_lastSceneValid = true;
    if (++g_scenesFound == 1) Log("frame generation: SceneInfo found (%ux%u, buffer heap %d)", w, h, static_cast<int>(d.heap));
}
void OnDestroyList(command_list* cl) { cl->destroy_private_data<ListState>(); }

void OnResetList(command_list* cl) {
    if (ListState* s = State(cl)) *s = ListState();
}

void OnBindPipeline(command_list* cl, pipeline_stage stages, pipeline pipe) {
    ListState* s = State(cl);
    if (!s) return;
    s->pipe = pipe;
    s->pipeStages = stages;
}

void OnBindTables(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
                  const descriptor_table* tables, uint32_t, const uint32_t*) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    if (st->tables.size() < first + count) st->tables.resize(first + count, descriptor_table{0});
    for (uint32_t i = 0; i < count; ++i) st->tables[first + i] = tables[i];
}

void OnPushDescriptors(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t param,
                       const descriptor_table_update& u) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    if (u.count != 1 || u.table.handle) return;
    if (u.type == descriptor_type::constant_buffer || u.type == descriptor_type::buffer_shader_resource_view ||
        u.type == descriptor_type::buffer_unordered_access_view)
        st->roots[param] = {stages, u.type, *static_cast<const buffer_range*>(u.descriptors)};
    if (u.type == descriptor_type::constant_buffer && FrameGenWantsCamera())
        CheckSceneInfo(cl, *s, *static_cast<const buffer_range*>(u.descriptors));
}

void OnPushConstants(command_list* cl, shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first,
                     uint32_t count, const void* values) {
    ListState* s = State(cl);
    if (!s) return;
    StageState* st = Stage(s, stages);
    if (!st) return;
    st->Layout(layout);
    StageState::Constants& c = st->constants[param];
    c.stages = stages;
    if (c.values.size() < first + count) c.values.resize(first + count);
    memcpy(c.values.data() + first, values, count * 4);
}

// Puts the game's bindings back after DLSS. A bind with no tables makes ReShade set the game's
// descriptor heaps and root signature again (NGX changed both behind its back).
void RestoreStage(command_list* cl, const StageState& st, shader_stage stages) {
    if (!st.layout.handle) return;
    cl->bind_descriptor_tables(stages, st.layout, 0, 0, nullptr);
    for (uint32_t i = 0; i < st.tables.size(); ++i)
        if (st.tables[i].handle) cl->bind_descriptor_tables(stages, st.layout, i, 1, &st.tables[i]);
    for (const auto& [param, r] : st.roots) {
        descriptor_table_update u = {};
        u.count = 1;
        u.type = r.type;
        u.descriptors = &r.range;
        cl->push_descriptors(r.stages, st.layout, param, u);
    }
    for (const auto& [param, c] : st.constants)
        cl->push_constants(c.stages, st.layout, param, 0, static_cast<uint32_t>(c.values.size()), c.values.data());
}

void Restore(command_list* cl, const ListState& s) {
    RestoreStage(cl, s.graphics, shader_stage::all_graphics);
    RestoreStage(cl, s.compute, shader_stage::all_compute);  // last: the game's next pass is compute
    if (s.pipe.handle) cl->bind_pipeline(s.pipeStages, s.pipe);
}

// ---------------------------------------------------------------- game resources

bool IsUav(descriptor_type t) {
    return t == descriptor_type::unordered_access_view || t == descriptor_type::texture_unordered_access_view ||
           t == descriptor_type::buffer_unordered_access_view;
}

// The texture at compute shader register t<reg> (uav false) or u<reg> (uav true), space0.
resource ComputeView(device* dev, const StageState& st, uint32_t reg, bool uav) {
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) return {0};
    for (uint32_t i = 0; i < st.tables.size(); ++i) {
        if (!st.tables[i].handle) continue;
        const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, i);
        if (p.type != pipeline_layout_param_type::descriptor_table || !p.descriptor_table.ranges) continue;
        for (uint32_t r = 0; r < p.descriptor_table.count; ++r) {
            const descriptor_range& range = p.descriptor_table.ranges[r];
            if (range.type == descriptor_type::sampler || range.type == descriptor_type::constant_buffer) continue;
            if (IsUav(range.type) != uav || range.dx_register_space != 0) continue;
            if (reg < range.dx_register_index || reg - range.dx_register_index >= range.count) continue;
            descriptor_heap heap = {0};
            uint32_t offset = 0;
            dev->get_descriptor_heap_offset(st.tables[i], range.binding + (reg - range.dx_register_index), 0, &heap,
                                            &offset);
            return ViewResource(dt->get_resource_view(heap, offset));
        }
    }
    return {0};
}

// cbFSR2: compute b0 space0. In RE4 a root CBV; a descriptor table entry or root constants are
// read too. *how says where it came from, or why it couldn't be read.
// The compute stage's b0 space0 constant buffer, <bytes> of it (a root CBV, a descriptor table
// entry or root constants).
bool ReadCb0(device* dev, const StageState& st, uint32_t bytes, void* out, std::string* how) {
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) {
        *how = "descriptor tracking unavailable";
        return false;
    }
    std::string sub;
    for (const auto& [param, r] : st.roots) {
        if (r.type != descriptor_type::constant_buffer) continue;
        const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, param);
        if (p.type != pipeline_layout_param_type::push_descriptors || p.push_descriptors.dx_register_index != 0 ||
            p.push_descriptors.dx_register_space != 0)
            continue;
        const bool ok = ReadBuffer(dev, r.range, bytes, out, &sub);
        *how = "root CBV, " + sub;
        return ok;
    }
    for (uint32_t i = 0; i < st.tables.size(); ++i) {
        if (!st.tables[i].handle) continue;
        const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, i);
        if (p.type != pipeline_layout_param_type::descriptor_table || !p.descriptor_table.ranges) continue;
        for (uint32_t k = 0; k < p.descriptor_table.count; ++k) {
            const descriptor_range& range = p.descriptor_table.ranges[k];
            if (range.type != descriptor_type::constant_buffer || range.dx_register_space != 0 || range.dx_register_index != 0)
                continue;
            descriptor_heap heap = {0};
            uint32_t offset = 0;
            dev->get_descriptor_heap_offset(st.tables[i], range.binding, 0, &heap, &offset);
            const buffer_range br = dt->get_buffer_range(heap, offset);
            if (!br.buffer.handle) {
                *how = "descriptor table CBV, buffer unknown";
                return false;
            }
            const bool ok = ReadBuffer(dev, br, bytes, out, &sub);
            *how = "descriptor table CBV, " + sub;
            return ok;
        }
    }
    for (const auto& [param, c] : st.constants) {
        const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, param);
        if (p.type != pipeline_layout_param_type::push_constants || p.push_constants.dx_register_index != 0 ||
            p.push_constants.dx_register_space != 0 || c.values.size() * 4 < bytes)
            continue;
        memcpy(out, c.values.data(), bytes);
        *how = "root constants";
        return true;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "no b0 constant buffer bound (%zu root descriptors, %zu tables, %zu root constants)",
             st.roots.size(), st.tables.size(), st.constants.size());
    *how = msg;
    return false;
}

bool ReadFsrConstants(device* dev, const StageState& st, uint32_t* out, std::string* how) {
    return ReadCb0(dev, st, kCbWords * 4, out, how);
}

// ---------------------------------------------------------------- Ray Reconstruction
//
// RE4 ray traces its reflections in compute shaders at a lower size (1920x1080 at 2560x1440) and
// denoises them itself (docs/FINDINGS.md "Ray tracing"); rayTracingUpscale then adds them onto the
// scene colour at the render size, before FSR 2. For Ray Reconstruction (from RE2DLSS/RE7DLSS)
// the game's denoise passes can be skipped and the signal from before them copied into the
// texture rayTracingUpscale reads. At FSR 2's accumulate, guides are made from the G-buffer that
// rayTracingUpscale reads, and DLSS-D runs in place of DLSS.

// The game's ray tracing passes: the reflections-only form of RE2/RE3/RE7's chain (one specular
// signal), matched by structure from docs/captures/RE4DLSS-frame-1.txt (RE7 id in brackets).
constexpr uint32_t kRtShadeDiffuse = 8;              // (no ray-traced diffuse in RE4)
constexpr uint32_t kRtShadeSpecular = 0x3dd76aeb;    // rayTracingShading: u0 specular (ec2602f1)
constexpr uint32_t kRtShadeVfx = 4;                  // (RE2 only)
constexpr uint32_t kRtSpatialDiffuse = 9;            // (no ray-traced diffuse in RE4)
constexpr uint32_t kRtSpatialSpecular = 0x567f11a3;  // rayTracingSpatialAccumulation: u0 specular (2712f710)
constexpr uint32_t kRtSpatialVfx = 5;                // (RE2 only)
constexpr uint32_t kRtVfxMedian = 6;                 // (RE2 only)
constexpr uint32_t kRtMeanVariance = 0x200f44e3;     // rayTracingMeanVariance (81dbf3d7)
constexpr uint32_t kRtTemporalSpecular = 0xfc8ea1ae;  // rayTracingTemporalAccumulation, specular (38fb0c41)
constexpr uint32_t kRtTemporalDiffuse = 10;           // (no ray-traced diffuse in RE4)
constexpr uint32_t kRtDeflicker = 0xe1d94131;         // rayTracingDeflickerSpecular (387ae12e)
constexpr uint32_t kRtBilateral = 0xb38899cf;         // rayTracingBilateralFilter (3f14ab68)
constexpr uint32_t kRtTemporalVfx = 7;                // (RE2 only)
constexpr uint32_t kRtUpscale = 0x4c66de88;           // rayTracingUpscale: t3 base colour, t4 normal, t7 specular (ba99f22d)
constexpr uint32_t kUpBase = 3, kUpNormal = 4, kUpSignal = 7;  // t7 specular
constexpr uint32_t kRtSignals = 1;
constexpr int kSpecularSlot = 0;  // signal[] index of the specular signal
constexpr int kViewProj = 0;       // SceneInfo: row_major viewProjMat, byte 0

const char* const kRrDenoiseNames[] = {"Game's denoiser, Ray Reconstruction on top (recommended)",
                                       "Skip all but the spatial passes (experimental)", "Skip all: raw rays (experimental)"};
const char* const kGuideViewNames[] = {"Off", "Diffuse albedo", "Specular albedo", "Normals", "Roughness"};
constexpr int kGuideViewCount = 5;

std::mutex g_rtMutex;  // everything in this section
struct RtState {
    uint64_t signalFrame = UINT64_MAX;  // present count when the signals below were seen
    resource signal[4] = {};            // shY, cocg, specular, VFX specular: what replaces the denoised ones
    uint64_t upscaleFrame = UINT64_MAX;  // present count when rayTracingUpscale ran
    resource base = {0}, normal = {0};   // its G-buffer
    float scene[kSceneInfoBytes / 4];    // its camera (SceneInfo, b0), read once a frame there
    uint64_t sceneFrame = UINT64_MAX;    // present count when that was read
    std::string sceneHow;                // why it couldn't be read, if it couldn't
    uint64_t rrFrame = UINT64_MAX;       // present count when Ray Reconstruction last ran
    uint64_t rrFrames = 0, skipped = 0, copied = 0;
    std::string error;                   // why Ray Reconstruction isn't running (overlay)
    std::map<uint64_t, resource_usage> states;  // last recorded state of the textures above
    std::map<std::pair<uint64_t, uint32_t>, resource_view> views;  // ours, by resource and kind/format
    // Guides (render size), ours.
    resource guide[3] = {};
    resource_view guideUav[3] = {};
    uint32_t guideW = 0, guideH = 0;
    pipeline_layout layout = {0}, viewLayout = {0};
    pipeline pipe = {0}, viewPipe = {0};
    bool pipesFailed = false;
    bool loggedMatrices = false;
} g_rt;

void RtWatch(resource r) {
    if (r.handle && !g_rt.states.count(r.handle)) g_rt.states[r.handle] = resource_usage::undefined;
}

void RtOnBarrier(uint32_t count, const resource* res, const resource_usage* after) {
    if (!g_set.rr || !g_set.enabled) return;  // every barrier the game records comes here
    std::lock_guard<std::mutex> lock(g_rtMutex);
    if (g_rt.states.empty()) return;
    for (uint32_t i = 0; i < count; ++i) {
        auto it = g_rt.states.find(res[i].handle);
        if (it != g_rt.states.end()) it->second = after[i];
    }
}

void RtOnDestroyResource(device* dev, resource r) {
    std::lock_guard<std::mutex> lock(g_rtMutex);
    g_rt.states.erase(r.handle);
    for (auto it = g_rt.views.begin(); it != g_rt.views.end();) {
        if (it->first.first == r.handle) {
            dev->destroy_resource_view(it->second);
            it = g_rt.views.erase(it);
        } else {
            ++it;
        }
    }
    for (resource& s : g_rt.signal)
        if (s == r) s = {0};
    if (g_rt.base == r) g_rt.base = {0};
    if (g_rt.normal == r) g_rt.normal = {0};
}

// A view of ours on <r> (cached until the resource goes).
resource_view RtView(device* dev, resource r, format f, bool uav) {
    const auto key = std::make_pair(r.handle, (static_cast<uint32_t>(f) << 1) | (uav ? 1u : 0u));
    auto it = g_rt.views.find(key);
    if (it != g_rt.views.end()) return it->second;
    resource_view v = {0};
    if (!dev->create_resource_view(r, uav ? resource_usage::unordered_access : resource_usage::shader_resource,
                                   resource_view_desc(f), &v))
        return {0};
    g_rt.views[key] = v;
    return v;
}

// Ray Reconstruction is chosen and can run with the upscaler in use.
bool RrChosen() {
    std::string why;
    return g_set.enabled && g_set.rr && g_active == Tech::Dlss && dlss::RrAvailable(&why);
}

// The game's denoise passes are skipped while Ray Reconstruction ran last frame (so the first frame
// with it, and every frame after it stops, keeps the game's).
bool RtSkipping(uint64_t frame) {
    return RrChosen() && g_set.rrDenoise > 0 && g_rt.rrFrame != UINT64_MAX && g_rt.rrFrame + 2 >= frame;
}

bool RtIsSkipped(uint32_t id) {
    switch (id) {
        case kRtMeanVariance: case kRtTemporalSpecular: case kRtTemporalDiffuse: case kRtDeflicker: case kRtBilateral:
        case kRtTemporalVfx:
            return true;
        case kRtSpatialDiffuse: case kRtSpatialSpecular: case kRtSpatialVfx: case kRtVfxMedian:
            return g_set.rrDenoise == 2;
        default:
            return false;
    }
}

// Copies <src> over <dst> when they match, in their recorded states (left as they were).
bool RtCopy(command_list* cl, resource src, resource dst) {
    device* dev = cl->get_device();
    if (!src.handle || !dst.handle || src == dst) return false;
    const resource_desc sd = dev->get_resource_desc(src), dd = dev->get_resource_desc(dst);
    if (sd.texture.width != dd.texture.width || sd.texture.height != dd.texture.height ||
        sd.texture.format != dd.texture.format) {
        static int logged = 0;
        if (logged++ < 4)
            Log("Ray Reconstruction: signal %ux%u format %u doesn't match the game's %ux%u format %u", sd.texture.width,
                sd.texture.height, static_cast<unsigned>(sd.texture.format), dd.texture.width, dd.texture.height,
                static_cast<unsigned>(dd.texture.format));
        return false;
    }
    auto si = g_rt.states.find(src.handle), di = g_rt.states.find(dst.handle);
    const resource_usage ss = si != g_rt.states.end() ? si->second : resource_usage::undefined;
    // rayTracingUpscale is about to read dst: an SRV state, non-pixel if nothing better is known.
    resource_usage ds = di != g_rt.states.end() ? di->second : resource_usage::undefined;
    if (ds == resource_usage::undefined) ds = resource_usage::shader_resource_non_pixel;
    if (ss == resource_usage::undefined) return false;  // not seen in a barrier yet: next frame
    cl->barrier(src, ss, resource_usage::copy_source);
    cl->barrier(dst, ds, resource_usage::copy_dest);
    cl->copy_resource(src, dst);
    cl->barrier(src, resource_usage::copy_source, ss);
    cl->barrier(dst, resource_usage::copy_dest, ds);
    return true;
}

// Every compute dispatch: notes the signals and the G-buffer, skips the game's denoise passes, and
// copies the signals in before rayTracingUpscale. True: the dispatch is skipped.
bool RtOnDispatch(command_list* cl, ListState& s, uint32_t id) {
    if (!g_set.rr || !g_set.enabled) return false;
    device* dev = cl->get_device();
    const uint64_t frame = g_presents.load();
    std::lock_guard<std::mutex> lock(g_rtMutex);
    const bool raw = g_set.rrDenoise == 2;
    auto note = [&](int slot, resource r) {
        if (g_rt.signalFrame != frame) {
            for (resource& x : g_rt.signal) x = {0};
            g_rt.signalFrame = frame;
        }
        g_rt.signal[slot] = r;
        RtWatch(r);
    };
    switch (id) {
        case kRtShadeDiffuse: if (raw) note(0, ComputeView(dev, s.compute, 0, true)), note(1, ComputeView(dev, s.compute, 1, true)); break;
        case kRtShadeSpecular: if (raw) note(kSpecularSlot, ComputeView(dev, s.compute, 0, true)); break;
        case kRtShadeVfx: if (raw) note(3, ComputeView(dev, s.compute, 0, true)); break;
        case kRtSpatialDiffuse: if (!raw) note(0, ComputeView(dev, s.compute, 0, true)), note(1, ComputeView(dev, s.compute, 1, true)); break;
        case kRtSpatialSpecular: if (!raw) note(kSpecularSlot, ComputeView(dev, s.compute, 0, true)); break;
        case kRtSpatialVfx: if (!raw) note(3, ComputeView(dev, s.compute, 0, true)); break;
        default: break;
    }
    const bool skipping = RtSkipping(frame);
    if (id == kRtUpscale) {
        g_rt.upscaleFrame = frame;
        g_rt.base = ComputeView(dev, s.compute, kUpBase, false);
        g_rt.normal = ComputeView(dev, s.compute, kUpNormal, false);
        // The camera the ray tracing used: SceneInfo is this pass's b0 (one small read a frame,
        // instead of looking through the constant buffers the game binds).
        if (g_rt.sceneFrame != frame) {
            std::string how;
            if (ReadCb0(dev, s.compute, kSceneInfoBytes, g_rt.scene, &how)) {
                g_rt.sceneFrame = frame;
            } else if (how != g_rt.sceneHow) {
                Log("Ray Reconstruction: camera (SceneInfo) not readable: %s", how.c_str());
                g_rt.sceneHow = how;
            }
        }
        if (skipping && g_rt.signalFrame == frame) {
            int copied = 0;
            for (uint32_t i = 0; i < kRtSignals; ++i) {
                const resource dst = ComputeView(dev, s.compute, kUpSignal + i, false);
                RtWatch(dst);
                copied += RtCopy(cl, g_rt.signal[i], dst);
            }
            if (copied && !g_rt.copied++)
                Log("Ray Reconstruction: %d of %u ray tracing signals handed over un-denoised", copied, kRtSignals);
        }
        return false;
    }
    if (skipping && RtIsSkipped(id)) {
        if (!g_rt.skipped++) Log("Ray Reconstruction: skipping the game's ray tracing denoise passes (%s)", kRrDenoiseNames[g_set.rrDenoise]);
        return true;
    }
    return false;
}

bool RtCreatePipelines(device* dev) {
    if (g_rt.pipe.handle) return true;
    if (g_rt.pipesFailed) return false;
    constant_range c = {};
    c.count = 24;
    c.visibility = shader_stage::all_compute;
    descriptor_range srv = {};
    srv.count = 3;
    srv.visibility = shader_stage::all_compute;
    srv.type = descriptor_type::shader_resource_view;
    descriptor_range uav = srv;
    uav.type = descriptor_type::unordered_access_view;
    const pipeline_layout_param params[3] = {pipeline_layout_param(c), pipeline_layout_param(srv), pipeline_layout_param(uav)};
    shader_desc cs = {};
    cs.code = kGuideCs;
    cs.code_size = sizeof(kGuideCs);
    pipeline_subobject sub = {pipeline_subobject_type::compute_shader, 1, &cs};
    constant_range vc = c;
    vc.count = 8;
    descriptor_range vsrv = srv, vuav = uav;
    vsrv.count = 1, vuav.count = 1;
    const pipeline_layout_param vparams[3] = {pipeline_layout_param(vc), pipeline_layout_param(vsrv), pipeline_layout_param(vuav)};
    shader_desc vcs = {};
    vcs.code = kGuideViewCs;
    vcs.code_size = sizeof(kGuideViewCs);
    pipeline_subobject vsub = {pipeline_subobject_type::compute_shader, 1, &vcs};
    if (!dev->create_pipeline_layout(3, params, &g_rt.layout) || !dev->create_pipeline(g_rt.layout, 1, &sub, &g_rt.pipe) ||
        !dev->create_pipeline_layout(3, vparams, &g_rt.viewLayout) ||
        !dev->create_pipeline(g_rt.viewLayout, 1, &vsub, &g_rt.viewPipe)) {
        g_rt.pipesFailed = true;
        Log("couldn't create the Ray Reconstruction guide shader");
        return false;
    }
    Log("Ray Reconstruction guide shader created");
    return true;
}

// Replaced guide textures, released once the GPU is done with them (a few frames on).
struct RtOld {
    resource res;
    resource_view uav;
    uint64_t frame;
};
std::vector<RtOld> g_rtOld;

void RtReleaseOld(device* dev, bool all) {
    const uint64_t now = g_presents.load();
    for (auto it = g_rtOld.begin(); it != g_rtOld.end();) {
        if (all || it->frame + 8 < now) {
            if (it->uav.handle) dev->destroy_resource_view(it->uav);
            if (it->res.handle) dev->destroy_resource(it->res);
            it = g_rtOld.erase(it);
        } else {
            ++it;
        }
    }
}

// The caller holds g_rtMutex.
bool RtGuideTextures(device* dev, uint32_t w, uint32_t h) {
    RtReleaseOld(dev, false);
    if (g_rt.guide[0].handle && g_rt.guideW == w && g_rt.guideH == h) return true;
    const format fmts[3] = {format::r8g8b8a8_unorm, format::r8g8b8a8_unorm, format::r16g16b16a16_float};
    for (int i = 0; i < 3; ++i)
        if (g_rt.guide[i].handle) g_rtOld.push_back({g_rt.guide[i], g_rt.guideUav[i], g_presents.load()});  // the GPU may still use them
    for (int i = 0; i < 3; ++i) {
        g_rt.guide[i] = {0}, g_rt.guideUav[i] = {0};
        const resource_desc d(w, h, 1, 1, fmts[i], 1, memory_heap::default_,
                              resource_usage::shader_resource | resource_usage::unordered_access);
        if (!dev->create_resource(d, nullptr, resource_usage::shader_resource_non_pixel, &g_rt.guide[i]) ||
            !dev->create_resource_view(g_rt.guide[i], resource_usage::unordered_access, resource_view_desc(fmts[i]),
                                       &g_rt.guideUav[i])) {
            Log("couldn't create the Ray Reconstruction guide textures %ux%u", w, h);
            return false;
        }
    }
    g_rt.guideW = w, g_rt.guideH = h;
    Log("Ray Reconstruction guide textures %ux%u", w, h);
    return true;
}

// The G-buffer's shader resource format: sRGB base colour decodes to linear.
// A depth view format for <f>: RE2/RE3 have 32-bit depth with stencil, RE7 plain 32-bit depth (a
// view in the wrong one lost the device).
format RtDepthSrvFormat(format f) {
    switch (f) {
        case format::r32_g8_typeless: case format::d32_float_s8_uint: case format::r32_float_x8_uint:
            return format::r32_float_x8_uint;
        case format::r32_typeless: case format::d32_float: case format::r32_float:
            return format::r32_float;
        case format::r24_g8_typeless: case format::d24_unorm_s8_uint: case format::r24_unorm_x8_uint:
            return format::r24_unorm_x8_uint;
        case format::r16_typeless: case format::d16_unorm: case format::r16_unorm:
            return format::r16_unorm;
        default:
            return format::unknown;
    }
}

format RtSrvFormat(format f) {
    if (f == format::r8g8b8a8_typeless) return format::r8g8b8a8_unorm_srgb;
    if (f == format::b8g8r8a8_typeless) return format::b8g8r8a8_unorm_srgb;
    return format_to_default_typed(f, 0);
}

// 4x4 inverse (row-major); false if singular.
bool Inverse4(const float* m, float* out) {
    float a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0f : 0.0f);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12f) return false;
        if (p != c)
            for (int k = 0; k < 8; ++k) std::swap(a[p][k], a[c][k]);
        const float inv = 1 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const float f = a[r][c];
            for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
    return true;
}

// Ray Reconstruction's camera: world-to-view from SceneInfo.transposeViewMat (byte 64, the view
// matrix's columns as rows) and view-to-clip = inverse(world-to-view) * viewProjMat.
// A projection: no x/y crosstalk (row-vector form: [0][1], [0][2], [1][0], [1][2] are 0).
bool LooksLikeProjection(const float* p) {
    const float s = std::fmax(std::fabs(p[0]), std::fabs(p[5]));
    return s > 0.05f && std::fabs(p[1]) < s * 1e-3f && std::fabs(p[2]) < s * 1e-3f && std::fabs(p[4]) < s * 1e-3f &&
           std::fabs(p[6]) < s * 1e-3f;
}

bool RtMatrices(const float* scene, float* worldToView, float* viewToClip) {
    const float* t = scene + 16;  // 3 rows of 4
    const float* vp = scene + kViewProj;
    // The view matrix's columns as rows (as the name says); if that doesn't give a projection, rows.
    static int layout = 0;  // 0 transposed, 1 as is (found on the first frame that fits)
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int l = attempt == 0 ? layout : 1 - layout;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                float v = r == 3 && c == 3 ? 1.0f : 0.0f;
                if (c < 3 && r < 4) v = l == 0 ? t[c * 4 + r] : (r < 3 ? t[r * 4 + c] : t[c * 4 + 3]);
                worldToView[r * 4 + c] = v;
            }
        float inv[16];
        if (!Inverse4(worldToView, inv)) continue;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                float sum = 0;
                for (int k = 0; k < 4; ++k) sum += inv[r * 4 + k] * vp[k * 4 + c];
                viewToClip[r * 4 + c] = sum;
            }
        if (LooksLikeProjection(viewToClip)) {
            if (l != layout) Log("Ray Reconstruction camera: SceneInfo.transposeViewMat read %s", l ? "as rows" : "as columns");
            layout = l;
            break;
        }
        if (attempt == 1) {
            static int logged = 0;
            if (logged++ < 4) Log("Ray Reconstruction camera: neither reading of transposeViewMat gives a projection");
            return false;
        }
    }
    // The rotation must be orthonormal.
    bool ok = true;
    for (int i = 0; i < 3 && ok; ++i)
        for (int j = 0; j < 3; ++j) {
            float d = 0;
            for (int k = 0; k < 3; ++k) d += worldToView[i * 4 + k] * worldToView[j * 4 + k];
            if (std::fabs(d - (i == j ? 1.0f : 0.0f)) > 5e-3f) ok = false;
        }
    if (!g_rt.loggedMatrices || !ok) {
        static int logged = 0;
        if (logged++ < 4) {
            const float* w = worldToView;
            const float* p = viewToClip;
            Log("Ray Reconstruction camera %s: world-to-view rows (%.3f %.3f %.3f %.3f) (%.3f %.3f %.3f %.3f) (%.3f %.3f "
                "%.3f %.3f) (%.3f %.3f %.3f %.3f); view-to-clip rows (%.4f %.4f %.4f %.4f) (%.4f %.4f %.4f %.4f) (%.4f "
                "%.4f %.4f %.4f) (%.4f %.4f %.4f %.4f)",
                ok ? "ok" : "NOT orthonormal", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
                w[12], w[13], w[14], w[15], p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11],
                p[12], p[13], p[14], p[15]);
        }
        g_rt.loggedMatrices = true;
    }
    return ok;
}

// Records guide.hlsl on the TAA's list (the caller puts the game's state back after DLSS). The
// G-buffer is in a shader resource state at the TAA (the lighting passes read it).
bool RtBuildGuides(command_list* cl, resource depth, const float* scene, uint32_t w, uint32_t h) {
    device* dev = cl->get_device();
    if (!g_rt.base.handle || !g_rt.normal.handle) return g_rt.error = "G-buffer not seen", false;
    const resource_desc bd = dev->get_resource_desc(g_rt.base), nd = dev->get_resource_desc(g_rt.normal);
    if (bd.texture.width < w || bd.texture.height < h || nd.texture.width < w || nd.texture.height < h)
        return g_rt.error = "G-buffer smaller than the render size", false;
    const format depthFormat = RtDepthSrvFormat(dev->get_resource_desc(depth).texture.format);
    if (depthFormat == format::unknown) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("Ray Reconstruction: depth format %u not handled",
                static_cast<unsigned>(dev->get_resource_desc(depth).texture.format));
        }
        return g_rt.error = "depth format not handled (see the log)", false;
    }
    if (!RtCreatePipelines(dev) || !RtGuideTextures(dev, w, h)) return g_rt.error = "guide shader or textures failed", false;
    const resource_view srvs[3] = {RtView(dev, depth, depthFormat, false),
                                   RtView(dev, g_rt.base, RtSrvFormat(bd.texture.format), false),
                                   RtView(dev, g_rt.normal, RtSrvFormat(nd.texture.format), false)};
    for (const resource_view& v : srvs)
        if (!v.handle) return g_rt.error = "couldn't read the G-buffer (views)", false;
    static bool loggedFormats = false;
    if (!loggedFormats) {
        loggedFormats = true;
        Log("Ray Reconstruction G-buffer: base colour %ux%u format %u, normal %ux%u format %u, depth view format %u",
            bd.texture.width, bd.texture.height, static_cast<unsigned>(bd.texture.format), nd.texture.width,
            nd.texture.height, static_cast<unsigned>(nd.texture.format), static_cast<unsigned>(depthFormat));
    }
    float params[24] = {};
    memcpy(params, scene + 56, 16 * sizeof(float));  // viewProjInvMat, byte 224
    const float* tvi = scene + 28;                     // transposeViewInvMat, byte 112: .w of each row = camera
    params[16] = tvi[3], params[17] = tvi[7], params[18] = tvi[11];
    params[20] = static_cast<float>(w), params[21] = static_cast<float>(h), params[22] = 1.0f / w, params[23] = 1.0f / h;
    const resource_usage from[3] = {resource_usage::shader_resource_non_pixel, resource_usage::shader_resource_non_pixel,
                                    resource_usage::shader_resource_non_pixel};
    const resource_usage to[3] = {resource_usage::unordered_access, resource_usage::unordered_access,
                                  resource_usage::unordered_access};
    cl->barrier(3, g_rt.guide, from, to);
    cl->bind_pipeline(pipeline_stage::all_compute, g_rt.pipe);
    cl->push_constants(shader_stage::all_compute, g_rt.layout, 0, 0, 24, params);
    descriptor_table_update u = {};
    u.count = 3;
    u.type = descriptor_type::shader_resource_view;
    u.descriptors = srvs;
    cl->push_descriptors(shader_stage::all_compute, g_rt.layout, 1, u);
    u.type = descriptor_type::unordered_access_view;
    u.descriptors = g_rt.guideUav;
    cl->push_descriptors(shader_stage::all_compute, g_rt.layout, 2, u);
    cl->dispatch((w + 7) / 8, (h + 7) / 8, 1);
    cl->barrier(3, g_rt.guide, to, from);
    return true;
}

// Debug view: guide <which> (1 diffuse, 2 specular, 3 normals, 4 roughness) over <output> (UAV state).
void RtShowGuide(command_list* cl, resource output, int which, uint32_t renderW, uint32_t renderH) {
    device* dev = cl->get_device();
    if (!g_rt.viewPipe.handle || which < 1 || which > 4) return;
    const resource g = g_rt.guide[which == 1 ? 0 : which == 2 ? 1 : 2];
    const resource_desc od = dev->get_resource_desc(output);
    const resource_view srv = RtView(dev, g, which <= 2 ? format::r8g8b8a8_unorm : format::r16g16b16a16_float, false);
    const resource_view uav = RtView(dev, output, od.texture.format, true);
    if (!srv.handle || !uav.handle) return;
    float params[8] = {static_cast<float>(od.texture.width), static_cast<float>(od.texture.height),
                       static_cast<float>(renderW) / od.texture.width, static_cast<float>(renderH) / od.texture.height};
    const uint32_t mode = which <= 2 ? 1 : which == 3 ? 2 : 3;
    memcpy(&params[4], &mode, 4);
    cl->bind_pipeline(pipeline_stage::all_compute, g_rt.viewPipe);
    cl->push_constants(shader_stage::all_compute, g_rt.viewLayout, 0, 0, 8, params);
    descriptor_table_update u = {};
    u.count = 1;
    u.type = descriptor_type::shader_resource_view;
    u.descriptors = &srv;
    cl->push_descriptors(shader_stage::all_compute, g_rt.viewLayout, 1, u);
    u.type = descriptor_type::unordered_access_view;
    u.descriptors = &uav;
    cl->push_descriptors(shader_stage::all_compute, g_rt.viewLayout, 2, u);
    cl->dispatch((od.texture.width + 7) / 8, (od.texture.height + 7) / 8, 1);
    cl->barrier(output, resource_usage::unordered_access, resource_usage::unordered_access);
}

void RtDestroy(device* dev) {
    std::lock_guard<std::mutex> lock(g_rtMutex);
    for (auto& [k, v] : g_rt.views) dev->destroy_resource_view(v);
    for (int i = 0; i < 3; ++i) {
        if (g_rt.guideUav[i].handle) dev->destroy_resource_view(g_rt.guideUav[i]);
        if (g_rt.guide[i].handle) dev->destroy_resource(g_rt.guide[i]);
    }
    if (g_rt.pipe.handle) dev->destroy_pipeline(g_rt.pipe);
    if (g_rt.viewPipe.handle) dev->destroy_pipeline(g_rt.viewPipe);
    if (g_rt.layout.handle) dev->destroy_pipeline_layout(g_rt.layout);
    if (g_rt.viewLayout.handle) dev->destroy_pipeline_layout(g_rt.viewLayout);
    RtReleaseOld(dev, true);
    g_rt = RtState();
}

// ---------------------------------------------------------------- DLSS

bool OnAccumulate(command_list* cl, ListState& s) {
    device* dev = cl->get_device();
    const Chain& c = s.chain;
    const resource output = ComputeView(dev, s.compute, 0, true);
    if (!c.depth.handle || !c.motion.handle || !c.color.handle || !output.handle) {
        Fail("FSR 2 inputs not found (depth, motion, colour or output): FSR 2 kept");
        return false;
    }
    uint32_t cb[kCbWords];
    std::string how;
    if (!ReadFsrConstants(dev, s.compute, cb, &how)) {
        Fail("FSR 2 constants not readable (" + how + "): FSR 2 kept");
        return false;
    }
    {
        static std::string lastHow;  // where cbFSR2 comes from, logged when it changes
        std::lock_guard<std::mutex> lock(g_statusMutex);
        if (how != lastHow) {
            Log("FSR 2 constants: %s", how.c_str());
            lastHow = how;
        }
    }
    float f[kCbWords];
    memcpy(f, cb, sizeof(f));
    const uint32_t renderW = cb[kRenderSize], renderH = cb[kRenderSize + 1];
    const uint32_t outW = cb[kDisplaySize], outH = cb[kDisplaySize + 1];
    const resource_desc od = dev->get_resource_desc(output);
    if (renderW < 64 || renderH < 64 || outW != od.texture.width || outH != od.texture.height) {
        Fail("FSR 2 sizes don't match the output (" + std::to_string(renderW) + "x" + std::to_string(renderH) + " -> " +
             std::to_string(outW) + "x" + std::to_string(outH) + "): FSR 2 kept");
        return false;
    }

    // Which upscaler: Auto takes DLSS when NGX starts (RTX cards), FSR 3/4 otherwise.
    ID3D12Device* d3d = reinterpret_cast<ID3D12Device*>(dev->get_native());
    std::string err;
    Tech tech = static_cast<Tech>(g_set.upscaler);
    if (tech == Tech::Auto) {
        std::string dlssErr;
        tech = dlss::Init(d3d, dlssErr) ? Tech::Dlss : Tech::Fsr;
    }
    if (tech == Tech::Dlss ? !dlss::Init(d3d, err) : !fsr::Init(d3d, err)) {
        Fail(err);
        return false;
    }
    if (tech != g_active) {  // free the other upscaler's history textures
        if (g_active == Tech::Dlss) dlss::ReleaseFeatures();
        if (g_active == Tech::Fsr) fsr::ReleaseContexts();
        Log("Upscaler: %s", tech == Tech::Dlss ? "DLSS" : "FSR 3/4");
        g_active = tech;
    }

    static uint32_t lastFrameIndex = 0;
    // FSR 2 restarts its frame count on a history reset (camera cut, menu).
    const bool reset = cb[kFrameIndex] == 0 || cb[kFrameIndex] < lastFrameIndex;
    lastFrameIndex = cb[kFrameIndex];
    // FSR 2, DLSS and FSR 3 share the jitter convention (render pixels).
    const float jitterX = f[kJitter] * (g_set.flipJitterX ? -1 : 1);
    const float jitterY = f[kJitter + 1] * (g_set.flipJitterY ? -1 : 1);
    const float preExposure = f[kPreExposure] > 0 ? f[kPreExposure] : 1;
    auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native());
    char extra[160] = "";

    bool ok;
    if (tech == Tech::Dlss) {
        dlss::Inputs in;
        in.color = reinterpret_cast<ID3D12Resource*>(c.color.handle);
        in.depth = reinterpret_cast<ID3D12Resource*>(c.depth.handle);
        in.motion = reinterpret_cast<ID3D12Resource*>(c.motion.handle);
        in.output = reinterpret_cast<ID3D12Resource*>(output.handle);
        in.renderWidth = renderW, in.renderHeight = renderH;
        in.outputWidth = outW, in.outputHeight = outH;
        in.jitterX = jitterX, in.jitterY = jitterY;
        in.motionScaleX = static_cast<float>(renderW);  // UV offsets -> render pixels
        in.motionScaleY = static_cast<float>(renderH);
        in.preExposure = preExposure;
        in.reset = reset;
        in.preset = g_set.preset;
        in.key = reinterpret_cast<const void*>(1);  // one history: FSR alternates its accumulate output
        // Ray Reconstruction while the game ray traces (rayTracingUpscale ran in the last second or
        // so, so a skipped frame doesn't switch DLSS features and restart their history).
        // The camera: the SceneInfo rayTracingUpscale read this frame.
        std::lock_guard<std::mutex> rtLock(g_rtMutex);
        static float worldToView[16], viewToClip[16];
        constexpr uint64_t kRtGraceFrames = 60;
        const uint64_t frame = g_presents.load();
        float scene[kSceneInfoBytes / 4];
        const bool sceneOk = g_rt.sceneFrame == frame;
        if (sceneOk) memcpy(scene, g_rt.scene, sizeof(scene));
        // If this frame's camera isn't usable (or wasn't read), the last good one is used for up to
        // kRtGraceFrames.
        static float goodScene[kSceneInfoBytes / 4];
        static uint64_t goodFrame = UINT64_MAX, borrowed = 0;
        bool cameraOk = false;
        if (g_set.rr && sceneOk && RtMatrices(scene, worldToView, viewToClip)) {
            cameraOk = true;
            memcpy(goodScene, scene, sizeof(scene));
            goodFrame = frame;
        } else if (g_set.rr && goodFrame != UINT64_MAX && goodFrame + kRtGraceFrames >= frame) {
            memcpy(scene, goodScene, sizeof(scene));
            cameraOk = RtMatrices(scene, worldToView, viewToClip);
            if (cameraOk && !borrowed++) Log("Ray Reconstruction: a frame without a usable camera: the last good one is used");
        }
        if (g_set.rr) {
            std::string why;
            if (!dlss::RrAvailable(&why)) g_rt.error = why;
            else if (g_rt.upscaleFrame == UINT64_MAX || g_rt.upscaleFrame + kRtGraceFrames < frame)
                g_rt.error = "the game isn't ray tracing (turn on ray tracing)";
            else if (!sceneOk && !cameraOk) g_rt.error = "camera (SceneInfo) not found yet";
            else if (!cameraOk) g_rt.error = "camera matrices not as expected (see the log)";
            else if (RtBuildGuides(cl, c.depth, scene, renderW, renderH)) {
                in.rr = true;
                in.rrPreset = g_set.rrPreset;
                in.diffuseAlbedo = reinterpret_cast<ID3D12Resource*>(g_rt.guide[0].handle);
                in.specularAlbedo = reinterpret_cast<ID3D12Resource*>(g_rt.guide[1].handle);
                in.normalRoughness = reinterpret_cast<ID3D12Resource*>(g_rt.guide[2].handle);
                in.worldToView = worldToView;
                in.viewToClip = viewToClip;
            }
        }
        static std::string lastWhy;  // why Ray Reconstruction last didn't run (logged when it changes)
        static uint64_t whyLogged = 0;
        if (g_set.rr && !in.rr) {
            if (g_rt.error != lastWhy && whyLogged < 50) {
                ++whyLogged;
                Log("Ray Reconstruction not used: %s", g_rt.error.c_str());
            }
            lastWhy = g_rt.error;
        } else {
            lastWhy.clear();
        }
        ok = dlss::Evaluate(list, in, err);
        if (ok && in.rr) {
            g_rt.error.clear();
            g_rt.rrFrame = frame;
            if (!g_rt.rrFrames++) Log("DLSS Ray Reconstruction: first frame (%ux%u -> %ux%u)", renderW, renderH, outW, outH);
            if (g_set.guideView > 0) {
                cl->bind_descriptor_tables(shader_stage::all_compute, s.compute.layout, 0, 0, nullptr);  // ReShade's heaps back
                RtShowGuide(cl, output, g_set.guideView, renderW, renderH);
            }
        }
        if (ok) snprintf(extra, sizeof(extra), "%s", in.rr ? ", Ray Reconstruction" : "");
    } else {
        // FSR 2's depth -> view depth parameters (reversed Z): infinite far plane: (~0, near);
        // finite: (-q, q * far) with q = near / (far - near).
        const float dv0 = f[kDeviceToViewDepth], dv1 = f[kDeviceToViewDepth + 1];
        float nearZ = 0.01f, farZ = 0;
        if (std::fabs(dv0) < 1e-4f) {
            nearZ = dv1;
        } else if (dv0 < 0) {
            farZ = dv1 / -dv0;
            nearZ = dv1 / (1 - dv0);
        }
        if (!(nearZ > 1e-5f && nearZ < 100) || (farZ != 0 && !(farZ > nearZ))) nearZ = 0.01f, farZ = 0;
        const float tanHalf = f[kTanHalfFov];
        const float fovY = tanHalf > 0.01f && tanHalf < 20 ? 2 * std::atan(tanHalf) : 1.0f;

        fsr::Inputs in;
        in.color = reinterpret_cast<ID3D12Resource*>(c.color.handle);
        in.depth = reinterpret_cast<ID3D12Resource*>(c.depth.handle);
        in.motion = reinterpret_cast<ID3D12Resource*>(c.motion.handle);
        in.reactive = g_set.fsrReactive ? reinterpret_cast<ID3D12Resource*>(c.reactive.handle) : nullptr;
        in.output = reinterpret_cast<ID3D12Resource*>(output.handle);
        in.renderWidth = renderW, in.renderHeight = renderH;
        in.outputWidth = outW, in.outputHeight = outH;
        in.jitterX = jitterX, in.jitterY = jitterY;
        in.motionScaleX = static_cast<float>(renderW);
        in.motionScaleY = static_cast<float>(renderH);
        in.preExposure = preExposure;
        in.cameraNear = nearZ, in.cameraFar = farZ, in.cameraFovY = fovY;
        in.metersPerUnit = 1.0f;  // RE Engine: metres
        in.sharpness = 0;         // the game's RCAS sharpens afterwards
        in.reset = reset;
        ok = fsr::Evaluate(list, in, err);
        snprintf(extra, sizeof(extra), ", near %.4f, far %s, FOV %.1f deg, reactive mask %s", nearZ,
                 farZ > 0 ? std::to_string(farZ).c_str() : "infinite", fovY * 57.29578f,
                 in.reactive ? "yes" : (g_set.fsrReactive ? "not found" : "off"));
    }
    if (!ok) {
        Restore(cl, s);
        Fail(err);
        return false;
    }
    cl->barrier(output, resource_usage::unordered_access, resource_usage::unordered_access);
    Restore(cl, s);

    const std::string mode = std::string(tech == Tech::Dlss ? "DLSS " : "FSR ") + std::to_string(renderW) + "x" +
                             std::to_string(renderH) + " -> " + std::to_string(outW) + "x" + std::to_string(outH) +
                             (tech == Tech::Dlss ? extra : "");
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (!g_replaced++ || !g_lastError.empty() || mode != g_mode)
        Log("%s running: %s, jitter %.3f %.3f px, pre-exposure %.3f%s%s", tech == Tech::Dlss ? "DLSS" : "FSR",
            mode.c_str(), jitterX, jitterY, preExposure, tech == Tech::Dlss ? "" : extra, reset ? ", reset" : "");
    g_mode = mode;
    g_lastError.clear();
    g_lastRun = g_presents.load() + 1;
    return true;
}

void OnDestroyDevice(device* dev) {
    RtDestroy(dev);
    dlss::Shutdown();
    fsr::Shutdown();
}

// After the game's final pass: the scene back into the HUD texture, the same draw into our
// render target, the game's render target back.
uint64_t g_hudlessDrawn = 0;
void DrawHudless(command_list* cl, ListState& s) {
    device* dev = cl->get_device();
    resource tex = {0};
    resource_view rtv = {0};
    if (!s.rtv.handle || !s.copySrc.handle || !FrameGenHudlessTarget(dev, &tex, &rtv)) return;
    const resource_usage srcWas = s.srcState, dstWas = s.dstState;
    if (srcWas == resource_usage::undefined || dstWas == resource_usage::undefined) return;
    if (srcWas != resource_usage::copy_source) cl->barrier(s.copySrc, srcWas, resource_usage::copy_source);
    if (dstWas != resource_usage::copy_dest) cl->barrier(s.copyDst, dstWas, resource_usage::copy_dest);
    cl->copy_resource(s.copySrc, s.copyDst);
    if (srcWas != resource_usage::copy_source) cl->barrier(s.copySrc, resource_usage::copy_source, srcWas);
    if (dstWas != resource_usage::copy_dest) cl->barrier(s.copyDst, resource_usage::copy_dest, dstWas);
    cl->barrier(tex, resource_usage::shader_resource, resource_usage::render_target);
    cl->bind_render_targets_and_depth_stencil(1, &rtv);
    cl->draw(s.hudVertices, s.hudInstances, 0, 0);
    cl->bind_render_targets_and_depth_stencil(1, &s.rtv);
    cl->barrier(tex, resource_usage::render_target, resource_usage::shader_resource);
    FrameGenHudlessDrawn(cl);
    if (++g_hudlessDrawn == 1) Log("frame generation: picture before the HUD drawn (final pass repeated without the HUD)");
}

// Frame generation's inputs for this frame: the camera (this list's SceneInfo, else the last one
// seen), the game's converted depth and motion vectors (shader resource state, as FSR 2 left them),
// FSR 2's render size, jitter and reset.
void GiveFrameGen(command_list* cl, ListState& s) {
    if (!FrameGenWantsCamera() || !s.chain.depth.handle || !s.chain.motion.handle) return;
    device* dev = cl->get_device();
    uint32_t cb[kCbWords];
    std::string how;
    if (!ReadFsrConstants(dev, s.compute, cb, &how)) return;
    float f[kCbWords];
    memcpy(f, cb, sizeof(f));
    const uint32_t rw = cb[kRenderSize], rh = cb[kRenderSize + 1];
    g_sceneW = rw, g_sceneH = rh;  // what SceneInfo must say from now on
    if (s.sceneValid) {
        FrameGenSetSceneInfo(s.scene);
    } else {
        std::lock_guard<std::mutex> lock(g_sceneMutex);
        if (g_lastSceneValid && g_lastScene[kScreenSizeFloat] == static_cast<float>(rw) &&
            g_lastScene[kScreenSizeFloat + 1] == static_cast<float>(rh))
            FrameGenSetSceneInfo(g_lastScene);
    }
    static uint32_t lastFrameIndex = 0;
    const bool reset = cb[kFrameIndex] == 0 || cb[kFrameIndex] < lastFrameIndex;
    lastFrameIndex = cb[kFrameIndex];
    constexpr unsigned kShaderResource = 0x40 | 0x80;  // D3D12 non-pixel + pixel shader resource
    FrameGenTag(reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native()),
                reinterpret_cast<ID3D12Resource*>(s.chain.depth.handle), reinterpret_cast<ID3D12Resource*>(s.chain.motion.handle),
                kShaderResource, rw, rh, f[kJitter], f[kJitter + 1], reset);
}

bool g_keyWasDown = false;

}  // namespace

const ImVec4 kWarn(1, 0.6f, 0.3f, 1);

float ControlWidth() { return ImGui::GetFontSize() * 14.0f; }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void RestoreListState(command_list* cl) {
    if (ListState* s = State(cl)) Restore(cl, *s);
}

bool UpscalerOnDraw(command_list* cl, uint32_t vertices, uint32_t instances) {
    ListState* s = State(cl);
    if (!s || !s->pipe.handle || PipelinePs(s->pipe) != kFinalPs) return false;
    // The final pass reads the HUD texture, filled from the scene by the last copy (the dst must
    // still be in the shader-resource state the pass needs).
    if (FrameGenWantsHudless() && s->copyDst.handle && s->rtv.handle) {
        s->hudPending = true;
        s->hudVertices = vertices, s->hudInstances = instances;
    }
    return false;
}

bool UpscalerOnDispatch(command_list* cl) {
    ListState* s = State(cl);
    if (!s) return false;
    const uint32_t id = PipelineCs(s->pipe);
    if (RtOnDispatch(cl, *s, id)) return true;  // a skipped ray tracing denoise pass
    switch (id) {
        case kReactiveCs:  // FSR 2's passes in order: reactive mask, velocity, depth, luminance (colour)
            s->chain = Chain();
            s->chain.reactive = ComputeView(cl->get_device(), s->compute, 0, true);
            return false;
        case kVelocityCs: s->chain.motion = ComputeView(cl->get_device(), s->compute, 0, true); return false;
        case kDepthCs: s->chain.depth = ComputeView(cl->get_device(), s->compute, 0, true); return false;
        case kLumaCs: s->chain.color = ComputeView(cl->get_device(), s->compute, 0, false); return false;
        case kAccumulateCs: {
            GiveFrameGen(cl, *s);
            const bool replaced = g_set.enabled && OnAccumulate(cl, *s);
            s->chain = Chain();  // each frame gathers its own inputs
            return replaced;
        }
        default: return false;
    }
}

namespace {

// The render resolution for RE4DLSS.lua: the wanted FSR 2 quality (-1 = the game's) in
// reframework/data/RE4DLSS_mode.txt; the script reports the current one in RE4DLSS_status.txt.
std::wstring DataDir() { return ExeDir() + L"reframework\\data\\"; }

int WantedQuality() { return g_set.enabled ? kResolutions[g_set.resolution].quality : -1; }

int g_writtenQuality = -2;

void WriteWantedQuality() {
    const int want = WantedQuality();
    if (want == g_writtenQuality) return;
    if (GetFileAttributesW((ExeDir() + L"reframework").c_str()) == INVALID_FILE_ATTRIBUTES) return;  // no REFramework
    CreateDirectoryW(DataDir().c_str(), nullptr);
    if (FILE* f = _wfopen((DataDir() + L"RE4DLSS_mode.txt").c_str(), L"w")) {
        fprintf(f, "%d\n", want);
        fclose(f);
        g_writtenQuality = want;
        Log("render resolution for RE4DLSS.lua: %s", want >= 0 ? kResolutions[g_set.resolution].name : "the game's FSR 2 setting");
    }
}

// The script's last report (the engine's FSR 2 quality now), if it's recent (the script runs).
bool ScriptStatus(int* quality) {
    const std::wstring path = DataDir() + L"RE4DLSS_status.txt";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const uint64_t t = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    const uint64_t n = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    if (n > t && n - t > 50000000ull) return false;  // older than 5 s
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) return false;
    int v = -1;
    const int got = fscanf(f, "%d", &v);
    fclose(f);
    if (got != 1) return false;
    *quality = v;
    return true;
}

const char* QualityName(int q) {
    for (const Resolution& r : kResolutions)
        if (r.quality == q && q >= 0) return r.name;
    return "?";
}

}  // namespace

void UpscalerPresent(swapchain* sc) {
    WriteWantedQuality();
    dlss::EndFrame();
    fsr::EndFrame();
    ++g_presents;
    if (g_presents % 600 == 0) {
        const std::string d = FrameGenDiag();
        if (!d.empty() && d.find("frames untagged 0,") == std::string::npos) Log("frame generation diag:%s", d.c_str());
    }
    // Load AMD's runtime as soon as FSR is chosen, so the log and overlay say at once whether it
    // can run (not only when the game next runs its FSR 2 passes).
    if (g_set.upscaler == static_cast<int>(Tech::Fsr)) {
        std::string err;
        fsr::Init(reinterpret_cast<ID3D12Device*>(sc->get_device()->get_native()), err);
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_set.enabled = !g_set.enabled;
        SaveSettings();
        Log("Upscaling %s (F9)", g_set.enabled ? "on" : "off");
    }
    g_keyWasDown = down;
}

// DLSS models offered (NVSDK_NGX_DLSS_Hint_Render_Preset_*; dlss::kPresetAuto = per render scale).
const char* const kModelNames[] = {"Automatic (recommended)", "K (transformer)", "J (transformer)",
                                   "L (transformer 2, DLSS 4.5)", "M (transformer 2, DLSS 4.5)", "NVIDIA's default"};
const int kModelIds[] = {dlss::kPresetAuto, 11, 10, 12, 13, 0};
constexpr int kModelCount = 6;
int ModelIndex(int id) {
    for (int i = 0; i < kModelCount; ++i)
        if (kModelIds[i] == id) return i;
    return 0;
}

// The upscaler that runs with the current settings: DLSS or FSR (Auto resolves once DLSS was tried).
Tech ResolvedTech() {
    if (g_set.upscaler == static_cast<int>(Tech::Dlss)) return Tech::Dlss;
    if (g_set.upscaler == static_cast<int>(Tech::Fsr)) return Tech::Fsr;
    return g_active != Tech::Auto ? g_active : Tech::Dlss;
}

// One line for the top of the overlay: what upscales right now; *warn when it's a problem.
std::string UpscalerHeadline(bool* warn) {
    *warn = false;
    std::string err, mode;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        err = g_lastError;
        mode = g_mode;
    }
    if (!g_set.enabled) return "Upscaling off (F9): the game's own FSR 2 upscales";
    const uint64_t presents = g_presents.load(), lastRun = g_lastRun.load();
    if (lastRun && presents < lastRun + 30) return "Upscaling: " + mode;
    *warn = true;
    if (!err.empty()) return "Upscaling not running: " + err;
    return "Upscaling waits for the game's FSR 2: turn it on in the graphics settings (menus don't use it)";
}

void UpscalerOverlay() {
    ImGui::TextDisabled(RD_PRODUCT " " RD_VERSION);
    bool warn = false;
    const std::string up = UpscalerHeadline(&warn);
    if (warn) ImGui::TextColored(kWarn, "%s", up.c_str());
    else ImGui::TextUnformatted(up.c_str());
    const std::string fg = FrameGenHeadline(&warn);
    if (warn) ImGui::TextColored(kWarn, "%s", fg.c_str());
    else ImGui::TextUnformatted(fg.c_str());
    ImGui::Spacing();

    if (!ImGui::BeginTabBar("##re4dlss")) return;
    bool changed = false;
    if (ImGui::BeginTabItem("Settings")) {
        ImGui::SeparatorText("Upscaling");
        changed |= ImGui::Checkbox("Upscaling (F9)", &g_set.enabled);
        Help("Puts DLSS or FSR 3/4 in place of the game's FSR 2. Turn FSR 2 on in the game's graphics settings: "
             "its quality picks the render size unless Render resolution below picks one, and the game's "
             "Sharpness setting still applies.\n\nF9 switches it on and off.");
        ImGui::BeginDisabled(!g_set.enabled);
        const char* techs[] = {"Auto", "DLSS", "FSR 3.1 / FSR 4"};
        if (g_set.upscaler < 0 || g_set.upscaler > 2) g_set.upscaler = 0;
        ImGui::SetNextItemWidth(ControlWidth());
        if (ImGui::Combo("Upscaler", &g_set.upscaler, techs, 3)) {
            changed = true;
            Log("Upscaler setting: %s", TechName(g_set.upscaler));
        }
        Help("Auto: DLSS on NVIDIA RTX cards, FSR on others.\nDLSS: NVIDIA DLSS Super Resolution (RTX cards).\n"
             "FSR 3.1 / FSR 4: AMD's upscaler, any DirectX 12 card; AMD's runtime picks FSR 4 on Radeon cards that "
             "support it.\n\nSwitches right away.");
        ImGui::SetNextItemWidth(ControlWidth());
        static const char* const resNames[] = {kResolutions[0].name, kResolutions[1].name, kResolutions[2].name,
                                               kResolutions[3].name, kResolutions[4].name, kResolutions[5].name};
        static_assert(sizeof(resNames) / sizeof(resNames[0]) == kResolutionCount);
        if (ImGui::Combo("Render resolution", &g_set.resolution, resNames, kResolutionCount)) {
            changed = true;
            Log("Render resolution setting: %s", kResolutions[g_set.resolution].name);
        }
        Help("The size the game renders at before upscaling. Native (100%) isn't in the game's menu: DLSS then "
             "runs as DLAA, FSR as native anti-aliasing. The others match the game's FSR 2 qualities. \"Game's "
             "FSR 2 setting\" leaves it to the menu.\n\nNeeds REFramework (the bundled RE4DLSS.lua sets it) and "
             "the game's FSR 2 on. Switches right away.");
        if (g_set.resolution > 0) {
            int q = -1;
            if (ScriptStatus(&q))
                ImGui::TextDisabled("Game's FSR 2 now: %s", q == kResolutions[g_set.resolution].quality ? "as chosen" : QualityName(q));
            else
                ImGui::TextColored(kWarn, "RE4DLSS.lua isn't running: is REFramework installed?");
        }
        {
            // Always shown (greyed out while FSR upscales) so it's easy to find.
            const bool fsrRuns = ResolvedTech() == Tech::Fsr;
            ImGui::BeginDisabled(fsrRuns);
            int model = ModelIndex(g_set.preset);
            ImGui::SetNextItemWidth(ControlWidth());
            if (ImGui::Combo("DLSS model", &model, kModelNames, kModelCount)) {
                g_set.preset = kModelIds[model];
                changed = true;
                Log("DLSS model setting: %s", kModelNames[model]);
            }
            ImGui::EndDisabled();
            Help("Which DLSS model upscales. Automatic follows NVIDIA's recommendation for the render "
                 "resolution: K from 100% down to Balanced, M at Performance (50%), L at Ultra "
                 "Performance (33%). K, J, L or M use that model at every resolution; L and M (DLSS 4.5's "
                 "second-generation transformer) cost more, especially on RTX 20 and 30 cards. NVIDIA's "
                 "default leaves it to the driver. A DLSS override set in the NVIDIA App takes "
                 "precedence.\n\nOnly used when DLSS upscales (greyed out while FSR does).");
            ImGui::BeginDisabled(fsrRuns);
            if (ImGui::Checkbox("Ray Reconstruction (experimental)", &g_set.rr)) {
                changed = true;
                Log("Ray Reconstruction setting: %s", g_set.rr ? "on" : "off");
            }
            Help("NVIDIA's DLSS Ray Reconstruction in place of DLSS Super Resolution while the game ray traces:\n"
                 "it upscales and cleans up what's left of the noise in the ray-traced reflections, on top of the\n"
                 "game's own ray tracing denoiser. Needs the game's ray tracing on (Super Resolution otherwise).\n"
                 "Experimental. The Advanced tab can skip parts of the game's denoiser for testing.");
            if (g_set.rr) {
                static const char* const kRrNames[] = {"NVIDIA's default", "D (transformer)", "E (latest transformer)",
                                                       "F (RR 2)"};
                static const int kRrIds[] = {0, 4, 5, 6};
                int cur = g_set.rrPreset == 4 ? 1 : g_set.rrPreset == 5 ? 2 : g_set.rrPreset == 6 ? 3 : 0;
                ImGui::SetNextItemWidth(ControlWidth());
                if (ImGui::Combo("Ray Reconstruction model", &cur, kRrNames, 4)) {
                    g_set.rrPreset = kRrIds[cur];
                    changed = true;
                    Log("Ray Reconstruction model setting: %d", g_set.rrPreset);
                }
                Help("D, E or F, or NVIDIA's default. A Ray Reconstruction override set in the NVIDIA App takes\n"
                     "precedence (set it to \"Use app default\" there for this choice to apply).");
                std::string rrErr;
                {
                    std::lock_guard<std::mutex> lock(g_rtMutex);
                    rrErr = g_rt.error;
                }
                if (!rrErr.empty() && g_set.enabled) ImGui::TextColored(kWarn, "Ray Reconstruction: %s", rrErr.c_str());
            }
            ImGui::EndDisabled();
        }
        ImGui::EndDisabled();
        DrawFrameGenSettings();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Status")) {
        ImGui::SeparatorText("Upscaling");
        ImGui::TextDisabled("DLSS: %s", dlss::Status().c_str());
        ImGui::TextDisabled("FSR: %s", fsr::Status().c_str());
        std::string err;
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            err = g_lastError;
        }
        if (g_set.enabled && !err.empty()) ImGui::TextColored(kWarn, "Last problem: %s", err.c_str());
        if (g_set.rr) {
            std::lock_guard<std::mutex> lock(g_rtMutex);
            ImGui::TextDisabled("Ray Reconstruction frames %llu; game's ray tracing denoise passes skipped %llu, "
                                "signal handed over %llu times",
                                static_cast<unsigned long long>(g_rt.rrFrames), static_cast<unsigned long long>(g_rt.skipped),
                                static_cast<unsigned long long>(g_rt.copied));
        }
        DrawFrameGenStatus();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Advanced")) {
        ImGui::SeparatorText("Upscaling");
        changed |= ImGui::Checkbox("FSR: use the game's reactive mask", &g_set.fsrReactive);
        Help("The mask the game makes for its FSR 2 (particles, transparent effects): FSR 3.1 trusts the current "
             "frame more there, against ghosting. FSR 4 ignores it.");
        changed |= ImGui::Checkbox("Flip jitter X", &g_set.flipJitterX);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Flip jitter Y", &g_set.flipJitterY);
        Help("Testing only: the sign of the camera jitter given to DLSS and FSR. Leave both off.");
        ImGui::SeparatorText("Ray Reconstruction");
        ImGui::SetNextItemWidth(ControlWidth() * 1.4f);
        if (ImGui::Combo("Game's ray tracing denoiser", &g_set.rrDenoise, kRrDenoiseNames, 3)) {
            changed = true;
            Log("Ray Reconstruction denoising setting: %s", kRrDenoiseNames[g_set.rrDenoise]);
        }
        Help("While Ray Reconstruction runs, which of the game's own ray tracing denoise passes still run.\n"
             "Game's denoiser (recommended): everything stays; Ray Reconstruction upscales and cleans up the\n"
             "result. Skip all but the spatial pass: the game's frame-to-frame (temporal) filtering, variance\n"
             "and deflicker are skipped. Skip all: Ray Reconstruction gets the raw reflection rays. The game\n"
             "traces below the render size, so the skipping choices come out blotchier.");
        ImGui::SetNextItemWidth(ControlWidth());
        if (ImGui::Combo("Show guide", &g_set.guideView, kGuideViewNames, kGuideViewCount))
            Log("diagnosis: show guide %s", kGuideViewNames[g_set.guideView]);
        Help("Shows one of the surface pictures Ray Reconstruction gets from the game in place of the image:\n"
             "surface colour, reflectivity, normals or roughness. For checking; off at each launch.");
        DrawFrameGenAdvanced();
        ImGui::SeparatorText("Development");
        DrawCaptureTool();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    if (changed) SaveSettings();
}

void UpscalerInit() {
    LoadSettings();
    reshade::register_event<reshade::addon_event::init_command_list>(&OnInitList);
    reshade::register_event<reshade::addon_event::destroy_command_list>(&OnDestroyList);
    reshade::register_event<reshade::addon_event::reset_command_list>(&OnResetList);
    reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
    reshade::register_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::register_event<reshade::addon_event::push_constants>(&OnPushConstants);
    reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRts);
    reshade::register_event<reshade::addon_event::copy_resource>(&OnCopy);
    reshade::register_event<reshade::addon_event::copy_buffer_region>(&OnCopyBuffer);
    reshade::register_event<reshade::addon_event::destroy_resource>(&OnDestroyBuffer);
    reshade::register_event<reshade::addon_event::barrier>(&OnBarrier);
    RegisterFrameGen();
    Log("Upscaling %s at start (F9 toggles), upscaler setting %d (0 auto, 1 DLSS, 2 FSR)", g_set.enabled ? "on" : "off",
        g_set.upscaler);
}

void UpscalerShutdown() {
    reshade::unregister_event<reshade::addon_event::init_command_list>(&OnInitList);
    reshade::unregister_event<reshade::addon_event::destroy_command_list>(&OnDestroyList);
    reshade::unregister_event<reshade::addon_event::reset_command_list>(&OnResetList);
    reshade::unregister_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
    reshade::unregister_event<reshade::addon_event::bind_descriptor_tables>(&OnBindTables);
    reshade::unregister_event<reshade::addon_event::push_descriptors>(&OnPushDescriptors);
    reshade::unregister_event<reshade::addon_event::push_constants>(&OnPushConstants);
    reshade::unregister_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
    reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRts);
    reshade::unregister_event<reshade::addon_event::copy_resource>(&OnCopy);
    reshade::unregister_event<reshade::addon_event::copy_buffer_region>(&OnCopyBuffer);
    reshade::unregister_event<reshade::addon_event::destroy_resource>(&OnDestroyBuffer);
    reshade::unregister_event<reshade::addon_event::barrier>(&OnBarrier);
    UnregisterFrameGen();
}

}  // namespace rd
