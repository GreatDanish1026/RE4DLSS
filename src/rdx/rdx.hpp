// rdx: the D3D12 hook layer that replaces ReShade for RE4DLSS 2.0 (REFramework plugin, no ReShade).
//
// The add-on code (main.cpp, upscaler.cpp, framegen.cpp) is written against ReShade's add-on API.
// rdx serves that API itself:
//   - rdx_hooks.cpp: hooks the game's D3D12 calls (device, command lists, queues, the swapchain
//     and DXGI factory) from this DLL's DllMain, before the game creates its device (REFramework
//     loads plugins that early; see probe/FINDINGS.md), and fires ReShade's add-on events from them.
//     Events fire only for calls made by the game itself (return address in re4.exe): DLSS,
//     Streamline, FSR and overlays recording into the same lists stay invisible, as they were
//     to ReShade when they were handed native objects.
//   - rdx_track.cpp: what ReShade tracked for the API: resources (descriptions, GPU addresses,
//     destruction), descriptor heaps and what every descriptor holds, root signatures as
//     pipeline layouts, command signatures.
//   - rdx_api.cpp: ReShade's device / command_list / command_queue / swapchain objects on D3D12.
//   - rdx_events.cpp: the event registry, settings (RE4DLSS.ini), logging.
// With ReShade still installed (RenoDX), the objects the game holds are ReShade's: rdx hooks
// those, above ReShade, so it sees the game's own calls and shaders; ReShade and RenoDX see
// rdx's calls as the game's.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <reshade.hpp>

namespace rdx {
using namespace reshade::api;
using reshade::addon_event;

// ---------------------------------------------------------------- events (rdx_events.cpp)

constexpr int kMaxEvents = 128;
constexpr int kMaxCallbacks = 16;
extern std::atomic<void*> g_events[kMaxEvents][kMaxCallbacks];
extern std::atomic<bool> g_eventUsed[kMaxEvents];

inline bool Has(addon_event ev) { return g_eventUsed[static_cast<int>(ev)].load(std::memory_order_relaxed); }

// Calls every callback of <ev>; for events that can skip the call, true once one asks to skip
// (the rest aren't called, as in ReShade).
template <addon_event ev, typename... Args>
inline bool Invoke(Args&&... args) {
    using Traits = reshade::addon_event_traits<ev>;
    if (!Has(ev)) return false;
    for (auto& slot : g_events[static_cast<int>(ev)]) {
        void* p = slot.load(std::memory_order_acquire);
        if (!p) continue;
        auto fn = reinterpret_cast<typename Traits::decl>(p);
        if constexpr (std::is_same_v<typename Traits::type, bool>) {
            if (fn(args...)) return true;
        } else {
            fn(args...);
        }
    }
    return false;
}

void Log(const char* fmt, ...);
std::wstring ExeDirW();

// ---------------------------------------------------------------- private data (ReShade's api_object)

class PrivateData {
public:
    void Get(const uint8_t guid[16], uint64_t* data) const;
    void Set(const uint8_t guid[16], uint64_t data);

private:
    mutable std::mutex m_;
    std::vector<std::pair<GUID, uint64_t>> v_;
};

// ---------------------------------------------------------------- tracking (rdx_track.cpp)

// One descriptor slot: what the game (or we) last wrote there. Views keep the handle they were
// created at (origin), and copies carry it along, as ReShade reports views.
struct Slot {
    uint64_t origin = 0;           // view / sampler: CPU handle it was created at; 0 = nothing known
    ID3D12Resource* res = nullptr;  // view: the resource; constant buffer: the buffer
    uint64_t offset = 0, size = 0;  // constant buffer range
    descriptor_type type = descriptor_type::sampler;
    uint32_t format = 0;  // DXGI_FORMAT of the view
    uint8_t dim = 0;      // resource_view_type
    bool used = false;
};

struct HeapInfo {
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uint64_t cpu = 0, gpu = 0;  // starts (gpu 0: not shader visible)
    uint32_t count = 0, inc = 0;
    Slot* slots = nullptr;  // count entries
    bool ours = false;
};

// Descriptor heaps by handle (any heap: ours and the game's).
HeapInfo* HeapOfCpu(uint64_t cpu, uint32_t* index);
HeapInfo* HeapOfGpu(uint64_t gpu, uint32_t* index);
HeapInfo* HeapOfObject(ID3D12DescriptorHeap* heap);
void TrackHeap(ID3D12DescriptorHeap* heap, bool ours);

// Slot helpers (nullptr when the handle isn't in a tracked heap).
Slot* SlotAt(uint64_t cpu);
void CopySlots(uint64_t dst, uint64_t src, uint32_t count, uint32_t inc);

// The full description of views created straight into shader-visible heaps (they can't be
// copied from there): recreated from this when we need them in our own heap.
struct FullView {
    ID3D12Resource* res = nullptr;
    ID3D12Resource* counter = nullptr;
    int kind = 0;  // 1 SRV, 2 UAV, 3 CBV
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    bool hasDesc = false;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
};
void RememberFullView(uint64_t cpu, const FullView& v);
bool FullViewAt(uint64_t cpu, FullView* out);

// Resources.
struct ResInfo {
    resource_desc desc;
    uint64_t va = 0;  // buffers: GPU virtual address
};
void TrackResource(ID3D12Resource* r, bool fireEvents, D3D12_RESOURCE_STATES initial);
bool ResourceDesc(ID3D12Resource* r, resource_desc* out);  // tracked (alive) ones
bool IsDead(ID3D12Resource* r);                            // tracked once, since destroyed
void MarkAlive(ID3D12Resource* r);                         // known alive now (a swapchain buffer at a reused address)
ID3D12Resource* ResourceAtVa(uint64_t va, uint64_t* offset);
resource_desc DescOf(ID3D12Resource* r);  // tracked description, or asked of D3D12

// Root signatures as ReShade pipeline layouts.
struct LayoutInfo {
    std::vector<pipeline_layout_param> params;
    std::vector<std::vector<descriptor_range>> ranges;
    std::vector<D3D12_ROOT_PARAMETER_TYPE> rootTypes;  // per parameter
};
void TrackRootSignature(ID3D12RootSignature* rs, const void* blob, size_t size);
void TrackOwnLayout(ID3D12RootSignature* rs, uint32_t count, const pipeline_layout_param* params);
bool LayoutParam(ID3D12RootSignature* rs, uint32_t param, pipeline_layout_param* out);
bool RootParamType(ID3D12RootSignature* rs, uint32_t param, D3D12_ROOT_PARAMETER_TYPE* out);

void TrackCommandSignature(ID3D12CommandSignature* sig, const D3D12_COMMAND_SIGNATURE_DESC* desc);
indirect_command CommandSignatureType(ID3D12CommandSignature* sig);

// Destruction notices: D3D12 releases this when the object it is attached to goes away.
void OnDestroyed(IUnknown* object, void (*fn)(void* object, void* ctx), void* ctx);
void OnDestroyedDxgi(IDXGIObject* object, void (*fn)(void* object, void* ctx), void* ctx);

// ---------------------------------------------------------------- conversions (rdx_track.cpp)

D3D12_RESOURCE_STATES ToStates(resource_usage u);
resource_usage ToUsage(D3D12_RESOURCE_STATES s);
resource_desc ToDesc(const D3D12_RESOURCE_DESC& d, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags);
void FromDesc(const resource_desc& in, D3D12_RESOURCE_DESC* d, D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS* flags);
sampler_desc ToSamplerDesc(const D3D12_SAMPLER_DESC& d);
D3D12_SAMPLER_DESC FromSamplerDesc(const sampler_desc& d);
resource_view_desc ToViewDesc(const D3D12_SHADER_RESOURCE_VIEW_DESC& d);
resource_view_desc ToViewDesc(const D3D12_UNORDERED_ACCESS_VIEW_DESC& d);
resource_view_desc ToViewDesc(const D3D12_RENDER_TARGET_VIEW_DESC& d);
resource_view_desc ToViewDesc(const D3D12_DEPTH_STENCIL_VIEW_DESC& d);
resource_view_desc DefaultViewDesc(ID3D12Resource* r, int kind);  // views created with a null desc
shader_stage ToStages(D3D12_SHADER_VISIBILITY v);

// ---------------------------------------------------------------- API objects (rdx_api.cpp)

class Device;
class CommandList;
class Queue;
class Swapchain;

Device* GameDevice();
CommandList* ListOf(void* nativeList);  // nullptr: not a list the game created
Queue* QueueOf(void* nativeQueue);
Swapchain* SwapchainOf(void* nativeSwapchain);

class Device final : public device {
public:
    explicit Device(ID3D12Device* d);
    ID3D12Device* d3d() const { return d_; }

    // Our own descriptor heaps.
    bool AllocCpu(D3D12_DESCRIPTOR_HEAP_TYPE type, uint64_t* out);
    void FreeCpu(uint64_t handle);
    // Transient shader-visible descriptors (a ring): CPU and GPU handles of n in a row.
    bool AllocGpu(bool sampler, uint32_t n, uint64_t* cpu, uint64_t* gpu);
    ID3D12DescriptorHeap* GpuHeap(bool sampler) const { return sampler ? gpuSamplers_ : gpuViews_; }
    uint32_t Inc(D3D12_DESCRIPTOR_HEAP_TYPE t) const { return inc_[t]; }
    // Writes the descriptor <type> / <data> into the CPU handle (ours or the game's).
    void WriteDescriptor(uint64_t dst, descriptor_type type, const void* data, uint32_t k);

    uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(d_); }
    void get_private_data(const uint8_t guid[16], uint64_t* data) const override { pd_.Get(guid, data); }
    void set_private_data(const uint8_t guid[16], const uint64_t data) override { pd_.Set(guid, data); }
    device_api get_api() const override { return device_api::d3d12; }
    bool check_capability(device_caps) const override;
    bool check_format_support(format, resource_usage) const override;
    bool create_sampler(const sampler_desc& desc, sampler* out) override;
    void destroy_sampler(sampler s) override;
    bool create_resource(const resource_desc& desc, const subresource_data* initial, resource_usage state, resource* out,
                         void** shared = nullptr) override;
    void destroy_resource(resource r) override;
    resource_desc get_resource_desc(resource r) const override;
    bool create_resource_view(resource r, resource_usage usage, const resource_view_desc& desc, resource_view* out) override;
    void destroy_resource_view(resource_view v) override;
    resource get_resource_from_view(resource_view v) const override;
    resource_view_desc get_resource_view_desc(resource_view v) const override;
    bool map_buffer_region(resource r, uint64_t offset, uint64_t size, map_access access, void** out) override;
    void unmap_buffer_region(resource r) override;
    bool map_texture_region(resource, uint32_t, const subresource_box*, map_access, subresource_data*) override;
    void unmap_texture_region(resource, uint32_t) override;
    void update_buffer_region(const void*, resource, uint64_t, uint64_t) override;
    void update_texture_region(const subresource_data&, resource, uint32_t, const subresource_box*) override;
    bool create_pipeline(pipeline_layout layout, uint32_t count, const pipeline_subobject* subs, pipeline* out) override;
    void destroy_pipeline(pipeline p) override;
    bool create_pipeline_layout(uint32_t count, const pipeline_layout_param* params, pipeline_layout* out) override;
    void destroy_pipeline_layout(pipeline_layout l) override;
    bool allocate_descriptor_tables(uint32_t, pipeline_layout, uint32_t, descriptor_table*) override;
    void free_descriptor_tables(uint32_t, const descriptor_table*) override;
    void get_descriptor_heap_offset(descriptor_table table, uint32_t binding, uint32_t array_offset, descriptor_heap* heap,
                                    uint32_t* offset) const override;
    void copy_descriptor_tables(uint32_t count, const descriptor_table_copy* copies) override;
    void update_descriptor_tables(uint32_t count, const descriptor_table_update* updates) override;
    bool create_query_heap(query_type, uint32_t, query_heap*) override;
    void destroy_query_heap(query_heap) override;
    bool get_query_heap_results(query_heap, query_type, uint32_t, uint32_t, void*, uint32_t) override;
    void set_resource_name(resource r, const char* name) override;
    void set_resource_view_name(resource_view, const char*) override;
    bool create_fence(uint64_t, fence_flags, fence*, void**) override;
    void destroy_fence(fence) override;
    uint64_t get_completed_fence_value(fence) const override;
    bool wait(fence, uint64_t, uint64_t) override;
    bool signal(fence, uint64_t) override;
    bool get_property(device_properties, void*) const override;
    uint64_t get_resource_view_gpu_address(resource_view) const override;
    void get_acceleration_structure_size(acceleration_structure_type, acceleration_structure_build_flags, uint32_t,
                                         const acceleration_structure_build_input*, uint64_t*, uint64_t*, uint64_t*) const override;
    bool get_pipeline_shader_group_handles(pipeline, uint32_t, uint32_t, void*) override;

private:
    bool InitHeaps();
    ID3D12Device* d_;
    PrivateData pd_;
    uint32_t inc_[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES] = {};
    std::mutex heapMutex_;
    struct CpuHeap {
        ID3D12DescriptorHeap* heap = nullptr;
        uint64_t start = 0;
        uint32_t count = 0;
        std::vector<uint32_t> free;
    };
    CpuHeap cpu_[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];
    ID3D12DescriptorHeap* gpuViews_ = nullptr;
    ID3D12DescriptorHeap* gpuSamplers_ = nullptr;
    uint32_t gpuViewNext_ = 0, gpuSamplerNext_ = 0;
    static constexpr uint32_t kGpuViews = 65536, kGpuSamplers = 2048;
    bool heapsOk_ = false, heapsTried_ = false;
};

// The state rdx knows a command list is in (for its own commands and for putting the game's back).
struct ListActual {
    ID3D12RootSignature* root[2] = {};  // graphics, compute: as last set by anyone
    ID3D12DescriptorHeap* heaps[2] = {};
    uint32_t heapCount = 0;
    ID3D12DescriptorHeap* gameHeaps[2] = {};  // as last set by the game
    uint32_t gameHeapCount = 0;
};

class CommandList final : public command_list {
public:
    CommandList(Device* dev, ID3D12GraphicsCommandList* l);
    ID3D12GraphicsCommandList* d3d() const { return l_; }
    ListActual act;

    uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(l_); }
    void get_private_data(const uint8_t guid[16], uint64_t* data) const override { pd_.Get(guid, data); }
    void set_private_data(const uint8_t guid[16], const uint64_t data) override { pd_.Set(guid, data); }
    device* get_device() override { return dev_; }
    void barrier(uint32_t count, const resource* resources, const resource_usage* old_states, const resource_usage* new_states) override;
    void end_render_pass() override {}
    void bind_render_targets_and_depth_stencil(uint32_t count, const resource_view* rtvs, resource_view dsv) override;
    void bind_pipeline(pipeline_stage stages, pipeline p) override;
    void bind_pipeline_states(uint32_t count, const dynamic_state* states, const uint32_t* values) override;
    void bind_viewports(uint32_t first, uint32_t count, const viewport* vps) override;
    void bind_scissor_rects(uint32_t first, uint32_t count, const rect* rects) override;
    void push_constants(shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first, uint32_t count,
                        const void* values) override;
    void push_descriptors(shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update& update) override;
    void bind_index_buffer(resource, uint64_t, uint32_t) override;
    void bind_vertex_buffers(uint32_t, uint32_t, const resource*, const uint64_t*, const uint32_t*) override;
    void bind_stream_output_buffers(uint32_t, uint32_t, const resource*, const uint64_t*, const uint64_t*, const resource*,
                                    const uint64_t*) override;
    void draw(uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance) override;
    void draw_indexed(uint32_t indices, uint32_t instances, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) override;
    void dispatch(uint32_t x, uint32_t y, uint32_t z) override;
    void draw_or_dispatch_indirect(indirect_command, resource, uint64_t, uint32_t, uint32_t) override;
    void copy_resource(resource src, resource dst) override;
    void copy_buffer_region(resource src, uint64_t src_offset, resource dst, uint64_t dst_offset, uint64_t size) override;
    void copy_buffer_to_texture(resource, uint64_t, uint32_t, uint32_t, resource, uint32_t, const subresource_box*) override;
    void copy_texture_region(resource src, uint32_t src_sub, const subresource_box* src_box, resource dst, uint32_t dst_sub,
                             const subresource_box* dst_box, filter_mode filter) override;
    void copy_texture_to_buffer(resource src, uint32_t src_sub, const subresource_box* src_box, resource dst, uint64_t dst_offset,
                                uint32_t row_length, uint32_t slice_height) override;
    void resolve_texture_region(resource, uint32_t, const subresource_box*, resource, uint32_t, uint32_t, uint32_t, uint32_t,
                                format) override;
    void clear_depth_stencil_view(resource_view, const float*, const uint8_t*, uint32_t, const rect*) override;
    void clear_render_target_view(resource_view rtv, const float color[4], uint32_t, const rect*) override;
    void clear_unordered_access_view_uint(resource_view, const uint32_t[4], uint32_t, const rect*) override;
    void clear_unordered_access_view_float(resource_view, const float[4], uint32_t, const rect*) override;
    void generate_mipmaps(resource_view) override;
    void begin_query(query_heap, query_type, uint32_t) override;
    void end_query(query_heap, query_type, uint32_t) override;
    void copy_query_heap_results(query_heap, query_type, uint32_t, uint32_t, resource, uint64_t, uint32_t) override;
    void begin_debug_event(const char*, const float[4]) override {}
    void end_debug_event() override {}
    void insert_debug_marker(const char*, const float[4]) override {}
    void dispatch_mesh(uint32_t, uint32_t, uint32_t) override;
    void dispatch_rays(resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t,
                       resource, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t) override;
    void copy_acceleration_structure(resource_view, resource_view, acceleration_structure_copy_mode) override;
    void build_acceleration_structure(acceleration_structure_type, acceleration_structure_build_flags, uint32_t,
                                      const acceleration_structure_build_input*, resource, uint64_t, resource_view, resource_view,
                                      acceleration_structure_build_mode) override;
    void query_acceleration_structures(uint32_t, const resource_view*, query_heap, query_type, uint32_t) override;
    void update_buffer_region(const void*, resource, uint64_t, uint64_t) override;
    void update_texture_region(const subresource_data&, resource, uint32_t, const subresource_box*) override;
    void begin_render_pass2(uint32_t, const render_pass_render_target_desc*, const render_pass_depth_stencil_desc*, render_pass_flags) override;
    void bind_descriptor_tables2(shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count, const descriptor_table* tables,
                                 uint32_t, const uint32_t*) override;

private:
    void SetRoot(int which, ID3D12RootSignature* rs);
    void SetHeaps(uint32_t n, ID3D12DescriptorHeap* const* heaps);
    Device* dev_;
    ID3D12GraphicsCommandList* l_;
    PrivateData pd_;
};

class Queue final : public command_queue {
public:
    Queue(Device* dev, ID3D12CommandQueue* q, D3D12_COMMAND_LIST_TYPE type) : dev_(dev), q_(q), type_(type) {}
    ID3D12CommandQueue* d3d() const { return q_; }
    uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(q_); }
    void get_private_data(const uint8_t guid[16], uint64_t* data) const override { pd_.Get(guid, data); }
    void set_private_data(const uint8_t guid[16], const uint64_t data) override { pd_.Set(guid, data); }
    device* get_device() override { return dev_; }
    command_queue_type get_type() const override;
    void wait_idle() const override;
    void flush_immediate_command_list() const override {}
    command_list* get_immediate_command_list() override { return nullptr; }
    void begin_debug_event(const char*, const float[4]) override {}
    void end_debug_event() override {}
    void insert_debug_marker(const char*, const float[4]) override {}
    bool wait(fence, uint64_t) override { return false; }
    bool signal(fence, uint64_t) override { return false; }
    uint64_t get_timestamp_frequency() const override;

private:
    Device* dev_;
    ID3D12CommandQueue* q_;
    D3D12_COMMAND_LIST_TYPE type_;
    PrivateData pd_;
};

class Swapchain final : public swapchain {
public:
    Swapchain(Device* dev, IDXGISwapChain* sc, Queue* queue, HWND wnd) : dev_(dev), sc_(sc), queue_(queue), wnd_(wnd) {}
    IDXGISwapChain* dxgi() const { return sc_; }
    Queue* queue() const { return queue_; }
    std::atomic<color_space> colorSpace{color_space::unknown};

    uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(sc_); }
    void get_private_data(const uint8_t guid[16], uint64_t* data) const override { pd_.Get(guid, data); }
    void set_private_data(const uint8_t guid[16], const uint64_t data) override { pd_.Set(guid, data); }
    device* get_device() override { return dev_; }
    void* get_hwnd() const override { return wnd_; }
    resource get_back_buffer(uint32_t index) override;
    uint32_t get_back_buffer_count() const override;
    uint32_t get_current_back_buffer_index() const override;
    bool check_color_space_support(color_space cs) const override;
    color_space get_color_space() const override;

private:
    Device* dev_;
    IDXGISwapChain* sc_;
    Queue* queue_;
    HWND wnd_;
    PrivateData pd_;
};

// Called by the hooks.
Device* CreateDeviceWrapper(ID3D12Device* d);
CommandList* CreateListWrapper(ID3D12GraphicsCommandList* l);
Queue* CreateQueueWrapper(ID3D12CommandQueue* q, D3D12_COMMAND_LIST_TYPE type);
Swapchain* CreateSwapchainWrapper(IDXGISwapChain* sc, IUnknown* queueOrDevice, HWND wnd);

// ---------------------------------------------------------------- hooks (rdx_hooks.cpp)

// From DllMain: the game's GetProcAddress (D3D12CreateDevice) and DXGI factory creation.
void InstallEarly(HMODULE self);
void Uninstall();

// rdx's own commands go through the hooked methods like anyone's: calls not made by the game fire
// no events, but still update ListActual (root signatures, descriptor heaps).
// Framegen's swapchain creation goes through rdx's hook of the DXGI factory: it creates the game's
// swapchain itself (through Streamline's or FSR's proxy) when set.
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
// Sets the interceptors; *origX get the functions that create a swapchain without them.
void SetSwapchainCreators(CreateSwapChainFn create, CreateSwapChainForHwndFn createForHwnd, CreateSwapChainFn* origCreate,
                          CreateSwapChainForHwndFn* origCreateForHwnd);

}  // namespace rdx
