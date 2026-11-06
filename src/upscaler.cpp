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
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "RenderResolution", g_set.resolution);
    reshade::set_config_value(nullptr, RD_PRODUCT, "DlssModel", g_set.preset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Upscaler", g_set.upscaler);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FsrReactiveMask", g_set.fsrReactive);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
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

void OnBindRts(command_list* cl, uint32_t count, const resource_view* rtvs, resource_view) {
    if (ListState* s = State(cl)) s->rtv = count ? rtvs[0] : resource_view{0};
}

bool OnCopy(command_list* cl, resource src, resource dst) {
    ListState* s = State(cl);
    if (!s) return false;
    device* dev = cl->get_device();
    const resource_desc a = dev->get_resource_desc(src), b = dev->get_resource_desc(dst);
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
    // SceneInfo is written by the CPU every frame (upload memory); GPU-only buffers can't be read.
    if (d.type != resource_type::buffer || d.heap == memory_heap::default_ || r.offset + kSceneInfoBytes > d.buffer.size)
        return;
    void* data = nullptr;
    if (!dev->map_buffer_region(r.buffer, r.offset, kSceneInfoBytes, map_access::read_only, &data) || !data) return;
    const float* f = static_cast<const float*>(data);
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
    if (match) {
        memcpy(s.scene, f, kSceneInfoBytes);
        s.sceneValid = true;
    }
    dev->unmap_buffer_region(r.buffer);
    if (!match) return;
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

// cbFSR2: the compute root CBV b0 space0.
bool ReadFsrConstants(device* dev, const StageState& st, uint32_t* out) {
    descriptor_tracking* dt = dev->get_private_data<descriptor_tracking>();
    if (!dt) return false;
    for (const auto& [param, r] : st.roots) {
        if (r.type != descriptor_type::constant_buffer) continue;
        const pipeline_layout_param p = dt->get_pipeline_layout_param(st.layout, param);
        if (p.type != pipeline_layout_param_type::push_descriptors || p.push_descriptors.dx_register_index != 0 ||
            p.push_descriptors.dx_register_space != 0)
            continue;
        void* data = nullptr;
        if (!dev->map_buffer_region(r.range.buffer, r.range.offset, kCbWords * 4, map_access::read_only, &data) || !data)
            return false;
        memcpy(out, data, kCbWords * 4);
        dev->unmap_buffer_region(r.range.buffer);
        return true;
    }
    return false;
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
    if (!ReadFsrConstants(dev, s.compute, cb)) {
        Fail("FSR 2 constants not found: FSR 2 kept");
        return false;
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
        ok = dlss::Evaluate(list, in, err);
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
                             std::to_string(renderH) + " -> " + std::to_string(outW) + "x" + std::to_string(outH);
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (!g_replaced++ || !g_lastError.empty() || mode != g_mode)
        Log("%s running: %s, jitter %.3f %.3f px, pre-exposure %.3f%s%s", tech == Tech::Dlss ? "DLSS" : "FSR",
            mode.c_str(), jitterX, jitterY, preExposure, extra, reset ? ", reset" : "");
    g_mode = mode;
    g_lastError.clear();
    g_lastRun = g_presents.load() + 1;
    return true;
}

void OnDestroyDevice(device*) {
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
    if (!ReadFsrConstants(dev, s.compute, cb)) return;
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
    reshade::unregister_event<reshade::addon_event::barrier>(&OnBarrier);
    UnregisterFrameGen();
}

}  // namespace rd
