// rdx: what ReShade tracked for its API (resources, descriptor heaps and their contents, root
// signatures as pipeline layouts, command signatures), and conversions between D3D12 and ReShade's
// API types (ReShade's d3d12_impl_type_convert, the parts the add-on uses).

#include "rdx.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <unordered_set>

#include "descriptor_tracking.hpp"

namespace rdx {

// ---------------------------------------------------------------- destruction notices

namespace {

class Notice final : public IUnknown {
public:
    Notice(void (*fn)(void*, void*), void* obj, void* ctx) : fn_(fn), obj_(obj), ctx_(ctx) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) {
            fn_(obj_, ctx_);
            delete this;
        }
        return n;
    }

private:
    LONG ref_ = 1;
    void (*fn_)(void*, void*);
    void* obj_;
    void* ctx_;
};

// Each notice under its own GUID, so a second one on the same object never replaces (and
// releases) the first.
GUID NoticeGuid() {
    static std::atomic<uint32_t> n{0};
    GUID g = {0x7d3e2a10, 0x52b1, 0x4c6e, {0x9a, 0x41, 0x2f, 0x0b, 0x6c, 0x8d, 0x13, 0x77}};
    g.Data1 += n++;
    return g;
}

}  // namespace

void OnDestroyed(IUnknown* object, void (*fn)(void*, void*), void* ctx) {
    ID3D12Object* o = nullptr;
    if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&o)))) return;
    Notice* n = new Notice(fn, object, ctx);
    const GUID g = NoticeGuid();
    if (FAILED(o->SetPrivateDataInterface(g, n))) Log("couldn't attach a destruction notice");
    n->Release();  // the object holds it now
    o->Release();
}

void OnDestroyedDxgi(IDXGIObject* object, void (*fn)(void*, void*), void* ctx) {
    if (!object) return;
    Notice* n = new Notice(fn, object, ctx);
    const GUID g = NoticeGuid();
    if (FAILED(object->SetPrivateDataInterface(g, n))) Log("couldn't attach a destruction notice (DXGI)");
    n->Release();
}

// ---------------------------------------------------------------- descriptor heaps

namespace {

std::shared_mutex g_heapMutex;
std::vector<HeapInfo*> g_heapsByCpu;  // sorted by cpu start
std::vector<HeapInfo*> g_heapsByGpu;  // shader visible only, sorted by gpu start

HeapInfo* Find(const std::vector<HeapInfo*>& v, uint64_t h, bool gpu, uint32_t* index) {
    auto it = std::upper_bound(v.begin(), v.end(), h, [gpu](uint64_t x, const HeapInfo* hi) { return x < (gpu ? hi->gpu : hi->cpu); });
    if (it == v.begin()) return nullptr;
    HeapInfo* hi = *(it - 1);
    const uint64_t start = gpu ? hi->gpu : hi->cpu;
    const uint64_t i = (h - start) / hi->inc;
    if (i >= hi->count) return nullptr;
    if (index) *index = static_cast<uint32_t>(i);
    return hi;
}

void Untrack(void* object, void*) {
    std::unique_lock<std::shared_mutex> lock(g_heapMutex);
    HeapInfo* gone = nullptr;
    for (auto* v : {&g_heapsByCpu, &g_heapsByGpu}) {
        auto it = std::find_if(v->begin(), v->end(), [&](HeapInfo* h) { return h->heap == object; });
        if (it == v->end()) continue;
        gone = *it;
        v->erase(it);
    }
    // The info and its slots are kept (not freed): a lookup racing with this would read freed
    // memory, and heaps come and go rarely.
    if (gone) gone->heap = nullptr;
}

}  // namespace

void TrackHeap(ID3D12DescriptorHeap* heap, bool ours) {
    if (!heap) return;
    const D3D12_DESCRIPTOR_HEAP_DESC d = heap->GetDesc();
    ID3D12Device* dev = nullptr;
    if (FAILED(heap->GetDevice(IID_PPV_ARGS(&dev)))) return;
    HeapInfo* h = new HeapInfo;
    h->heap = heap;
    h->type = d.Type;
    h->count = d.NumDescriptors;
    h->inc = dev->GetDescriptorHandleIncrementSize(d.Type);
    dev->Release();
    h->cpu = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) h->gpu = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    h->slots = new Slot[h->count];
    h->ours = ours;
    {
        std::unique_lock<std::shared_mutex> lock(g_heapMutex);
        auto cmpCpu = [](HeapInfo* a, HeapInfo* b) { return a->cpu < b->cpu; };
        g_heapsByCpu.insert(std::upper_bound(g_heapsByCpu.begin(), g_heapsByCpu.end(), h, cmpCpu), h);
        if (h->gpu) {
            auto cmpGpu = [](HeapInfo* a, HeapInfo* b) { return a->gpu < b->gpu; };
            g_heapsByGpu.insert(std::upper_bound(g_heapsByGpu.begin(), g_heapsByGpu.end(), h, cmpGpu), h);
        }
    }
    if (!ours) OnDestroyed(heap, &Untrack, nullptr);
    static std::atomic<int> logged{0};
    if (logged++ < 40)
        Log("descriptor heap type %d, %u descriptors%s%s", d.Type, d.NumDescriptors, h->gpu ? ", shader visible" : "", ours ? " (ours)" : "");
}

HeapInfo* HeapOfCpu(uint64_t cpu, uint32_t* index) {
    std::shared_lock<std::shared_mutex> lock(g_heapMutex);
    return Find(g_heapsByCpu, cpu, false, index);
}

HeapInfo* HeapOfGpu(uint64_t gpu, uint32_t* index) {
    std::shared_lock<std::shared_mutex> lock(g_heapMutex);
    return Find(g_heapsByGpu, gpu, true, index);
}

HeapInfo* HeapOfObject(ID3D12DescriptorHeap* heap) {
    std::shared_lock<std::shared_mutex> lock(g_heapMutex);
    for (HeapInfo* h : g_heapsByCpu)
        if (h->heap == heap) return h;
    return nullptr;
}

Slot* SlotAt(uint64_t cpu) {
    uint32_t i = 0;
    HeapInfo* h = HeapOfCpu(cpu, &i);
    return h ? &h->slots[i] : nullptr;
}

void CopySlots(uint64_t dst, uint64_t src, uint32_t count, uint32_t) {
    uint32_t di = 0, si = 0;
    HeapInfo* dh = HeapOfCpu(dst, &di);
    if (!dh) return;
    HeapInfo* sh = HeapOfCpu(src, &si);
    const uint32_t n = std::min(count, dh->count - di);
    if (!sh) {
        for (uint32_t k = 0; k < n; ++k) dh->slots[di + k] = Slot();
        return;
    }
    const uint32_t m = std::min(n, sh->count - si);
    memmove(&dh->slots[di], &sh->slots[si], m * sizeof(Slot));
    for (uint32_t k = m; k < n; ++k) dh->slots[di + k] = Slot();
}

namespace {
std::mutex g_fullMutex;
std::unordered_map<uint64_t, FullView> g_fullViews;
}  // namespace

void RememberFullView(uint64_t cpu, const FullView& v) {
    std::lock_guard<std::mutex> lock(g_fullMutex);
    g_fullViews[cpu] = v;
}

bool FullViewAt(uint64_t cpu, FullView* out) {
    std::lock_guard<std::mutex> lock(g_fullMutex);
    auto it = g_fullViews.find(cpu);
    if (it == g_fullViews.end()) return false;
    *out = it->second;
    return true;
}

// ---------------------------------------------------------------- resources

namespace {

std::shared_mutex g_resMutex;
std::unordered_map<ID3D12Resource*, ResInfo> g_res;
std::unordered_set<ID3D12Resource*> g_dead;
std::map<uint64_t, std::pair<ID3D12Resource*, uint64_t>> g_vas;  // buffer GPU address -> (buffer, size)

void ResourceGone(void* object, void* ctx) {
    ID3D12Resource* r = static_cast<ID3D12Resource*>(object);
    const bool fire = ctx != nullptr;
    if (fire) Invoke<addon_event::destroy_resource>(static_cast<device*>(GameDevice()), resource{reinterpret_cast<uintptr_t>(r)});
    std::unique_lock<std::shared_mutex> lock(g_resMutex);
    auto it = g_res.find(r);
    if (it != g_res.end()) {
        if (it->second.va) {
            auto v = g_vas.find(it->second.va);
            if (v != g_vas.end() && v->second.first == r) g_vas.erase(v);
        }
        g_res.erase(it);
    }
    g_dead.insert(r);
    if (g_dead.size() > 200000) g_dead.clear();  // bounded: very old pointers may be reused by now anyway
}

}  // namespace

void TrackResource(ID3D12Resource* r, bool fireEvents, D3D12_RESOURCE_STATES initial) {
    if (!r) return;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    D3D12_HEAP_PROPERTIES heap = {};
    D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
    const bool hasHeap = SUCCEEDED(r->GetHeapProperties(&heap, &flags));
    ResInfo info;
    info.desc = ToDesc(d, hasHeap ? &heap : nullptr, flags);
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) info.va = r->GetGPUVirtualAddress();
    {
        std::unique_lock<std::shared_mutex> lock(g_resMutex);
        g_res[r] = info;
        g_dead.erase(r);
        if (info.va) g_vas[info.va] = {r, d.Width};
    }
    OnDestroyed(r, &ResourceGone, fireEvents ? reinterpret_cast<void*>(1) : nullptr);
    if (fireEvents)
        Invoke<addon_event::init_resource>(static_cast<device*>(GameDevice()), info.desc, static_cast<const subresource_data*>(nullptr),
                                           ToUsage(initial), resource{reinterpret_cast<uintptr_t>(r)});
}

bool ResourceDesc(ID3D12Resource* r, resource_desc* out) {
    std::shared_lock<std::shared_mutex> lock(g_resMutex);
    auto it = g_res.find(r);
    if (it == g_res.end()) return false;
    *out = it->second.desc;
    return true;
}

bool IsDead(ID3D12Resource* r) {
    std::shared_lock<std::shared_mutex> lock(g_resMutex);
    return g_dead.count(r) != 0;
}

void MarkAlive(ID3D12Resource* r) {
    {
        std::shared_lock<std::shared_mutex> lock(g_resMutex);
        if (!g_dead.count(r)) return;
    }
    std::unique_lock<std::shared_mutex> lock(g_resMutex);
    g_dead.erase(r);
}

ID3D12Resource* ResourceAtVa(uint64_t va, uint64_t* offset) {
    std::shared_lock<std::shared_mutex> lock(g_resMutex);
    auto it = g_vas.upper_bound(va);
    if (it == g_vas.begin()) return nullptr;
    --it;
    if (va - it->first >= it->second.second) return nullptr;
    if (offset) *offset = va - it->first;
    return it->second.first;
}

resource_desc DescOf(ID3D12Resource* r) {
    resource_desc d;
    if (!r) return d;
    if (ResourceDesc(r, &d)) return d;
    // Not created through the hooks (swapchain buffers, others'): asked of D3D12. Not refused when
    // the address once held a freed game resource: DXGI's back buffers can sit at such an address
    // after a resize (Village), and callers only ask about resources they hold now (views of freed
    // resources already resolve to none in get_resource_from_view).
    const D3D12_RESOURCE_DESC rd = r->GetDesc();
    D3D12_HEAP_PROPERTIES heap = {};
    D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
    const bool hasHeap = SUCCEEDED(r->GetHeapProperties(&heap, &flags));
    return ToDesc(rd, hasHeap ? &heap : nullptr, flags);
}

// ---------------------------------------------------------------- root signatures

namespace {

std::shared_mutex g_layoutMutex;
std::unordered_map<ID3D12RootSignature*, std::unique_ptr<LayoutInfo>> g_layouts;
std::vector<std::unique_ptr<LayoutInfo>> g_oldLayouts;  // replaced ones: kept, a lookup may still point into them

void StoreLayout(ID3D12RootSignature* rs, std::unique_ptr<LayoutInfo> info) {
    // The params point into this info's own range vectors.
    for (size_t i = 0; i < info->params.size(); ++i)
        if (info->params[i].type == pipeline_layout_param_type::descriptor_table) {
            info->params[i].descriptor_table.count = static_cast<uint32_t>(info->ranges[i].size());
            info->params[i].descriptor_table.ranges = info->ranges[i].data();
        }
    std::unique_lock<std::shared_mutex> lock(g_layoutMutex);
    auto& slot = g_layouts[rs];
    if (slot) g_oldLayouts.push_back(std::move(slot));
    slot = std::move(info);
}

descriptor_type RangeType(D3D12_DESCRIPTOR_RANGE_TYPE t) {
    switch (t) {
        case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: return descriptor_type::shader_resource_view;
        case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: return descriptor_type::unordered_access_view;
        case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: return descriptor_type::constant_buffer;
        default: return descriptor_type::sampler;
    }
}

}  // namespace

shader_stage ToStages(D3D12_SHADER_VISIBILITY v) {
    switch (v) {
        case D3D12_SHADER_VISIBILITY_VERTEX: return shader_stage::vertex;
        case D3D12_SHADER_VISIBILITY_HULL: return shader_stage::hull;
        case D3D12_SHADER_VISIBILITY_DOMAIN: return shader_stage::domain;
        case D3D12_SHADER_VISIBILITY_GEOMETRY: return shader_stage::geometry;
        case D3D12_SHADER_VISIBILITY_PIXEL: return shader_stage::pixel;
        case D3D12_SHADER_VISIBILITY_AMPLIFICATION: return shader_stage::amplification;
        case D3D12_SHADER_VISIBILITY_MESH: return shader_stage::mesh;
        default: return shader_stage::all;
    }
}

void TrackRootSignature(ID3D12RootSignature* rs, const void* blob, size_t size) {
    using DeserFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    static DeserFn deser = reinterpret_cast<DeserFn>(
        GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12CreateVersionedRootSignatureDeserializer"));
    if (!deser || !rs || !blob) return;
    ID3D12VersionedRootSignatureDeserializer* d = nullptr;
    if (FAILED(deser(blob, size, IID_PPV_ARGS(&d)))) return;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
    if (FAILED(d->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) || !vd) {
        d->Release();
        return;
    }
    const D3D12_ROOT_SIGNATURE_DESC1& desc = vd->Desc_1_1;
    auto info = std::make_unique<LayoutInfo>();
    info->params.resize(desc.NumParameters);
    info->ranges.resize(desc.NumParameters);
    info->rootTypes.resize(desc.NumParameters);
    for (UINT i = 0; i < desc.NumParameters; ++i) {
        const D3D12_ROOT_PARAMETER1& p = desc.pParameters[i];
        pipeline_layout_param& out = info->params[i];
        info->rootTypes[i] = p.ParameterType;
        const shader_stage vis = ToStages(p.ShaderVisibility);
        switch (p.ParameterType) {
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
                out.type = pipeline_layout_param_type::descriptor_table;
                uint32_t offset = 0;
                for (UINT k = 0; k < p.DescriptorTable.NumDescriptorRanges; ++k) {
                    const D3D12_DESCRIPTOR_RANGE1& r = p.DescriptorTable.pDescriptorRanges[k];
                    descriptor_range dr;
                    if (r.OffsetInDescriptorsFromTableStart != D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
                        offset = r.OffsetInDescriptorsFromTableStart;
                    dr.binding = offset;
                    dr.dx_register_index = r.BaseShaderRegister;
                    dr.dx_register_space = r.RegisterSpace;
                    dr.count = r.NumDescriptors;
                    dr.visibility = vis;
                    dr.array_size = 1;
                    dr.type = RangeType(r.RangeType);
                    info->ranges[i].push_back(dr);
                    if (r.NumDescriptors != UINT_MAX) offset += r.NumDescriptors;
                }
                break;
            }
            case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
                out.type = pipeline_layout_param_type::push_constants;
                out.push_constants = constant_range{0, p.Constants.ShaderRegister, p.Constants.RegisterSpace, p.Constants.Num32BitValues, vis};
                break;
            default: {
                out.type = pipeline_layout_param_type::push_descriptors;
                descriptor_range dr;
                dr.binding = 0;
                dr.dx_register_index = p.Descriptor.ShaderRegister;
                dr.dx_register_space = p.Descriptor.RegisterSpace;
                dr.count = 1;
                dr.visibility = vis;
                dr.type = p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV   ? descriptor_type::constant_buffer
                          : p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? descriptor_type::buffer_shader_resource_view
                                                                             : descriptor_type::buffer_unordered_access_view;
                out.push_descriptors = dr;
                break;
            }
        }
    }
    d->Release();
    StoreLayout(rs, std::move(info));
}

void TrackOwnLayout(ID3D12RootSignature* rs, uint32_t count, const pipeline_layout_param* params) {
    auto info = std::make_unique<LayoutInfo>();
    info->params.assign(params, params + count);
    info->ranges.resize(count);
    info->rootTypes.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        // Our layouts: push constants are root constants; everything else is a descriptor table.
        if (params[i].type == pipeline_layout_param_type::push_constants) {
            info->rootTypes[i] = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            continue;
        }
        info->rootTypes[i] = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        if (params[i].type == pipeline_layout_param_type::push_descriptors) {
            info->ranges[i].push_back(params[i].push_descriptors);
        } else if (params[i].type == pipeline_layout_param_type::descriptor_table ||
                   params[i].type == pipeline_layout_param_type::push_descriptors_with_ranges) {
            info->ranges[i].assign(params[i].descriptor_table.ranges, params[i].descriptor_table.ranges + params[i].descriptor_table.count);
        } else {
            for (uint32_t k = 0; k < params[i].descriptor_table_with_flags.count; ++k)
                info->ranges[i].push_back(params[i].descriptor_table_with_flags.ranges[k]);
        }
        info->params[i].type = pipeline_layout_param_type::descriptor_table;
    }
    StoreLayout(rs, std::move(info));
}

bool LayoutParam(ID3D12RootSignature* rs, uint32_t param, pipeline_layout_param* out) {
    std::shared_lock<std::shared_mutex> lock(g_layoutMutex);
    auto it = g_layouts.find(rs);
    if (it == g_layouts.end() || param >= it->second->params.size()) return false;
    *out = it->second->params[param];
    return true;
}

bool RootParamType(ID3D12RootSignature* rs, uint32_t param, D3D12_ROOT_PARAMETER_TYPE* out) {
    std::shared_lock<std::shared_mutex> lock(g_layoutMutex);
    auto it = g_layouts.find(rs);
    if (it == g_layouts.end() || param >= it->second->rootTypes.size()) return false;
    *out = it->second->rootTypes[param];
    return true;
}

// ---------------------------------------------------------------- command signatures

namespace {
std::shared_mutex g_sigMutex;
std::unordered_map<ID3D12CommandSignature*, indirect_command> g_sigs;
}  // namespace

void TrackCommandSignature(ID3D12CommandSignature* sig, const D3D12_COMMAND_SIGNATURE_DESC* desc) {
    if (!sig || !desc) return;
    indirect_command type = indirect_command::unknown;
    for (UINT i = 0; i < desc->NumArgumentDescs; ++i) {
        switch (desc->pArgumentDescs[i].Type) {
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: type = indirect_command::draw; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: type = indirect_command::draw_indexed; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: type = indirect_command::dispatch; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH: type = indirect_command::dispatch_mesh; break;
            case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS: type = indirect_command::dispatch_rays; break;
            default: break;
        }
    }
    std::unique_lock<std::shared_mutex> lock(g_sigMutex);
    g_sigs[sig] = type;
}

indirect_command CommandSignatureType(ID3D12CommandSignature* sig) {
    std::shared_lock<std::shared_mutex> lock(g_sigMutex);
    auto it = g_sigs.find(sig);
    return it == g_sigs.end() ? indirect_command::unknown : it->second;
}

// ---------------------------------------------------------------- conversions

D3D12_RESOURCE_STATES ToStates(resource_usage u) {
    if (u == resource_usage::general || u == resource_usage::undefined) return D3D12_RESOURCE_STATE_COMMON;
    if (u == resource_usage::present) return D3D12_RESOURCE_STATE_PRESENT;
    if (u == resource_usage::cpu_access) return D3D12_RESOURCE_STATE_GENERIC_READ;
    uint32_t s = static_cast<uint32_t>(u) & 0x7FFFFFFF;
    if ((s & 0x30) == 0x30) s &= ~0x20u;  // depth write and read: write
    if (s & 0x8000) s = (s & ~0x8000u) | D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    return static_cast<D3D12_RESOURCE_STATES>(s);
}

resource_usage ToUsage(D3D12_RESOURCE_STATES s) {
    if (s == D3D12_RESOURCE_STATE_COMMON) return resource_usage::general;
    if (s == D3D12_RESOURCE_STATE_GENERIC_READ) return resource_usage::cpu_access;
    uint32_t u = static_cast<uint32_t>(s);
    if (s & D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) u |= static_cast<uint32_t>(resource_usage::constant_buffer);
    return static_cast<resource_usage>(u);
}

resource_desc ToDesc(const D3D12_RESOURCE_DESC& d, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags) {
    resource_desc out;
    switch (d.Dimension) {
        case D3D12_RESOURCE_DIMENSION_BUFFER: out.type = resource_type::buffer; break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE1D: out.type = resource_type::texture_1d; break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE2D: out.type = resource_type::texture_2d; break;
        case D3D12_RESOURCE_DIMENSION_TEXTURE3D: out.type = resource_type::texture_3d; break;
        default: out.type = resource_type::unknown; break;
    }
    if (out.type == resource_type::buffer) {
        out.buffer.size = d.Width;
        out.buffer.structured.stride = 0;
    } else {
        out.texture.width = static_cast<uint32_t>(d.Width);
        out.texture.height = d.Height;
        out.texture.depth_or_layers = d.DepthOrArraySize;
        out.texture.levels = d.MipLevels;
        out.texture.format = static_cast<format>(d.Format);
        out.texture.samples = static_cast<uint16_t>(d.SampleDesc.Count);
    }
    if (!heap) {
        out.heap = memory_heap::unknown;
        out.flags |= resource_flags::sparse_binding;
    } else {
        switch (heap->Type) {
            case D3D12_HEAP_TYPE_DEFAULT: out.heap = memory_heap::default_; break;
            case D3D12_HEAP_TYPE_UPLOAD: out.heap = memory_heap::upload; break;
            case D3D12_HEAP_TYPE_READBACK: out.heap = memory_heap::readback; break;
            case static_cast<D3D12_HEAP_TYPE>(5): out.heap = memory_heap::gpu_upload; break;  // D3D12_HEAP_TYPE_GPU_UPLOAD
            default: out.heap = memory_heap::custom; break;
        }
    }
    if (flags & D3D12_HEAP_FLAG_SHARED) out.flags |= resource_flags::shared;
    uint32_t u = static_cast<uint32_t>(resource_usage::copy_dest | resource_usage::copy_source);
    if (out.type == resource_type::buffer)
        u |= static_cast<uint32_t>(resource_usage::index_buffer | resource_usage::vertex_buffer | resource_usage::constant_buffer |
                                   resource_usage::indirect_argument);
    if (!(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) u |= static_cast<uint32_t>(resource_usage::shader_resource);
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) u |= static_cast<uint32_t>(resource_usage::render_target);
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) u |= static_cast<uint32_t>(resource_usage::depth_stencil);
    if (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) u |= static_cast<uint32_t>(resource_usage::unordered_access);
    out.usage = static_cast<resource_usage>(u);
    return out;
}

void FromDesc(const resource_desc& in, D3D12_RESOURCE_DESC* d, D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS* flags) {
    *d = {};
    *heap = {};
    *flags = D3D12_HEAP_FLAG_NONE;
    switch (in.type) {
        case resource_type::buffer: d->Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; break;
        case resource_type::texture_1d: d->Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D; break;
        case resource_type::texture_3d: d->Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D; break;
        default: d->Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; break;
    }
    if (in.type == resource_type::buffer) {
        d->Width = in.buffer.size;
        d->Height = 1;
        d->DepthOrArraySize = 1;
        d->MipLevels = 1;
        d->Format = DXGI_FORMAT_UNKNOWN;
        d->SampleDesc.Count = 1;
        d->Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    } else {
        d->Width = in.texture.width;
        d->Height = in.texture.height;
        d->DepthOrArraySize = in.texture.depth_or_layers ? in.texture.depth_or_layers : 1;
        d->MipLevels = in.texture.levels;
        d->Format = static_cast<DXGI_FORMAT>(in.texture.format);
        d->SampleDesc.Count = in.texture.samples ? in.texture.samples : 1;
        d->Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }
    if ((in.usage & resource_usage::render_target) != 0) d->Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if ((in.usage & resource_usage::depth_stencil) != 0) {
        d->Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if ((in.usage & resource_usage::shader_resource) == 0) d->Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }
    if ((in.usage & resource_usage::unordered_access) != 0) d->Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    switch (in.heap) {
        case memory_heap::upload: heap->Type = D3D12_HEAP_TYPE_UPLOAD; break;
        case memory_heap::readback: heap->Type = D3D12_HEAP_TYPE_READBACK; break;
        default: heap->Type = D3D12_HEAP_TYPE_DEFAULT; break;
    }
    if ((in.flags & resource_flags::shared) != 0) *flags |= D3D12_HEAP_FLAG_SHARED;
}

sampler_desc ToSamplerDesc(const D3D12_SAMPLER_DESC& d) {
    sampler_desc s;
    s.filter = static_cast<filter_mode>(d.Filter);
    s.address_u = static_cast<texture_address_mode>(d.AddressU);
    s.address_v = static_cast<texture_address_mode>(d.AddressV);
    s.address_w = static_cast<texture_address_mode>(d.AddressW);
    s.mip_lod_bias = d.MipLODBias;
    s.max_anisotropy = static_cast<float>(d.MaxAnisotropy);
    s.compare_op = static_cast<compare_op>(d.ComparisonFunc);
    for (int i = 0; i < 4; ++i) s.border_color[i] = d.BorderColor[i];
    s.min_lod = d.MinLOD;
    s.max_lod = d.MaxLOD;
    return s;
}

D3D12_SAMPLER_DESC FromSamplerDesc(const sampler_desc& s) {
    D3D12_SAMPLER_DESC d = {};
    d.Filter = static_cast<D3D12_FILTER>(s.filter);
    d.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.address_u);
    d.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.address_v);
    d.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(s.address_w);
    d.MipLODBias = s.mip_lod_bias;
    d.MaxAnisotropy = static_cast<UINT>(s.max_anisotropy);
    d.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(s.compare_op);
    for (int i = 0; i < 4; ++i) d.BorderColor[i] = s.border_color[i];
    d.MinLOD = s.min_lod;
    d.MaxLOD = s.max_lod;
    return d;
}

namespace {
resource_view_desc Tex(resource_view_type t, DXGI_FORMAT f, uint32_t mip, uint32_t mips, uint32_t layer, uint32_t layers) {
    return resource_view_desc(t, static_cast<format>(f), mip, mips, layer, layers);
}
}  // namespace

resource_view_desc ToViewDesc(const D3D12_SHADER_RESOURCE_VIEW_DESC& d) {
    switch (d.ViewDimension) {
        case D3D12_SRV_DIMENSION_BUFFER:
            return resource_view_desc(resource_view_type::buffer, static_cast<format>(d.Format), d.Buffer.FirstElement, d.Buffer.NumElements);
        case D3D12_SRV_DIMENSION_TEXTURE1D: return Tex(resource_view_type::texture_1d, d.Format, d.Texture1D.MostDetailedMip, d.Texture1D.MipLevels, 0, 1);
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
            return Tex(resource_view_type::texture_1d_array, d.Format, d.Texture1DArray.MostDetailedMip, d.Texture1DArray.MipLevels,
                       d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize);
        case D3D12_SRV_DIMENSION_TEXTURE2D: return Tex(resource_view_type::texture_2d, d.Format, d.Texture2D.MostDetailedMip, d.Texture2D.MipLevels, 0, 1);
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            return Tex(resource_view_type::texture_2d_array, d.Format, d.Texture2DArray.MostDetailedMip, d.Texture2DArray.MipLevels,
                       d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize);
        case D3D12_SRV_DIMENSION_TEXTURE2DMS: return Tex(resource_view_type::texture_2d_multisample, d.Format, 0, 1, 0, 1);
        case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
            return Tex(resource_view_type::texture_2d_multisample_array, d.Format, 0, 1, d.Texture2DMSArray.FirstArraySlice,
                       d.Texture2DMSArray.ArraySize);
        case D3D12_SRV_DIMENSION_TEXTURE3D: return Tex(resource_view_type::texture_3d, d.Format, d.Texture3D.MostDetailedMip, d.Texture3D.MipLevels, 0, 1);
        case D3D12_SRV_DIMENSION_TEXTURECUBE: return Tex(resource_view_type::texture_cube, d.Format, d.TextureCube.MostDetailedMip, d.TextureCube.MipLevels, 0, 6);
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            return Tex(resource_view_type::texture_cube_array, d.Format, d.TextureCubeArray.MostDetailedMip, d.TextureCubeArray.MipLevels,
                       d.TextureCubeArray.First2DArrayFace, d.TextureCubeArray.NumCubes * 6);
        case D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE: {
            resource_view_desc v;
            v.type = resource_view_type::acceleration_structure;
            v.buffer.offset = d.RaytracingAccelerationStructure.Location;
            return v;
        }
        default: return resource_view_desc();
    }
}

resource_view_desc ToViewDesc(const D3D12_UNORDERED_ACCESS_VIEW_DESC& d) {
    switch (d.ViewDimension) {
        case D3D12_UAV_DIMENSION_BUFFER:
            return resource_view_desc(resource_view_type::buffer, static_cast<format>(d.Format), d.Buffer.FirstElement, d.Buffer.NumElements);
        case D3D12_UAV_DIMENSION_TEXTURE1D: return Tex(resource_view_type::texture_1d, d.Format, d.Texture1D.MipSlice, 1, 0, 1);
        case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
            return Tex(resource_view_type::texture_1d_array, d.Format, d.Texture1DArray.MipSlice, 1, d.Texture1DArray.FirstArraySlice,
                       d.Texture1DArray.ArraySize);
        case D3D12_UAV_DIMENSION_TEXTURE2D: return Tex(resource_view_type::texture_2d, d.Format, d.Texture2D.MipSlice, 1, 0, 1);
        case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
            return Tex(resource_view_type::texture_2d_array, d.Format, d.Texture2DArray.MipSlice, 1, d.Texture2DArray.FirstArraySlice,
                       d.Texture2DArray.ArraySize);
        case D3D12_UAV_DIMENSION_TEXTURE3D:
            return Tex(resource_view_type::texture_3d, d.Format, d.Texture3D.MipSlice, 1, d.Texture3D.FirstWSlice, d.Texture3D.WSize);
        default: return resource_view_desc();
    }
}

resource_view_desc ToViewDesc(const D3D12_RENDER_TARGET_VIEW_DESC& d) {
    switch (d.ViewDimension) {
        case D3D12_RTV_DIMENSION_BUFFER:
            return resource_view_desc(resource_view_type::buffer, static_cast<format>(d.Format), d.Buffer.FirstElement, d.Buffer.NumElements);
        case D3D12_RTV_DIMENSION_TEXTURE1D: return Tex(resource_view_type::texture_1d, d.Format, d.Texture1D.MipSlice, 1, 0, 1);
        case D3D12_RTV_DIMENSION_TEXTURE2D: return Tex(resource_view_type::texture_2d, d.Format, d.Texture2D.MipSlice, 1, 0, 1);
        case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
            return Tex(resource_view_type::texture_2d_array, d.Format, d.Texture2DArray.MipSlice, 1, d.Texture2DArray.FirstArraySlice,
                       d.Texture2DArray.ArraySize);
        case D3D12_RTV_DIMENSION_TEXTURE2DMS: return Tex(resource_view_type::texture_2d_multisample, d.Format, 0, 1, 0, 1);
        case D3D12_RTV_DIMENSION_TEXTURE3D:
            return Tex(resource_view_type::texture_3d, d.Format, d.Texture3D.MipSlice, 1, d.Texture3D.FirstWSlice, d.Texture3D.WSize);
        default: return resource_view_desc();
    }
}

resource_view_desc ToViewDesc(const D3D12_DEPTH_STENCIL_VIEW_DESC& d) {
    switch (d.ViewDimension) {
        case D3D12_DSV_DIMENSION_TEXTURE1D: return Tex(resource_view_type::texture_1d, d.Format, d.Texture1D.MipSlice, 1, 0, 1);
        case D3D12_DSV_DIMENSION_TEXTURE2D: return Tex(resource_view_type::texture_2d, d.Format, d.Texture2D.MipSlice, 1, 0, 1);
        case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
            return Tex(resource_view_type::texture_2d_array, d.Format, d.Texture2DArray.MipSlice, 1, d.Texture2DArray.FirstArraySlice,
                       d.Texture2DArray.ArraySize);
        case D3D12_DSV_DIMENSION_TEXTURE2DMS: return Tex(resource_view_type::texture_2d_multisample, d.Format, 0, 1, 0, 1);
        default: return resource_view_desc();
    }
}

resource_view_desc DefaultViewDesc(ID3D12Resource* r, int) {
    if (!r) return resource_view_desc();
    const resource_desc d = DescOf(r);
    if (d.type == resource_type::buffer) return resource_view_desc(resource_view_type::buffer, format::unknown, 0, d.buffer.size);
    const bool array = d.texture.depth_or_layers > 1 && d.type != resource_type::texture_3d;
    resource_view_type t = resource_view_type::texture_2d;
    if (d.type == resource_type::texture_1d) t = array ? resource_view_type::texture_1d_array : resource_view_type::texture_1d;
    else if (d.type == resource_type::texture_3d) t = resource_view_type::texture_3d;
    else if (d.texture.samples > 1) t = array ? resource_view_type::texture_2d_multisample_array : resource_view_type::texture_2d_multisample;
    else if (array) t = resource_view_type::texture_2d_array;
    return resource_view_desc(t, d.texture.format, 0, d.texture.levels, 0, d.texture.depth_or_layers);
}

}  // namespace rdx

// ---------------------------------------------------------------- descriptor_tracking (ReShade's utility)

using namespace reshade::api;

namespace {
const rdx::Slot* TrackedSlot(descriptor_heap heap, uint32_t offset) {
    rdx::HeapInfo* h = rdx::HeapOfObject(reinterpret_cast<ID3D12DescriptorHeap*>(heap.handle));
    if (!h || offset >= h->count) return nullptr;
    return &h->slots[offset];
}
}  // namespace

buffer_range descriptor_tracking::get_buffer_range(descriptor_heap heap, uint32_t offset) const {
    const rdx::Slot* s = TrackedSlot(heap, offset);
    if (!s || !s->used || s->type != descriptor_type::constant_buffer || !s->res) return {};
    buffer_range b;
    b.buffer = resource{reinterpret_cast<uintptr_t>(s->res)};
    b.offset = s->offset;
    b.size = s->size;
    return b;
}

sampler descriptor_tracking::get_sampler(descriptor_heap heap, uint32_t offset) const {
    const rdx::Slot* s = TrackedSlot(heap, offset);
    if (!s || !s->used || s->type != descriptor_type::sampler) return {0};
    return sampler{s->origin};
}

resource_view descriptor_tracking::get_resource_view(descriptor_heap heap, uint32_t offset) const {
    const rdx::Slot* s = TrackedSlot(heap, offset);
    if (!s || !s->used || s->type == descriptor_type::sampler || s->type == descriptor_type::constant_buffer) return {0};
    return resource_view{s->origin};
}

pipeline_layout_param descriptor_tracking::get_pipeline_layout_param(pipeline_layout layout, uint32_t param) const {
    pipeline_layout_param p;
    if (!rdx::LayoutParam(reinterpret_cast<ID3D12RootSignature*>(layout.handle), param, &p)) return {};
    return p;
}
