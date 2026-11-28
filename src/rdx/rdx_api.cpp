// rdx: ReShade's API objects (device, command_list, command_queue, swapchain) on D3D12, the parts
// the add-on uses. Handles are as ReShade's on D3D12: resources, pipelines and root signatures are
// the D3D12 object pointers, views and samplers CPU descriptor handles, descriptor tables GPU
// descriptor handles, descriptor heaps the heap objects.

#include "rdx.hpp"

#include <algorithm>

#include "descriptor_tracking.hpp"

namespace rdx {

namespace {

Device* g_device = nullptr;

// Wrappers by native object: a fixed open-addressing table read without locks (objects are few,
// lookups are on every hooked call). Entries are never removed: a destroyed object's entry is
// cleared and taken again if D3D12 reuses its address. Wrappers aren't freed (a hook may still
// hold one; they're small).
template <typename T, size_t N>
class Table {
public:
    T* Get(void* key) const {
        size_t i = Hash(key);
        for (size_t n = 0; n < N; ++n, i = (i + 1) % N) {
            void* k = keys_[i].load(std::memory_order_acquire);
            if (k == key) return vals_[i].load(std::memory_order_acquire);
            if (!k) return nullptr;
        }
        return nullptr;
    }
    void Put(void* key, T* val) {
        std::lock_guard<std::mutex> lock(m_);
        size_t i = Hash(key);
        for (size_t n = 0; n < N; ++n, i = (i + 1) % N) {
            void* k = keys_[i].load(std::memory_order_relaxed);
            if (k == key || !k) {
                vals_[i].store(val, std::memory_order_release);
                keys_[i].store(key, std::memory_order_release);
                return;
            }
        }
        Log("wrapper table full");
    }

private:
    static size_t Hash(void* p) { return (reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull % N; }
    std::atomic<void*> keys_[N] = {};
    std::atomic<T*> vals_[N] = {};
    std::mutex m_;
};

Table<CommandList, 8192> g_lists;
Table<Queue, 64> g_queues;
Table<Swapchain, 64> g_swapchains;

uint32_t BytesPerPixel(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT: return 16;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT: return 8;
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_D16_UNORM:
            return f == DXGI_FORMAT_R16G16_TYPELESS || f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R16G16_UNORM ? 4 : 2;
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT: return 1;
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM: return 2;
        default: return 4;
    }
}

D3D12_SHADER_VISIBILITY Visibility(shader_stage s) {
    if (s == shader_stage::pixel) return D3D12_SHADER_VISIBILITY_PIXEL;
    if (s == shader_stage::vertex) return D3D12_SHADER_VISIBILITY_VERTEX;
    if (s == shader_stage::geometry) return D3D12_SHADER_VISIBILITY_GEOMETRY;
    return D3D12_SHADER_VISIBILITY_ALL;
}

D3D12_DESCRIPTOR_RANGE_TYPE RangeType(descriptor_type t) {
    switch (t) {
        case descriptor_type::sampler: return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        case descriptor_type::constant_buffer: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        case descriptor_type::unordered_access_view:
        case descriptor_type::buffer_unordered_access_view: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        default: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    }
}

bool IsSamplerType(descriptor_type t) { return t == descriptor_type::sampler; }

}  // namespace

Device* GameDevice() { return g_device; }
CommandList* ListOf(void* native) { return native ? g_lists.Get(native) : nullptr; }
Queue* QueueOf(void* native) { return native ? g_queues.Get(native) : nullptr; }
Swapchain* SwapchainOf(void* native) { return native ? g_swapchains.Get(native) : nullptr; }

Device* CreateDeviceWrapper(ID3D12Device* d) {
    if (!g_device) {
        g_device = new Device(d);
        // ReShade's descriptor_tracking utility attaches itself to every device; the add-on asks
        // the device for it (stateless here: rdx tracks the descriptors).
        g_device->create_private_data<descriptor_tracking>();
    }
    return g_device;
}

CommandList* CreateListWrapper(ID3D12GraphicsCommandList* l) {
    if (!g_device || !l) return nullptr;
    CommandList* w = new CommandList(g_device, l);
    g_lists.Put(l, w);
    return w;
}

Queue* CreateQueueWrapper(ID3D12CommandQueue* q, D3D12_COMMAND_LIST_TYPE type) {
    if (!g_device || !q) return nullptr;
    Queue* w = new Queue(g_device, q, type);
    g_queues.Put(q, w);
    return w;
}

Swapchain* CreateSwapchainWrapper(IDXGISwapChain* sc, IUnknown* queueOrDevice, HWND wnd) {
    if (!g_device || !sc) return nullptr;
    Swapchain* w = new Swapchain(g_device, sc, QueueOf(queueOrDevice), wnd);
    g_swapchains.Put(sc, w);
    return w;
}

// ---------------------------------------------------------------- device

Device::Device(ID3D12Device* d) : d_(d) {
    for (int t = 0; t < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++t)
        inc_[t] = d_->GetDescriptorHandleIncrementSize(static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(t));
}

bool Device::InitHeaps() {
    if (heapsTried_) return heapsOk_;
    heapsTried_ = true;
    const uint32_t counts[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES] = {4096, 512, 512, 128};
    for (int t = 0; t < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++t) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(t);
        hd.NumDescriptors = counts[t];
        if (FAILED(d_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cpu_[t].heap)))) {
            Log("couldn't create a descriptor heap (type %d)", t);
            return false;
        }
        cpu_[t].start = cpu_[t].heap->GetCPUDescriptorHandleForHeapStart().ptr;
        cpu_[t].count = counts[t];
        for (uint32_t i = counts[t]; i-- > 0;) cpu_[t].free.push_back(i);
        TrackHeap(cpu_[t].heap, true);
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kGpuViews;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(d_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpuViews_)))) return false;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    hd.NumDescriptors = kGpuSamplers;
    if (FAILED(d_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpuSamplers_)))) return false;
    TrackHeap(gpuViews_, true);
    TrackHeap(gpuSamplers_, true);
    heapsOk_ = true;
    return true;
}

bool Device::AllocCpu(D3D12_DESCRIPTOR_HEAP_TYPE type, uint64_t* out) {
    std::lock_guard<std::mutex> lock(heapMutex_);
    if (!InitHeaps()) return false;
    CpuHeap& h = cpu_[type];
    if (h.free.empty()) {
        Log("out of descriptors (heap type %d)", type);
        return false;
    }
    *out = h.start + uint64_t(h.free.back()) * inc_[type];
    h.free.pop_back();
    return true;
}

void Device::FreeCpu(uint64_t handle) {
    std::lock_guard<std::mutex> lock(heapMutex_);
    for (int t = 0; t < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; ++t) {
        CpuHeap& h = cpu_[t];
        if (!h.heap || handle < h.start || handle >= h.start + uint64_t(h.count) * inc_[t]) continue;
        h.free.push_back(static_cast<uint32_t>((handle - h.start) / inc_[t]));
        if (Slot* s = SlotAt(handle)) *s = Slot();
        return;
    }
}

bool Device::AllocGpu(bool sampler, uint32_t n, uint64_t* cpu, uint64_t* gpu) {
    std::lock_guard<std::mutex> lock(heapMutex_);
    if (!InitHeaps()) return false;
    ID3D12DescriptorHeap* h = sampler ? gpuSamplers_ : gpuViews_;
    uint32_t& next = sampler ? gpuSamplerNext_ : gpuViewNext_;
    const uint32_t cap = sampler ? kGpuSamplers : kGpuViews;
    if (n == 0 || n > cap) return false;
    if (next + n > cap) next = 0;  // a ring: thousands of frames before a slot comes round again
    const uint32_t inc = inc_[sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
    *cpu = h->GetCPUDescriptorHandleForHeapStart().ptr + uint64_t(next) * inc;
    *gpu = h->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(next) * inc;
    next += n;
    return true;
}

void Device::WriteDescriptor(uint64_t dst, descriptor_type type, const void* data, uint32_t k) {
    Slot* ds = SlotAt(dst);
    if (type == descriptor_type::sampler) {
        const uint64_t src = static_cast<const sampler*>(data)[k].handle;
        if (!src) return;
        d_->CopyDescriptorsSimple(1, D3D12_CPU_DESCRIPTOR_HANDLE{dst}, D3D12_CPU_DESCRIPTOR_HANDLE{src}, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        CopySlots(dst, src, 1, 0);
        return;
    }
    if (type == descriptor_type::constant_buffer) {
        const buffer_range& b = static_cast<const buffer_range*>(data)[k];
        ID3D12Resource* r = reinterpret_cast<ID3D12Resource*>(b.buffer.handle);
        if (!r || IsDead(r)) return;
        const resource_desc rd = DescOf(r);
        uint64_t size = b.size == UINT64_MAX || b.size == 0 ? rd.buffer.size - b.offset : b.size;
        size = (size + 255) & ~255ull;
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {r->GetGPUVirtualAddress() + b.offset, static_cast<UINT>(size)};
        d_->CreateConstantBufferView(&cbv, D3D12_CPU_DESCRIPTOR_HANDLE{dst});
        if (ds) {
            *ds = Slot();
            ds->used = true;
            ds->type = descriptor_type::constant_buffer;
            ds->res = r;
            ds->offset = b.offset;
            ds->size = size;
        }
        return;
    }
    // A view: copied from where it was created (non-shader-visible heaps), or made again from its
    // description (views created straight into a shader-visible heap can't be copied from there).
    const uint64_t src = static_cast<const resource_view*>(data)[k].handle;
    if (!src) return;
    uint32_t si = 0;
    HeapInfo* sh = HeapOfCpu(src, &si);
    if (sh && !sh->gpu) {
        d_->CopyDescriptorsSimple(1, D3D12_CPU_DESCRIPTOR_HANDLE{dst}, D3D12_CPU_DESCRIPTOR_HANDLE{src}, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CopySlots(dst, src, 1, 0);
        return;
    }
    FullView fv;
    if (FullViewAt(src, &fv) && fv.res && !IsDead(fv.res)) {
        if (fv.kind == 1) d_->CreateShaderResourceView(fv.res, fv.hasDesc ? &fv.srv : nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{dst});
        else if (fv.kind == 2) d_->CreateUnorderedAccessView(fv.res, fv.counter, fv.hasDesc ? &fv.uav : nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{dst});
        CopySlots(dst, src, 1, 0);
        return;
    }
    static std::atomic<int> warned{0};
    if (warned++ < 5) Log("a view to copy (%llx) isn't known: left as it was", static_cast<unsigned long long>(src));
}

bool Device::check_capability(device_caps) const { return true; }
bool Device::check_format_support(format, resource_usage) const { return true; }

bool Device::create_sampler(const sampler_desc& desc, sampler* out) {
    uint64_t h = 0;
    if (!AllocCpu(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, &h)) return false;
    const D3D12_SAMPLER_DESC d = FromSamplerDesc(desc);
    d_->CreateSampler(&d, D3D12_CPU_DESCRIPTOR_HANDLE{h});
    if (Slot* s = SlotAt(h)) {
        *s = Slot();
        s->used = true;
        s->type = descriptor_type::sampler;
        s->origin = h;
    }
    *out = sampler{h};
    return true;
}

void Device::destroy_sampler(sampler s) {
    if (s.handle) FreeCpu(s.handle);
}

bool Device::create_resource(const resource_desc& desc, const subresource_data*, resource_usage state, resource* out, void**) {
    D3D12_RESOURCE_DESC d;
    D3D12_HEAP_PROPERTIES heap;
    D3D12_HEAP_FLAGS flags;
    FromDesc(desc, &d, &heap, &flags);
    D3D12_RESOURCE_STATES st = ToStates(state);
    if (heap.Type == D3D12_HEAP_TYPE_UPLOAD) st = D3D12_RESOURCE_STATE_GENERIC_READ;
    if (heap.Type == D3D12_HEAP_TYPE_READBACK) st = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12Resource* r = nullptr;
    const HRESULT hr = d_->CreateCommittedResource(&heap, flags, &d, st, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) {
        Log("CreateCommittedResource failed: 0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }
    TrackResource(r, false, st);
    *out = resource{reinterpret_cast<uintptr_t>(r)};
    return true;
}

void Device::destroy_resource(resource r) {
    if (r.handle) reinterpret_cast<ID3D12Resource*>(r.handle)->Release();
}

resource_desc Device::get_resource_desc(resource r) const { return DescOf(reinterpret_cast<ID3D12Resource*>(r.handle)); }

bool Device::create_resource_view(resource r, resource_usage usage, const resource_view_desc& desc, resource_view* out) {
    ID3D12Resource* res = reinterpret_cast<ID3D12Resource*>(r.handle);
    if (!res) return false;
    const resource_desc rd = DescOf(res);
    resource_view_desc v = desc;
    if (v.type == resource_view_type::unknown) {
        const resource_view_desc def = DefaultViewDesc(res, 0);
        v.type = def.type;
        if (v.type != resource_view_type::buffer) v.texture = def.texture;
    }
    DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(v.format == format::unknown && rd.type != resource_type::buffer ? rd.texture.format : v.format);
    const bool array = v.type == resource_view_type::texture_2d_array;
    const uint32_t mips = v.texture.levels == UINT32_MAX ? UINT32_MAX : v.texture.levels;
    const uint32_t layers = v.texture.layers == UINT32_MAX ? (rd.texture.depth_or_layers - v.texture.first_layer) : v.texture.layers;
    uint64_t h = 0;
    Slot slot;
    slot.used = true;
    slot.res = res;
    slot.format = fmt;
    slot.dim = static_cast<uint8_t>(v.type);
    if (usage == resource_usage::render_target) {
        if (!AllocCpu(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, &h)) return false;
        D3D12_RENDER_TARGET_VIEW_DESC d = {};
        d.Format = fmt;
        if (array) {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {v.texture.first_level, v.texture.first_layer, layers, 0};
        } else {
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipSlice = v.texture.first_level;
        }
        d_->CreateRenderTargetView(res, &d, D3D12_CPU_DESCRIPTOR_HANDLE{h});
        slot.type = descriptor_type::shader_resource_view;
    } else if (usage == resource_usage::depth_stencil || usage == resource_usage::depth_stencil_write ||
               usage == resource_usage::depth_stencil_read) {
        if (!AllocCpu(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, &h)) return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC d = {};
        d.Format = fmt;
        d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = v.texture.first_level;
        d_->CreateDepthStencilView(res, &d, D3D12_CPU_DESCRIPTOR_HANDLE{h});
        slot.type = descriptor_type::shader_resource_view;
    } else if (usage == resource_usage::unordered_access) {
        if (!AllocCpu(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, &h)) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.Format = fmt;
        if (v.type == resource_view_type::buffer) {
            d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            d.Buffer.FirstElement = v.buffer.offset;
            d.Buffer.NumElements = static_cast<UINT>(v.buffer.size);
            slot.type = descriptor_type::buffer_unordered_access_view;
        } else if (array) {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {v.texture.first_level, v.texture.first_layer, layers, 0};
            slot.type = descriptor_type::unordered_access_view;
        } else {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipSlice = v.texture.first_level;
            slot.type = descriptor_type::unordered_access_view;
        }
        d_->CreateUnorderedAccessView(res, nullptr, &d, D3D12_CPU_DESCRIPTOR_HANDLE{h});
    } else {
        if (!AllocCpu(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, &h)) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = fmt;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (v.type == resource_view_type::buffer) {
            d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Buffer.FirstElement = v.buffer.offset;
            d.Buffer.NumElements = static_cast<UINT>(v.buffer.size);
            slot.type = descriptor_type::buffer_shader_resource_view;
        } else if (array) {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray = {v.texture.first_level, mips, v.texture.first_layer, layers, 0, 0.0f};
            slot.type = descriptor_type::shader_resource_view;
        } else {
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D = {v.texture.first_level, mips, 0, 0.0f};
            slot.type = descriptor_type::shader_resource_view;
        }
        d_->CreateShaderResourceView(res, &d, D3D12_CPU_DESCRIPTOR_HANDLE{h});
    }
    slot.origin = h;
    if (Slot* s = SlotAt(h)) *s = slot;
    *out = resource_view{h};
    return true;
}

void Device::destroy_resource_view(resource_view v) {
    if (v.handle) FreeCpu(v.handle);
}

resource Device::get_resource_from_view(resource_view v) const {
    const Slot* s = SlotAt(v.handle);
    if (!s || !s->used || !s->res || IsDead(s->res)) return resource{0};
    return resource{reinterpret_cast<uintptr_t>(s->res)};
}

resource_view_desc Device::get_resource_view_desc(resource_view v) const {
    const Slot* s = SlotAt(v.handle);
    if (!s || !s->used) return resource_view_desc();
    resource_view_desc d;
    d.type = static_cast<resource_view_type>(s->dim);
    d.format = static_cast<format>(s->format);
    return d;
}

bool Device::map_buffer_region(resource r, uint64_t offset, uint64_t size, map_access access, void** out) {
    ID3D12Resource* res = reinterpret_cast<ID3D12Resource*>(r.handle);
    if (!res || !out) return false;
    *out = nullptr;
    D3D12_RANGE read = {0, 0};
    if (access == map_access::read_only || access == map_access::read_write)
        read = {static_cast<SIZE_T>(offset), size == UINT64_MAX ? static_cast<SIZE_T>(DescOf(res).buffer.size) : static_cast<SIZE_T>(offset + size)};
    void* p = nullptr;
    if (FAILED(res->Map(0, &read, &p)) || !p) return false;
    *out = static_cast<uint8_t*>(p) + offset;
    return true;
}

void Device::unmap_buffer_region(resource r) {
    if (r.handle) reinterpret_cast<ID3D12Resource*>(r.handle)->Unmap(0, nullptr);
}

bool Device::map_texture_region(resource, uint32_t, const subresource_box*, map_access, subresource_data*) { return false; }
void Device::unmap_texture_region(resource, uint32_t) {}
void Device::update_buffer_region(const void*, resource, uint64_t, uint64_t) { Log("update_buffer_region isn't implemented"); }
void Device::update_texture_region(const subresource_data&, resource, uint32_t, const subresource_box*) {
    Log("update_texture_region isn't implemented");
}

bool Device::create_pipeline(pipeline_layout layout, uint32_t count, const pipeline_subobject* subs, pipeline* out) {
    ID3D12RootSignature* rs = reinterpret_cast<ID3D12RootSignature*>(layout.handle);
    const shader_desc *vs = nullptr, *ps = nullptr, *cs = nullptr;
    const rasterizer_desc* rast = nullptr;
    const depth_stencil_desc* depth = nullptr;
    const primitive_topology* topo = nullptr;
    const format* rtFormats = nullptr;
    uint32_t rtCount = 0;
    for (uint32_t i = 0; i < count; ++i) {
        switch (subs[i].type) {
            case pipeline_subobject_type::vertex_shader: vs = static_cast<const shader_desc*>(subs[i].data); break;
            case pipeline_subobject_type::pixel_shader: ps = static_cast<const shader_desc*>(subs[i].data); break;
            case pipeline_subobject_type::compute_shader: cs = static_cast<const shader_desc*>(subs[i].data); break;
            case pipeline_subobject_type::rasterizer_state: rast = static_cast<const rasterizer_desc*>(subs[i].data); break;
            case pipeline_subobject_type::depth_stencil_state: depth = static_cast<const depth_stencil_desc*>(subs[i].data); break;
            case pipeline_subobject_type::primitive_topology: topo = static_cast<const primitive_topology*>(subs[i].data); break;
            case pipeline_subobject_type::render_target_formats:
                rtFormats = static_cast<const format*>(subs[i].data);
                rtCount = subs[i].count;
                break;
            default: break;
        }
    }
    ID3D12PipelineState* p = nullptr;
    HRESULT hr;
    if (cs) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = rs;
        d.CS = {cs->code, cs->code_size};
        hr = d_->CreateComputePipelineState(&d, IID_PPV_ARGS(&p));
    } else {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = rs;
        if (vs) d.VS = {vs->code, vs->code_size};
        if (ps) d.PS = {ps->code, ps->code_size};
        for (auto& rt : d.BlendState.RenderTarget) {
            rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
            rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.LogicOp = D3D12_LOGIC_OP_NOOP;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = rast ? static_cast<D3D12_CULL_MODE>(static_cast<uint32_t>(rast->cull_mode) + 1) : D3D12_CULL_MODE_BACK;
        d.RasterizerState.FrontCounterClockwise = rast && rast->front_counter_clockwise;
        d.RasterizerState.DepthClipEnable = !rast || rast->depth_clip_enable;
        if (depth) {
            d.DepthStencilState.DepthEnable = depth->depth_enable;
            d.DepthStencilState.DepthWriteMask = depth->depth_write_mask ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            d.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(depth->depth_func);
            d.DepthStencilState.StencilEnable = depth->stencil_enable;
        } else {
            d.DepthStencilState.DepthEnable = TRUE;
            d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        }
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        if (topo && (*topo == primitive_topology::point_list)) d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        else if (topo && (*topo == primitive_topology::line_list || *topo == primitive_topology::line_strip))
            d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        d.NumRenderTargets = std::min<uint32_t>(rtCount, 8);
        for (uint32_t i = 0; i < d.NumRenderTargets; ++i) d.RTVFormats[i] = static_cast<DXGI_FORMAT>(rtFormats[i]);
        d.SampleDesc.Count = 1;
        hr = d_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p));
    }
    if (FAILED(hr)) {
        Log("pipeline creation failed: 0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }
    *out = pipeline{reinterpret_cast<uintptr_t>(p)};
    return true;
}

void Device::destroy_pipeline(pipeline p) {
    if (p.handle) reinterpret_cast<ID3D12PipelineState*>(p.handle)->Release();
}

bool Device::create_pipeline_layout(uint32_t count, const pipeline_layout_param* params, pipeline_layout* out) {
    std::vector<D3D12_ROOT_PARAMETER> rp(count);
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE>> ranges(count);
    for (uint32_t i = 0; i < count; ++i) {
        const pipeline_layout_param& p = params[i];
        D3D12_ROOT_PARAMETER& r = rp[i];
        if (p.type == pipeline_layout_param_type::push_constants) {
            r.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            r.Constants = {p.push_constants.dx_register_index, p.push_constants.dx_register_space, p.push_constants.count};
            r.ShaderVisibility = Visibility(p.push_constants.visibility);
            continue;
        }
        std::vector<descriptor_range> src;
        if (p.type == pipeline_layout_param_type::push_descriptors) {
            src.push_back(p.push_descriptors);
        } else if (p.type == pipeline_layout_param_type::descriptor_table || p.type == pipeline_layout_param_type::push_descriptors_with_ranges) {
            src.assign(p.descriptor_table.ranges, p.descriptor_table.ranges + p.descriptor_table.count);
        } else {
            for (uint32_t k = 0; k < p.descriptor_table_with_flags.count; ++k) src.push_back(p.descriptor_table_with_flags.ranges[k]);
        }
        shader_stage vis = shader_stage::all;
        for (const descriptor_range& s : src) {
            D3D12_DESCRIPTOR_RANGE d = {};
            d.RangeType = RangeType(s.type);
            d.NumDescriptors = s.count;
            d.BaseShaderRegister = s.dx_register_index;
            d.RegisterSpace = s.dx_register_space;
            d.OffsetInDescriptorsFromTableStart = s.binding;
            ranges[i].push_back(d);
            vis = s.visibility;
        }
        r.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        r.DescriptorTable = {static_cast<UINT>(ranges[i].size()), ranges[i].data()};
        r.ShaderVisibility = Visibility(vis);
    }
    D3D12_ROOT_SIGNATURE_DESC desc = {count, rp.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    using SerializeFn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    static SerializeFn serialize = reinterpret_cast<SerializeFn>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
    if (!serialize) return false;
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        Log("root signature serialization failed: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        return false;
    }
    ID3D12RootSignature* rs = nullptr;
    const HRESULT hr = d_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    blob->Release();
    if (FAILED(hr)) return false;
    TrackOwnLayout(rs, count, params);
    *out = pipeline_layout{reinterpret_cast<uintptr_t>(rs)};
    return true;
}

void Device::destroy_pipeline_layout(pipeline_layout l) {
    if (l.handle) reinterpret_cast<ID3D12RootSignature*>(l.handle)->Release();
}

bool Device::allocate_descriptor_tables(uint32_t, pipeline_layout, uint32_t, descriptor_table*) { return false; }
void Device::free_descriptor_tables(uint32_t, const descriptor_table*) {}

void Device::get_descriptor_heap_offset(descriptor_table table, uint32_t binding, uint32_t array_offset, descriptor_heap* heap,
                                        uint32_t* offset) const {
    uint32_t i = 0;
    HeapInfo* h = HeapOfGpu(table.handle, &i);
    if (!h) h = HeapOfCpu(table.handle, &i);
    if (heap) *heap = descriptor_heap{h ? reinterpret_cast<uintptr_t>(h->heap) : 0};
    if (offset) *offset = h ? i + binding + array_offset : 0;
}

namespace {
// The CPU handle of descriptor <index> of a table (a GPU handle, or a CPU one).
uint64_t TableCpu(descriptor_table table, uint32_t index) {
    uint32_t i = 0;
    if (HeapInfo* h = HeapOfGpu(table.handle, &i)) return h->cpu + uint64_t(i + index) * h->inc;
    if (HeapInfo* h = HeapOfCpu(table.handle, &i)) return h->cpu + uint64_t(i + index) * h->inc;
    return 0;
}
}  // namespace

void Device::copy_descriptor_tables(uint32_t count, const descriptor_table_copy* copies) {
    for (uint32_t i = 0; i < count; ++i) {
        const descriptor_table_copy& c = copies[i];
        uint32_t si = 0;
        HeapInfo* h = HeapOfGpu(c.source_table.handle, &si);
        if (!h) h = HeapOfCpu(c.source_table.handle, &si);
        if (!h) continue;
        const uint64_t src = TableCpu(c.source_table, c.source_binding + c.source_array_offset);
        const uint64_t dst = TableCpu(c.dest_table, c.dest_binding + c.dest_array_offset);
        if (!src || !dst) continue;
        d_->CopyDescriptorsSimple(c.count, D3D12_CPU_DESCRIPTOR_HANDLE{dst}, D3D12_CPU_DESCRIPTOR_HANDLE{src}, h->type);
        CopySlots(dst, src, c.count, h->inc);
    }
}

void Device::update_descriptor_tables(uint32_t count, const descriptor_table_update* updates) {
    for (uint32_t i = 0; i < count; ++i) {
        const descriptor_table_update& u = updates[i];
        const uint64_t base = TableCpu(u.table, u.binding + u.array_offset);
        if (!base) continue;
        const uint32_t inc = inc_[IsSamplerType(u.type) ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV];
        for (uint32_t k = 0; k < u.count; ++k) WriteDescriptor(base + uint64_t(k) * inc, u.type, u.descriptors, k);
    }
}

bool Device::create_query_heap(query_type, uint32_t, query_heap*) { return false; }
void Device::destroy_query_heap(query_heap) {}
bool Device::get_query_heap_results(query_heap, query_type, uint32_t, uint32_t, void*, uint32_t) { return false; }

void Device::set_resource_name(resource r, const char* name) {
    if (!r.handle || !name) return;
    wchar_t w[256];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 256);
    reinterpret_cast<ID3D12Resource*>(r.handle)->SetName(w);
}
void Device::set_resource_view_name(resource_view, const char*) {}
bool Device::create_fence(uint64_t, fence_flags, fence*, void**) { return false; }
void Device::destroy_fence(fence) {}
uint64_t Device::get_completed_fence_value(fence) const { return 0; }
bool Device::wait(fence, uint64_t, uint64_t) { return false; }
bool Device::signal(fence, uint64_t) { return false; }
bool Device::get_property(device_properties, void*) const { return false; }
uint64_t Device::get_resource_view_gpu_address(resource_view) const { return 0; }
void Device::get_acceleration_structure_size(acceleration_structure_type, acceleration_structure_build_flags, uint32_t,
                                             const acceleration_structure_build_input*, uint64_t*, uint64_t*, uint64_t*) const {}
bool Device::get_pipeline_shader_group_handles(pipeline, uint32_t, uint32_t, void*) { return false; }

// ---------------------------------------------------------------- command list

CommandList::CommandList(Device* dev, ID3D12GraphicsCommandList* l) : dev_(dev), l_(l) {}

void CommandList::SetRoot(int which, ID3D12RootSignature* rs) {
    if (!rs || act.root[which] == rs) return;
    if (which == 1) l_->SetComputeRootSignature(rs);
    else l_->SetGraphicsRootSignature(rs);
    act.root[which] = rs;
}

void CommandList::SetHeaps(uint32_t n, ID3D12DescriptorHeap* const* heaps) {
    if (n == act.heapCount && std::equal(heaps, heaps + n, act.heaps)) return;
    l_->SetDescriptorHeaps(n, heaps);
    act.heapCount = n;
    for (uint32_t i = 0; i < n && i < 2; ++i) act.heaps[i] = heaps[i];
}

void CommandList::barrier(uint32_t count, const resource* resources, const resource_usage* olds, const resource_usage* news) {
    std::vector<D3D12_RESOURCE_BARRIER> b;
    b.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        D3D12_RESOURCE_BARRIER x = {};
        ID3D12Resource* r = reinterpret_cast<ID3D12Resource*>(resources[i].handle);
        if (olds[i] == resource_usage::unordered_access && news[i] == resource_usage::unordered_access) {
            x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            x.UAV.pResource = r;
        } else {
            const D3D12_RESOURCE_STATES a = ToStates(olds[i]), c = ToStates(news[i]);
            if (a == c || !r) continue;
            x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, c};
        }
        b.push_back(x);
    }
    if (!b.empty()) l_->ResourceBarrier(static_cast<UINT>(b.size()), b.data());
}

void CommandList::bind_render_targets_and_depth_stencil(uint32_t count, const resource_view* rtvs, resource_view dsv) {
    D3D12_CPU_DESCRIPTOR_HANDLE h[8] = {};
    count = std::min<uint32_t>(count, 8);
    for (uint32_t i = 0; i < count; ++i) h[i].ptr = rtvs[i].handle;
    const D3D12_CPU_DESCRIPTOR_HANDLE d = {dsv.handle};
    l_->OMSetRenderTargets(count, count ? h : nullptr, FALSE, dsv.handle ? &d : nullptr);
}

void CommandList::bind_pipeline(pipeline_stage, pipeline p) {
    if (p.handle) l_->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(p.handle));
}

void CommandList::bind_pipeline_states(uint32_t count, const dynamic_state* states, const uint32_t* values) {
    for (uint32_t i = 0; i < count; ++i) {
        if (states[i] == dynamic_state::primitive_topology) l_->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(values[i]));
        else if (states[i] == dynamic_state::blend_constant) {
            const uint32_t v = values[i];
            const float f[4] = {(v & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, ((v >> 16) & 0xFF) / 255.0f, ((v >> 24) & 0xFF) / 255.0f};
            l_->OMSetBlendFactor(f);
        }
    }
}

void CommandList::bind_viewports(uint32_t, uint32_t count, const viewport* vps) {
    l_->RSSetViewports(count, reinterpret_cast<const D3D12_VIEWPORT*>(vps));
}

void CommandList::bind_scissor_rects(uint32_t, uint32_t count, const rect* rects) {
    l_->RSSetScissorRects(count, reinterpret_cast<const D3D12_RECT*>(rects));
}

void CommandList::push_constants(shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first, uint32_t count,
                                 const void* values) {
    ID3D12RootSignature* rs = reinterpret_cast<ID3D12RootSignature*>(layout.handle);
    if ((stages & shader_stage::all_compute) != 0) {
        SetRoot(1, rs);
        l_->SetComputeRoot32BitConstants(param, count, values, first);
    }
    if ((stages & shader_stage::all_graphics) != 0) {
        SetRoot(0, rs);
        l_->SetGraphicsRoot32BitConstants(param, count, values, first);
    }
}

void CommandList::push_descriptors(shader_stage stages, pipeline_layout layout, uint32_t param, const descriptor_table_update& u) {
    ID3D12RootSignature* rs = reinterpret_cast<ID3D12RootSignature*>(layout.handle);
    const bool compute = (stages & shader_stage::all_compute) != 0, graphics = (stages & shader_stage::all_graphics) != 0;
    D3D12_ROOT_PARAMETER_TYPE type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParamType(rs, param, &type);
    if (type == D3D12_ROOT_PARAMETER_TYPE_CBV || type == D3D12_ROOT_PARAMETER_TYPE_SRV || type == D3D12_ROOT_PARAMETER_TYPE_UAV) {
        // A root descriptor (the game's layouts): a GPU address.
        if (!u.descriptors || u.count < 1) return;
        const buffer_range& b = *static_cast<const buffer_range*>(u.descriptors);
        ID3D12Resource* r = reinterpret_cast<ID3D12Resource*>(b.buffer.handle);
        if (!r || IsDead(r)) return;
        const D3D12_GPU_VIRTUAL_ADDRESS va = r->GetGPUVirtualAddress() + b.offset;
        if (compute) {
            SetRoot(1, rs);
            if (type == D3D12_ROOT_PARAMETER_TYPE_CBV) l_->SetComputeRootConstantBufferView(param, va);
            else if (type == D3D12_ROOT_PARAMETER_TYPE_SRV) l_->SetComputeRootShaderResourceView(param, va);
            else l_->SetComputeRootUnorderedAccessView(param, va);
        }
        if (graphics) {
            SetRoot(0, rs);
            if (type == D3D12_ROOT_PARAMETER_TYPE_CBV) l_->SetGraphicsRootConstantBufferView(param, va);
            else if (type == D3D12_ROOT_PARAMETER_TYPE_SRV) l_->SetGraphicsRootShaderResourceView(param, va);
            else l_->SetGraphicsRootUnorderedAccessView(param, va);
        }
        return;
    }
    // A descriptor table: written into our shader-visible heap, which is bound for it (the game's
    // heaps come back with the next bind_descriptor_tables).
    const bool sampler = IsSamplerType(u.type);
    uint64_t cpu = 0, gpu = 0;
    if (!dev_->AllocGpu(sampler, u.binding + u.count, &cpu, &gpu)) return;
    const uint32_t inc = dev_->Inc(sampler ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (uint32_t k = 0; k < u.count; ++k) dev_->WriteDescriptor(cpu + uint64_t(u.binding + k) * inc, u.type, u.descriptors, k);
    ID3D12DescriptorHeap* const heaps[2] = {dev_->GpuHeap(false), dev_->GpuHeap(true)};
    SetHeaps(2, heaps);
    if (compute) {
        SetRoot(1, rs);
        l_->SetComputeRootDescriptorTable(param, D3D12_GPU_DESCRIPTOR_HANDLE{gpu});
    }
    if (graphics) {
        SetRoot(0, rs);
        l_->SetGraphicsRootDescriptorTable(param, D3D12_GPU_DESCRIPTOR_HANDLE{gpu});
    }
}

void CommandList::bind_descriptor_tables2(shader_stage stages, pipeline_layout layout, uint32_t first, uint32_t count,
                                          const descriptor_table* tables, uint32_t, const uint32_t*) {
    // The game's descriptor heaps (as it last set them), with the heap of any table that's in
    // another one swapped in for its type. With no tables, this puts the game's heaps and the
    // root signature back (after DLSS or our own passes changed them).
    ID3D12DescriptorHeap* heaps[2] = {act.gameHeaps[0], act.gameHeaps[1]};
    uint32_t n = act.gameHeapCount;
    for (uint32_t i = 0; i < count; ++i) {
        if (!tables[i].handle) continue;
        HeapInfo* h = HeapOfGpu(tables[i].handle, nullptr);
        if (!h || !h->heap) continue;
        bool found = false;
        for (uint32_t k = 0; k < n; ++k) {
            HeapInfo* hk = HeapOfObject(heaps[k]);
            if (heaps[k] == h->heap) found = true;
            else if (hk && hk->type == h->type) {
                heaps[k] = h->heap;
                found = true;
            }
        }
        if (!found && n < 2) heaps[n++] = h->heap;
    }
    if (n) SetHeaps(n, heaps);
    ID3D12RootSignature* rs = reinterpret_cast<ID3D12RootSignature*>(layout.handle);
    if ((stages & shader_stage::all_compute) != 0) {
        act.root[1] = nullptr;  // set again, whatever we think is bound
        SetRoot(1, rs);
        for (uint32_t i = 0; i < count; ++i)
            if (tables[i].handle) l_->SetComputeRootDescriptorTable(first + i, D3D12_GPU_DESCRIPTOR_HANDLE{tables[i].handle});
    }
    if ((stages & shader_stage::all_graphics) != 0) {
        act.root[0] = nullptr;
        SetRoot(0, rs);
        for (uint32_t i = 0; i < count; ++i)
            if (tables[i].handle) l_->SetGraphicsRootDescriptorTable(first + i, D3D12_GPU_DESCRIPTOR_HANDLE{tables[i].handle});
    }
}

void CommandList::bind_index_buffer(resource, uint64_t, uint32_t) {}
void CommandList::bind_vertex_buffers(uint32_t, uint32_t, const resource*, const uint64_t*, const uint32_t*) {}
void CommandList::bind_stream_output_buffers(uint32_t, uint32_t, const resource*, const uint64_t*, const uint64_t*, const resource*,
                                             const uint64_t*) {}

void CommandList::draw(uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) { l_->DrawInstanced(v, i, fv, fi); }
void CommandList::draw_indexed(uint32_t n, uint32_t i, uint32_t fi, int32_t vo, uint32_t fin) { l_->DrawIndexedInstanced(n, i, fi, vo, fin); }
void CommandList::dispatch(uint32_t x, uint32_t y, uint32_t z) { l_->Dispatch(x, y, z); }
void CommandList::draw_or_dispatch_indirect(indirect_command, resource, uint64_t, uint32_t, uint32_t) {
    Log("draw_or_dispatch_indirect isn't implemented");
}

void CommandList::copy_resource(resource src, resource dst) {
    l_->CopyResource(reinterpret_cast<ID3D12Resource*>(dst.handle), reinterpret_cast<ID3D12Resource*>(src.handle));
}

void CommandList::copy_buffer_region(resource src, uint64_t so, resource dst, uint64_t dof, uint64_t size) {
    ID3D12Resource* s = reinterpret_cast<ID3D12Resource*>(src.handle);
    if (size == UINT64_MAX) size = DescOf(s).buffer.size - so;
    l_->CopyBufferRegion(reinterpret_cast<ID3D12Resource*>(dst.handle), dof, s, so, size);
}

void CommandList::copy_buffer_to_texture(resource, uint64_t, uint32_t, uint32_t, resource, uint32_t, const subresource_box*) {
    Log("copy_buffer_to_texture isn't implemented");
}

void CommandList::copy_texture_region(resource src, uint32_t ss, const subresource_box* sb, resource dst, uint32_t dsub,
                                      const subresource_box* db, filter_mode) {
    D3D12_TEXTURE_COPY_LOCATION s = {reinterpret_cast<ID3D12Resource*>(src.handle), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    s.SubresourceIndex = ss;
    D3D12_TEXTURE_COPY_LOCATION d = {reinterpret_cast<ID3D12Resource*>(dst.handle), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    d.SubresourceIndex = dsub;
    D3D12_BOX box;
    if (sb) box = {sb->left, sb->top, sb->front, sb->right, sb->bottom, sb->back};
    l_->CopyTextureRegion(&d, db ? db->left : 0, db ? db->top : 0, db ? db->front : 0, &s, sb ? &box : nullptr);
}

void CommandList::copy_texture_to_buffer(resource src, uint32_t ss, const subresource_box* sb, resource dst, uint64_t dof,
                                         uint32_t rowLength, uint32_t sliceHeight) {
    ID3D12Resource* s = reinterpret_cast<ID3D12Resource*>(src.handle);
    const resource_desc sd = DescOf(s);
    const uint32_t mip = sd.texture.levels ? ss % sd.texture.levels : 0;
    const uint32_t w = sb ? sb->width() : std::max(1u, sd.texture.width >> mip);
    const uint32_t h = sb ? sb->height() : std::max(1u, sd.texture.height >> mip);
    const uint32_t depth = sb ? std::max(1u, sb->depth()) : 1;
    const DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(sd.texture.format);
    D3D12_TEXTURE_COPY_LOCATION sl = {s, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    sl.SubresourceIndex = ss;
    D3D12_TEXTURE_COPY_LOCATION dl = {reinterpret_cast<ID3D12Resource*>(dst.handle), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint.Offset = dof;
    dl.PlacedFootprint.Footprint.Format = fmt;
    dl.PlacedFootprint.Footprint.Width = w;
    dl.PlacedFootprint.Footprint.Height = sliceHeight ? std::max(sliceHeight, h) : h;
    dl.PlacedFootprint.Footprint.Depth = depth;
    const uint32_t pitch = (rowLength ? rowLength : w) * BytesPerPixel(fmt);
    dl.PlacedFootprint.Footprint.RowPitch = (pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_BOX box;
    if (sb) box = {sb->left, sb->top, sb->front, sb->right, sb->bottom, sb->back};
    l_->CopyTextureRegion(&dl, 0, 0, 0, &sl, sb ? &box : nullptr);
}

void CommandList::resolve_texture_region(resource, uint32_t, const subresource_box*, resource, uint32_t, uint32_t, uint32_t, uint32_t,
                                         format) {
    Log("resolve_texture_region isn't implemented");
}

void CommandList::clear_depth_stencil_view(resource_view, const float*, const uint8_t*, uint32_t, const rect*) {}
void CommandList::clear_render_target_view(resource_view rtv, const float color[4], uint32_t, const rect*) {
    l_->ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE{rtv.handle}, color, 0, nullptr);
}
void CommandList::clear_unordered_access_view_uint(resource_view, const uint32_t[4], uint32_t, const rect*) {}
void CommandList::clear_unordered_access_view_float(resource_view, const float[4], uint32_t, const rect*) {}
void CommandList::generate_mipmaps(resource_view) {}
void CommandList::begin_query(query_heap, query_type, uint32_t) {}
void CommandList::end_query(query_heap, query_type, uint32_t) {}
void CommandList::copy_query_heap_results(query_heap, query_type, uint32_t, uint32_t, resource, uint64_t, uint32_t) {}
void CommandList::dispatch_mesh(uint32_t, uint32_t, uint32_t) {}
void CommandList::dispatch_rays(resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t,
                                resource, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t) {}
void CommandList::copy_acceleration_structure(resource_view, resource_view, acceleration_structure_copy_mode) {}
void CommandList::build_acceleration_structure(acceleration_structure_type, acceleration_structure_build_flags, uint32_t,
                                               const acceleration_structure_build_input*, resource, uint64_t, resource_view, resource_view,
                                               acceleration_structure_build_mode) {}
void CommandList::query_acceleration_structures(uint32_t, const resource_view*, query_heap, query_type, uint32_t) {}
void CommandList::update_buffer_region(const void*, resource, uint64_t, uint64_t) { Log("update_buffer_region (list) isn't implemented"); }
void CommandList::update_texture_region(const subresource_data&, resource, uint32_t, const subresource_box*) {}
void CommandList::begin_render_pass2(uint32_t, const render_pass_render_target_desc*, const render_pass_depth_stencil_desc*, render_pass_flags) {}

// ---------------------------------------------------------------- queue

command_queue_type Queue::get_type() const {
    switch (type_) {
        case D3D12_COMMAND_LIST_TYPE_COMPUTE: return command_queue_type::compute;
        case D3D12_COMMAND_LIST_TYPE_COPY: return command_queue_type::copy;
        default: return command_queue_type::graphics | command_queue_type::compute | command_queue_type::copy;
    }
}

void Queue::wait_idle() const {
    ID3D12Fence* f = nullptr;
    if (FAILED(dev_->d3d()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)))) return;
    HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(q_->Signal(f, 1)) && SUCCEEDED(f->SetEventOnCompletion(1, e))) WaitForSingleObject(e, 5000);
    CloseHandle(e);
    f->Release();
}

uint64_t Queue::get_timestamp_frequency() const {
    UINT64 f = 0;
    q_->GetTimestampFrequency(&f);
    return f;
}

// ---------------------------------------------------------------- swapchain

resource Swapchain::get_back_buffer(uint32_t index) {
    ID3D12Resource* r = nullptr;
    if (FAILED(sc_->GetBuffer(index, IID_PPV_ARGS(&r))) || !r) return resource{0};
    r->Release();  // the swapchain keeps it alive (as ReShade's handles)
    return resource{reinterpret_cast<uintptr_t>(r)};
}

uint32_t Swapchain::get_back_buffer_count() const {
    DXGI_SWAP_CHAIN_DESC d = {};
    return SUCCEEDED(sc_->GetDesc(&d)) ? d.BufferCount : 0;
}

uint32_t Swapchain::get_current_back_buffer_index() const {
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc_->QueryInterface(IID_PPV_ARGS(&sc3)))) return 0;
    const uint32_t i = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    return i;
}

namespace {
DXGI_COLOR_SPACE_TYPE ToDxgi(color_space cs) {
    switch (cs) {
        case color_space::scrgb: return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        case color_space::hdr10_pq: return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        case color_space::hdr10_hlg: return DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020;
        default: return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
}
}  // namespace

bool Swapchain::check_color_space_support(color_space cs) const {
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc_->QueryInterface(IID_PPV_ARGS(&sc3)))) return false;
    UINT support = 0;
    const bool ok = SUCCEEDED(sc3->CheckColorSpaceSupport(ToDxgi(cs), &support)) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT);
    sc3->Release();
    return ok;
}

color_space Swapchain::get_color_space() const {
    const color_space cs = colorSpace.load();
    return cs == color_space::unknown ? color_space::srgb : cs;
}

}  // namespace rdx
