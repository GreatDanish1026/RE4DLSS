// DLSS in place of FSR 2's accumulate pass in Resident Evil 4 (docs/FINDINGS.md).
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
//
// NGX records its own compute state and descriptor heaps on the game's command list, so the
// game's bindings are tracked per command list and put back afterwards (as in VillageDLSS).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>

#include "common.hpp"

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../third_party/reshade/utils/descriptor_tracking.hpp"
#include "dlss.hpp"
#include "version.h"

using namespace reshade::api;

namespace rd {
namespace {

constexpr uint32_t kDepthCs = 0xd2efdab0;       // CS_UpscalingDepthTranslation
constexpr uint32_t kVelocityCs = 0x5469503e;    // CS_UpscalingVelocityTranslate
constexpr uint32_t kLumaCs = 0xaa8d773e;        // FSR 2 luminance pyramid (t0 = render-size colour)
constexpr uint32_t kAccumulateCs = 0x226d3330;  // FSR 2 accumulate (u0 = output-size image)
constexpr int kToggleHotkey = VK_F9;

// cbFSR2 (FSR 2.1), as 32-bit words.
constexpr int kRenderSize = 0;    // int2
constexpr int kDisplaySize = 2;   // int2
constexpr int kFrameIndex = 7;    // uint
constexpr int kJitter = 10;       // float2, render pixels
constexpr int kPreExposure = 28;  // float
constexpr int kCbWords = 36;      // 144 bytes

// ---------------------------------------------------------------- settings

struct Settings {
    bool enabled = true;  // F9; remembered across launches
    int preset = 11;      // NVSDK_NGX_DLSS_Hint_Render_Preset_K (transformer); 0 = DLSS default
    bool flipJitterX = false, flipJitterY = false;  // testing: jitter sign
} g_set;

void LoadSettings() {
    reshade::get_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::get_config_value(nullptr, RD_PRODUCT, "Preset", g_set.preset);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::get_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
}

void SaveSettings() {
    reshade::set_config_value(nullptr, RD_PRODUCT, "Enabled", g_set.enabled);
    reshade::set_config_value(nullptr, RD_PRODUCT, "Preset", g_set.preset);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterX", g_set.flipJitterX);
    reshade::set_config_value(nullptr, RD_PRODUCT, "FlipJitterY", g_set.flipJitterY);
}

std::mutex g_statusMutex;
std::string g_lastError, g_lastLogged, g_mode;
uint64_t g_replaced = 0;

void Fail(const std::string& why) {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (why != g_lastLogged) Log("DLSS: %s", why.c_str());
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
    resource depth = {0}, motion = {0}, color = {0};
};

struct __declspec(uuid("4c2a9e17-6b3d-4f81-a5c0-9d2e7b1f3a64")) ListState {
    pipeline pipe = {0};
    pipeline_stage pipeStages = pipeline_stage::all;
    StageState compute, graphics;
    Chain chain;
};

ListState* State(command_list* cl) { return cl->get_private_data<ListState>(); }

StageState* Stage(ListState* s, shader_stage stages) {
    if ((stages & shader_stage::all_compute) != 0) return &s->compute;
    if ((stages & shader_stage::all_graphics) != 0) return &s->graphics;
    return nullptr;
}

void OnInitList(command_list* cl) { cl->create_private_data<ListState>(); }
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

    std::string err;
    if (!dlss::Init(reinterpret_cast<ID3D12Device*>(dev->get_native()), err)) {
        Fail(err);
        return false;
    }
    static uint32_t lastFrameIndex = 0;
    dlss::Inputs in;
    in.color = reinterpret_cast<ID3D12Resource*>(c.color.handle);
    in.depth = reinterpret_cast<ID3D12Resource*>(c.depth.handle);
    in.motion = reinterpret_cast<ID3D12Resource*>(c.motion.handle);
    in.output = reinterpret_cast<ID3D12Resource*>(output.handle);
    in.renderWidth = renderW, in.renderHeight = renderH;
    in.outputWidth = outW, in.outputHeight = outH;
    // FSR 2 and DLSS use the same jitter convention (render pixels).
    in.jitterX = f[kJitter] * (g_set.flipJitterX ? -1 : 1);
    in.jitterY = f[kJitter + 1] * (g_set.flipJitterY ? -1 : 1);
    in.motionScaleX = static_cast<float>(renderW);  // UV offsets -> render pixels
    in.motionScaleY = static_cast<float>(renderH);
    in.preExposure = f[kPreExposure] > 0 ? f[kPreExposure] : 1;
    in.reset = cb[kFrameIndex] == 0 || cb[kFrameIndex] < lastFrameIndex;  // FSR 2 restarts its count on a reset
    lastFrameIndex = cb[kFrameIndex];
    in.preset = g_set.preset;
    in.key = reinterpret_cast<const void*>(1);  // one history: FSR alternates its accumulate output

    if (!dlss::Evaluate(reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native()), in, err)) {
        Restore(cl, s);
        Fail(err);
        return false;
    }
    cl->barrier(output, resource_usage::unordered_access, resource_usage::unordered_access);
    Restore(cl, s);

    const std::string mode = std::to_string(renderW) + "x" + std::to_string(renderH) + " -> " + std::to_string(outW) +
                             "x" + std::to_string(outH);
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (!g_replaced++ || !g_lastError.empty() || mode != g_mode)
        Log("DLSS running: %s, jitter %.3f %.3f px, pre-exposure %.3f%s", mode.c_str(), in.jitterX, in.jitterY,
            in.preExposure, in.reset ? ", reset" : "");
    g_mode = mode;
    g_lastError.clear();
    return true;
}

void OnDestroyDevice(device*) { dlss::Shutdown(); }

bool g_keyWasDown = false;

}  // namespace

bool UpscalerOnDraw(command_list*, uint32_t, uint32_t) { return false; }

bool UpscalerOnDispatch(command_list* cl) {
    ListState* s = State(cl);
    if (!s) return false;
    const uint32_t id = PipelineCs(s->pipe);
    switch (id) {
        case kDepthCs: s->chain.depth = ComputeView(cl->get_device(), s->compute, 0, true); return false;
        case kVelocityCs:  // the first of FSR 2's inputs in each frame (then depth, then colour)
            s->chain = Chain();
            s->chain.motion = ComputeView(cl->get_device(), s->compute, 0, true);
            return false;
        case kLumaCs: s->chain.color = ComputeView(cl->get_device(), s->compute, 0, false); return false;
        case kAccumulateCs: return g_set.enabled && OnAccumulate(cl, *s);
        default: return false;
    }
}

void UpscalerPresent(swapchain*) {
    dlss::EndFrame();
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_set.enabled = !g_set.enabled;
        SaveSettings();
        Log("DLSS %s (F9)", g_set.enabled ? "on" : "off");
    }
    g_keyWasDown = down;
}

void UpscalerOverlay() {
    bool changed = ImGui::Checkbox("DLSS in place of FSR 2 (F9)", &g_set.enabled);
    ImGui::TextDisabled("Turn FSR 2 on in the game; its quality sets the render size (Quality 67%%, Balanced 59%%,");
    ImGui::TextDisabled("Performance 50%%, Ultra Performance 33%%). The game's sharpness setting still applies.");
    const char* presets[] = {"DLSS default", "J (transformer)", "K (transformer, recommended)"};
    int idx = g_set.preset == 10 ? 1 : g_set.preset == 11 ? 2 : 0;
    if (ImGui::Combo("Model preset", &idx, presets, 3)) {
        g_set.preset = idx == 1 ? 10 : idx == 2 ? 11 : 0;
        changed = true;
    }
    if (ImGui::TreeNode("Testing")) {
        changed |= ImGui::Checkbox("Flip jitter X", &g_set.flipJitterX);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Flip jitter Y", &g_set.flipJitterY);
        ImGui::TreePop();
    }
    if (changed) SaveSettings();
    std::string err, mode;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        err = g_lastError;
        mode = g_mode;
    }
    ImGui::TextDisabled("DLSS: %s", dlss::Status().c_str());
    if (g_set.enabled && !err.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", err.c_str());
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
    Log("DLSS %s at start (F9 toggles)", g_set.enabled ? "on" : "off");
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
}

}  // namespace rd
