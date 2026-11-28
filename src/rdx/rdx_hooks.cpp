// rdx: hooks on the game's D3D12 and DXGI calls, firing ReShade's add-on events (rdx.hpp).
//
// Installed from DllMain (REFramework loads plugins before the game creates its device): the
// game's GetProcAddress (it looks D3D12CreateDevice up itself) and its CreateDXGIFactory1/2
// imports. From the device the game gets, the vtables of the objects it creates are patched
// (each class with its own originals: D3D12Core has three command list classes). Events fire,
// in ReShade's order (state changes after the call, draws / copies / submits / present before,
// with a callback able to skip them), only for calls the game itself makes.

#include "rdx.hpp"

#include <intrin.h>

#include <algorithm>

#include "rdx_slots.h"

#pragma intrinsic(_ReturnAddress)

namespace rdx {

namespace {

int g_slot[kSlotCount];
uintptr_t g_exeBase = 0, g_exeEnd = 0;

inline bool FromGame(void* ret) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(ret);
    return a >= g_exeBase && a < g_exeEnd;
}
#define GAME FromGame(_ReturnAddress())

// ---------------------------------------------------------------- vtable patching

struct VtEntry {
    std::atomic<void**> vt{nullptr};
    void* orig[kSlotCount] = {};
};
constexpr int kMaxVts = 64;
VtEntry g_vts[kMaxVts];
std::mutex g_vtMutex;

inline void* OrigFor(void* obj, RdxSlot s) {
    void** vt = *static_cast<void***>(obj);
    for (VtEntry& e : g_vts) {
        void** v = e.vt.load(std::memory_order_acquire);
        if (v == vt) return e.orig[s];
        if (!v) break;
    }
    return nullptr;
}

template <typename F>
inline F O(void* obj, RdxSlot s) {
    return reinterpret_cast<F>(OrigFor(obj, s));
}

struct Hook {
    RdxSlot slot;
    void* fn;
};

void Patch(void* obj, std::initializer_list<Hook> hooks) {
    void** vt = *static_cast<void***>(obj);
    std::lock_guard<std::mutex> lock(g_vtMutex);
    VtEntry* e = nullptr;
    for (VtEntry& x : g_vts) {
        void** v = x.vt.load();
        if (v == vt || !v) {
            e = &x;
            break;
        }
    }
    if (!e) {
        Log("too many hooked classes");
        return;
    }
    const bool fresh = e->vt.load() == nullptr;
    for (const Hook& h : hooks) {
        if (!fresh && e->orig[h.slot]) continue;
        void** slot = vt + g_slot[h.slot];
        if (*slot == h.fn) continue;
        e->orig[h.slot] = *slot;  // before the vtable is published, before the slot points at us
    }
    if (fresh) e->vt.store(vt, std::memory_order_release);
    for (const Hook& h : hooks) {
        void** slot = vt + g_slot[h.slot];
        if (*slot == h.fn || !e->orig[h.slot]) continue;
        DWORD old;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            Log("couldn't patch slot %d", g_slot[h.slot]);
            continue;
        }
        *slot = h.fn;
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
}

template <typename F>
void* P(F f) {
    return reinterpret_cast<void*>(f);
}

device* Dev() { return GameDevice(); }
pipeline_layout Layout(ID3D12RootSignature* rs) { return pipeline_layout{reinterpret_cast<uintptr_t>(rs)}; }
resource Res(ID3D12Resource* r) { return resource{reinterpret_cast<uintptr_t>(r)}; }

// ---------------------------------------------------------------- pipelines

struct PsoInfo {
    shader_desc sh[6];
    pipeline_subobject subs[6];
    uint32_t n = 0;
    ID3D12RootSignature* rs = nullptr;
    void Add(pipeline_subobject_type t, const D3D12_SHADER_BYTECODE& bc) {
        if (!bc.pShaderBytecode || !bc.BytecodeLength || n >= 6) return;
        sh[n].code = bc.pShaderBytecode;
        sh[n].code_size = bc.BytecodeLength;
        subs[n] = {t, 1, &sh[n]};
        ++n;
    }
};

void PipeGone(void* object, void*) { Invoke<addon_event::destroy_pipeline>(Dev(), pipeline{reinterpret_cast<uintptr_t>(object)}); }

void FirePipeline(void* pso, PsoInfo& info) {
    if (!pso || !Dev()) return;
    Invoke<addon_event::init_pipeline>(Dev(), Layout(info.rs), info.n, static_cast<const pipeline_subobject*>(info.subs),
                                       pipeline{reinterpret_cast<uintptr_t>(pso)});
    OnDestroyed(static_cast<IUnknown*>(pso), &PipeGone, nullptr);
}

void FromGfx(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, PsoInfo& info) {
    info.rs = d->pRootSignature;
    info.Add(pipeline_subobject_type::vertex_shader, d->VS);
    info.Add(pipeline_subobject_type::hull_shader, d->HS);
    info.Add(pipeline_subobject_type::domain_shader, d->DS);
    info.Add(pipeline_subobject_type::geometry_shader, d->GS);
    info.Add(pipeline_subobject_type::pixel_shader, d->PS);
}

void FromCs(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, PsoInfo& info) {
    info.rs = d->pRootSignature;
    info.Add(pipeline_subobject_type::compute_shader, d->CS);
}

// Pipeline state streams: each subobject is {type, data} aligned to a pointer (d3dx12.h's layout).
template <class T>
struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T data;
};

size_t SubSize(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t) {
    switch (t) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return sizeof(Sub<ID3D12RootSignature*>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return sizeof(Sub<D3D12_SHADER_BYTECODE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return sizeof(Sub<D3D12_STREAM_OUTPUT_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return sizeof(Sub<D3D12_BLEND_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return sizeof(Sub<UINT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return sizeof(Sub<D3D12_RASTERIZER_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return sizeof(Sub<D3D12_DEPTH_STENCIL_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return sizeof(Sub<D3D12_INPUT_LAYOUT_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return sizeof(Sub<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return sizeof(Sub<D3D12_PRIMITIVE_TOPOLOGY_TYPE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return sizeof(Sub<D3D12_RT_FORMAT_ARRAY>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return sizeof(Sub<DXGI_FORMAT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return sizeof(Sub<DXGI_SAMPLE_DESC>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return sizeof(Sub<UINT>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return sizeof(Sub<D3D12_CACHED_PIPELINE_STATE>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return sizeof(Sub<D3D12_PIPELINE_STATE_FLAGS>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return sizeof(Sub<D3D12_DEPTH_STENCIL_DESC1>);
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return sizeof(Sub<D3D12_VIEW_INSTANCING_DESC>);
        default: return 0;
    }
}

void FromStream(const D3D12_PIPELINE_STATE_STREAM_DESC* d, PsoInfo& info) {
    const uint8_t* p = static_cast<const uint8_t*>(d->pPipelineStateSubobjectStream);
    const uint8_t* end = p + d->SizeInBytes;
    while (p + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= end) {
        const auto t = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*>(p);
        const size_t n = SubSize(t);
        if (!n) {
            static std::atomic<int> warned{0};
            if (warned++ < 3) Log("pipeline stream subobject type %d not parsed", static_cast<int>(t));
            break;
        }
        const auto& bc = reinterpret_cast<const Sub<D3D12_SHADER_BYTECODE>*>(p)->data;
        switch (t) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: info.rs = reinterpret_cast<const Sub<ID3D12RootSignature*>*>(p)->data; break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: info.Add(pipeline_subobject_type::vertex_shader, bc); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: info.Add(pipeline_subobject_type::hull_shader, bc); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: info.Add(pipeline_subobject_type::domain_shader, bc); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: info.Add(pipeline_subobject_type::geometry_shader, bc); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: info.Add(pipeline_subobject_type::pixel_shader, bc); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: info.Add(pipeline_subobject_type::compute_shader, bc); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS: info.Add(pipeline_subobject_type::amplification_shader, bc); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: info.Add(pipeline_subobject_type::mesh_shader, bc); break;
            default: break;
        }
        p += n;
    }
}

HRESULT STDMETHODCALLTYPE HkLibLoadGfx(ID3D12PipelineLibrary* lib, LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID riid,
                                       void** out) {
    const HRESULT hr = O<decltype(&HkLibLoadGfx)>(lib, kLibLoadGraphicsPipeline)(lib, name, d, riid, out);
    if (SUCCEEDED(hr) && out && *out && GAME) {
        PsoInfo info;
        FromGfx(d, info);
        FirePipeline(*out, info);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkLibLoadCs(ID3D12PipelineLibrary* lib, LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID riid,
                                      void** out) {
    const HRESULT hr = O<decltype(&HkLibLoadCs)>(lib, kLibLoadComputePipeline)(lib, name, d, riid, out);
    if (SUCCEEDED(hr) && out && *out && GAME) {
        PsoInfo info;
        FromCs(d, info);
        FirePipeline(*out, info);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkLibLoad(ID3D12PipelineLibrary1* lib, LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC* d, REFIID riid,
                                    void** out) {
    const HRESULT hr = O<decltype(&HkLibLoad)>(lib, kLibLoadPipeline)(lib, name, d, riid, out);
    if (SUCCEEDED(hr) && out && *out && GAME) {
        PsoInfo info;
        FromStream(d, info);
        FirePipeline(*out, info);
    }
    return hr;
}

// ---------------------------------------------------------------- command lists

inline CommandList* L(void* list) { return ListOf(list); }

HRESULT STDMETHODCALLTYPE HkResetH(ID3D12GraphicsCommandList* l, ID3D12CommandAllocator* a, ID3D12PipelineState* pso) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkResetH)>(l, kListReset)(l, a, pso);
    if (CommandList* w = L(l)) {
        w->act = ListActual();
        if (game && SUCCEEDED(hr)) {
            Invoke<addon_event::reset_command_list>(static_cast<command_list*>(w));
            if (pso) Invoke<addon_event::bind_pipeline>(static_cast<command_list*>(w), pipeline_stage::all, pipeline{reinterpret_cast<uintptr_t>(pso)});
        }
    }
    return hr;
}

void STDMETHODCALLTYPE HkDraw(ID3D12GraphicsCommandList* l, UINT v, UINT i, UINT fv, UINT fi) {
    if (GAME)
        if (CommandList* w = L(l))
            if (Invoke<addon_event::draw>(static_cast<command_list*>(w), v, i, fv, fi)) return;
    O<decltype(&HkDraw)>(l, kListDrawInstanced)(l, v, i, fv, fi);
}

void STDMETHODCALLTYPE HkDrawIndexed(ID3D12GraphicsCommandList* l, UINT n, UINT i, UINT fi, INT vo, UINT fin) {
    if (GAME)
        if (CommandList* w = L(l))
            if (Invoke<addon_event::draw_indexed>(static_cast<command_list*>(w), n, i, fi, vo, fin)) return;
    O<decltype(&HkDrawIndexed)>(l, kListDrawIndexedInstanced)(l, n, i, fi, vo, fin);
}

void STDMETHODCALLTYPE HkDispatch(ID3D12GraphicsCommandList* l, UINT x, UINT y, UINT z) {
    if (GAME)
        if (CommandList* w = L(l))
            if (Invoke<addon_event::dispatch>(static_cast<command_list*>(w), x, y, z)) return;
    O<decltype(&HkDispatch)>(l, kListDispatch)(l, x, y, z);
}

void STDMETHODCALLTYPE HkExecuteIndirect(ID3D12GraphicsCommandList* l, ID3D12CommandSignature* sig, UINT max, ID3D12Resource* args,
                                         UINT64 argOffset, ID3D12Resource* count, UINT64 countOffset) {
    if (GAME)
        if (CommandList* w = L(l))
            if (Invoke<addon_event::draw_or_dispatch_indirect>(static_cast<command_list*>(w), CommandSignatureType(sig), Res(args), argOffset,
                                                               max, 0u))
                return;
    O<decltype(&HkExecuteIndirect)>(l, kListExecuteIndirect)(l, sig, max, args, argOffset, count, countOffset);
}

void STDMETHODCALLTYPE HkCopyBufferRegion(ID3D12GraphicsCommandList* l, ID3D12Resource* dst, UINT64 dof, ID3D12Resource* src, UINT64 so,
                                          UINT64 n) {
    if (GAME)
        if (CommandList* w = L(l))
            if (Invoke<addon_event::copy_buffer_region>(static_cast<command_list*>(w), Res(src), so, Res(dst), dof, n)) return;
    O<decltype(&HkCopyBufferRegion)>(l, kListCopyBufferRegion)(l, dst, dof, src, so, n);
}

void STDMETHODCALLTYPE HkCopyTextureRegion(ID3D12GraphicsCommandList* l, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                                           const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
    if (GAME && dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
        if (CommandList* w = L(l)) {
            subresource_box sb, db;
            if (box) {
                sb = {box->left, box->top, box->front, box->right, box->bottom, box->back};
                db = {x, y, z, x + sb.width(), y + sb.height(), z + sb.depth()};
            }
            if (Invoke<addon_event::copy_texture_region>(static_cast<command_list*>(w), Res(src->pResource), src->SubresourceIndex,
                                                         box ? &sb : static_cast<const subresource_box*>(nullptr), Res(dst->pResource),
                                                         dst->SubresourceIndex, box ? &db : static_cast<const subresource_box*>(nullptr),
                                                         filter_mode::min_mag_mip_point))
                return;
        }
    O<decltype(&HkCopyTextureRegion)>(l, kListCopyTextureRegion)(l, dst, x, y, z, src, box);
}

void STDMETHODCALLTYPE HkCopyResource(ID3D12GraphicsCommandList* l, ID3D12Resource* dst, ID3D12Resource* src) {
    if (GAME)
        if (CommandList* w = L(l))
            if (Invoke<addon_event::copy_resource>(static_cast<command_list*>(w), Res(src), Res(dst))) return;
    O<decltype(&HkCopyResource)>(l, kListCopyResource)(l, dst, src);
}

void STDMETHODCALLTYPE HkResolve(ID3D12GraphicsCommandList* l, ID3D12Resource* dst, UINT ds, ID3D12Resource* src, UINT ss, DXGI_FORMAT f) {
    if (GAME)
        if (CommandList* w = L(l))
            if (Invoke<addon_event::resolve_texture_region>(static_cast<command_list*>(w), Res(src), ss, static_cast<const subresource_box*>(nullptr),
                                                            Res(dst), ds, 0u, 0u, 0u, static_cast<format>(f)))
                return;
    O<decltype(&HkResolve)>(l, kListResolveSubresource)(l, dst, ds, src, ss, f);
}

void STDMETHODCALLTYPE HkViewports(ID3D12GraphicsCommandList* l, UINT n, const D3D12_VIEWPORT* vps) {
    const bool game = GAME;
    O<decltype(&HkViewports)>(l, kListRSSetViewports)(l, n, vps);
    if (game)
        if (CommandList* w = L(l)) Invoke<addon_event::bind_viewports>(static_cast<command_list*>(w), 0u, n, reinterpret_cast<const viewport*>(vps));
}

void STDMETHODCALLTYPE HkScissors(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RECT* rects) {
    const bool game = GAME;
    O<decltype(&HkScissors)>(l, kListRSSetScissorRects)(l, n, rects);
    if (game)
        if (CommandList* w = L(l)) Invoke<addon_event::bind_scissor_rects>(static_cast<command_list*>(w), 0u, n, reinterpret_cast<const rect*>(rects));
}

void STDMETHODCALLTYPE HkSetPso(ID3D12GraphicsCommandList* l, ID3D12PipelineState* pso) {
    const bool game = GAME;
    O<decltype(&HkSetPso)>(l, kListSetPipelineState)(l, pso);
    if (game)
        if (CommandList* w = L(l))
            Invoke<addon_event::bind_pipeline>(static_cast<command_list*>(w), pipeline_stage::all, pipeline{reinterpret_cast<uintptr_t>(pso)});
}

void STDMETHODCALLTYPE HkBarrier(ID3D12GraphicsCommandList* l, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    const bool game = GAME;
    O<decltype(&HkBarrier)>(l, kListResourceBarrier)(l, n, b);
    if (!game || !Has(addon_event::barrier)) return;
    CommandList* w = L(l);
    if (!w) return;
    resource res[64];
    resource_usage olds[64], news[64];
    uint32_t k = 0;
    for (UINT i = 0; i < n; ++i) {
        if (k == 64) {
            Invoke<addon_event::barrier>(static_cast<command_list*>(w), k, static_cast<const resource*>(res), static_cast<const resource_usage*>(olds),
                                         static_cast<const resource_usage*>(news));
            k = 0;
        }
        if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            res[k] = Res(b[i].Transition.pResource);
            olds[k] = ToUsage(b[i].Transition.StateBefore);
            news[k] = ToUsage(b[i].Transition.StateAfter);
            ++k;
        } else if (b[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            res[k] = Res(b[i].UAV.pResource);
            olds[k] = news[k] = resource_usage::unordered_access;
            ++k;
        }
    }
    if (k)
        Invoke<addon_event::barrier>(static_cast<command_list*>(w), k, static_cast<const resource*>(res), static_cast<const resource_usage*>(olds),
                                     static_cast<const resource_usage*>(news));
}

void STDMETHODCALLTYPE HkSetHeaps(ID3D12GraphicsCommandList* l, UINT n, ID3D12DescriptorHeap* const* heaps) {
    const bool game = GAME;
    O<decltype(&HkSetHeaps)>(l, kListSetDescriptorHeaps)(l, n, heaps);
    if (CommandList* w = L(l)) {
        w->act.heapCount = std::min<UINT>(n, 2);
        for (UINT i = 0; i < w->act.heapCount; ++i) w->act.heaps[i] = heaps[i];
        if (game) {
            w->act.gameHeapCount = w->act.heapCount;
            for (UINT i = 0; i < w->act.heapCount; ++i) w->act.gameHeaps[i] = heaps[i];
        }
    }
}

void STDMETHODCALLTYPE HkSetComputeRoot(ID3D12GraphicsCommandList* l, ID3D12RootSignature* rs) {
    O<decltype(&HkSetComputeRoot)>(l, kListSetComputeRootSignature)(l, rs);
    if (CommandList* w = L(l)) w->act.root[1] = rs;
}

void STDMETHODCALLTYPE HkSetGraphicsRoot(ID3D12GraphicsCommandList* l, ID3D12RootSignature* rs) {
    O<decltype(&HkSetGraphicsRoot)>(l, kListSetGraphicsRootSignature)(l, rs);
    if (CommandList* w = L(l)) w->act.root[0] = rs;
}

constexpr shader_stage kStage[2] = {shader_stage::all_graphics, shader_stage::all_compute};

void BindTable(ID3D12GraphicsCommandList* l, int which, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    if (CommandList* w = L(l)) {
        const descriptor_table t = {h.ptr};
        Invoke<addon_event::bind_descriptor_tables>(static_cast<command_list*>(w), kStage[which], Layout(w->act.root[which]), param, 1u,
                                                    static_cast<const descriptor_table*>(&t), 0u, static_cast<const uint32_t*>(nullptr));
    }
}

void STDMETHODCALLTYPE HkComputeTable(ID3D12GraphicsCommandList* l, UINT p, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    const bool game = GAME;
    O<decltype(&HkComputeTable)>(l, kListSetComputeRootDescriptorTable)(l, p, h);
    if (game) BindTable(l, 1, p, h);
}

void STDMETHODCALLTYPE HkGraphicsTable(ID3D12GraphicsCommandList* l, UINT p, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    const bool game = GAME;
    O<decltype(&HkGraphicsTable)>(l, kListSetGraphicsRootDescriptorTable)(l, p, h);
    if (game) BindTable(l, 0, p, h);
}

void Constants(ID3D12GraphicsCommandList* l, int which, UINT p, UINT first, UINT count, const void* values) {
    if (CommandList* w = L(l))
        Invoke<addon_event::push_constants>(static_cast<command_list*>(w), kStage[which], Layout(w->act.root[which]), p, first, count, values);
}

void STDMETHODCALLTYPE HkComputeConst(ID3D12GraphicsCommandList* l, UINT p, UINT v, UINT off) {
    const bool game = GAME;
    O<decltype(&HkComputeConst)>(l, kListSetComputeRoot32BitConstant)(l, p, v, off);
    if (game) Constants(l, 1, p, off, 1, &v);
}

void STDMETHODCALLTYPE HkGraphicsConst(ID3D12GraphicsCommandList* l, UINT p, UINT v, UINT off) {
    const bool game = GAME;
    O<decltype(&HkGraphicsConst)>(l, kListSetGraphicsRoot32BitConstant)(l, p, v, off);
    if (game) Constants(l, 0, p, off, 1, &v);
}

void STDMETHODCALLTYPE HkComputeConsts(ID3D12GraphicsCommandList* l, UINT p, UINT n, const void* d, UINT off) {
    const bool game = GAME;
    O<decltype(&HkComputeConsts)>(l, kListSetComputeRoot32BitConstants)(l, p, n, d, off);
    if (game) Constants(l, 1, p, off, n, d);
}

void STDMETHODCALLTYPE HkGraphicsConsts(ID3D12GraphicsCommandList* l, UINT p, UINT n, const void* d, UINT off) {
    const bool game = GAME;
    O<decltype(&HkGraphicsConsts)>(l, kListSetGraphicsRoot32BitConstants)(l, p, n, d, off);
    if (game) Constants(l, 0, p, off, n, d);
}

void RootView(ID3D12GraphicsCommandList* l, int which, UINT p, D3D12_GPU_VIRTUAL_ADDRESS va, descriptor_type type) {
    CommandList* w = L(l);
    if (!w) return;
    uint64_t off = 0;
    ID3D12Resource* r = ResourceAtVa(va, &off);
    buffer_range b;
    b.buffer = Res(r);
    b.offset = off;
    b.size = UINT64_MAX;
    descriptor_table_update u = {};
    u.count = 1;
    u.type = type;
    u.descriptors = &b;
    Invoke<addon_event::push_descriptors>(static_cast<command_list*>(w), kStage[which], Layout(w->act.root[which]), p,
                                          static_cast<const descriptor_table_update&>(u));
}

#define ROOT_VIEW_HOOK(Name, slot, which, type)                                                    \
    void STDMETHODCALLTYPE Name(ID3D12GraphicsCommandList* l, UINT p, D3D12_GPU_VIRTUAL_ADDRESS va) { \
        const bool game = GAME;                                                                    \
        O<decltype(&Name)>(l, slot)(l, p, va);                                                     \
        if (game) RootView(l, which, p, va, type);                                                 \
    }
ROOT_VIEW_HOOK(HkComputeCbv, kListSetComputeRootConstantBufferView, 1, descriptor_type::constant_buffer)
ROOT_VIEW_HOOK(HkGraphicsCbv, kListSetGraphicsRootConstantBufferView, 0, descriptor_type::constant_buffer)
ROOT_VIEW_HOOK(HkComputeSrv, kListSetComputeRootShaderResourceView, 1, descriptor_type::buffer_shader_resource_view)
ROOT_VIEW_HOOK(HkGraphicsSrv, kListSetGraphicsRootShaderResourceView, 0, descriptor_type::buffer_shader_resource_view)
ROOT_VIEW_HOOK(HkComputeUav, kListSetComputeRootUnorderedAccessView, 1, descriptor_type::buffer_unordered_access_view)
ROOT_VIEW_HOOK(HkGraphicsUav, kListSetGraphicsRootUnorderedAccessView, 0, descriptor_type::buffer_unordered_access_view)

void STDMETHODCALLTYPE HkSetTargets(ID3D12GraphicsCommandList* l, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single,
                                    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    const bool game = GAME;
    O<decltype(&HkSetTargets)>(l, kListOMSetRenderTargets)(l, n, rtvs, single, dsv);
    if (!game) return;
    CommandList* w = L(l);
    if (!w) return;
    resource_view v[8] = {};
    n = std::min<UINT>(n, 8);
    const uint32_t inc = GameDevice() ? static_cast<Device*>(GameDevice())->Inc(D3D12_DESCRIPTOR_HEAP_TYPE_RTV) : 0;
    for (UINT i = 0; i < n; ++i) v[i].handle = single ? rtvs[0].ptr + uint64_t(i) * inc : rtvs[i].ptr;
    Invoke<addon_event::bind_render_targets_and_depth_stencil>(static_cast<command_list*>(w), static_cast<uint32_t>(n),
                                                               static_cast<const resource_view*>(v), resource_view{dsv ? dsv->ptr : 0});
}

void HookList(void* list) {
    Patch(list, {{kListReset, P(&HkResetH)},
                 {kListDrawInstanced, P(&HkDraw)},
                 {kListDrawIndexedInstanced, P(&HkDrawIndexed)},
                 {kListDispatch, P(&HkDispatch)},
                 {kListCopyBufferRegion, P(&HkCopyBufferRegion)},
                 {kListCopyTextureRegion, P(&HkCopyTextureRegion)},
                 {kListCopyResource, P(&HkCopyResource)},
                 {kListResolveSubresource, P(&HkResolve)},
                 {kListRSSetViewports, P(&HkViewports)},
                 {kListRSSetScissorRects, P(&HkScissors)},
                 {kListSetPipelineState, P(&HkSetPso)},
                 {kListResourceBarrier, P(&HkBarrier)},
                 {kListSetDescriptorHeaps, P(&HkSetHeaps)},
                 {kListSetComputeRootSignature, P(&HkSetComputeRoot)},
                 {kListSetGraphicsRootSignature, P(&HkSetGraphicsRoot)},
                 {kListSetComputeRootDescriptorTable, P(&HkComputeTable)},
                 {kListSetGraphicsRootDescriptorTable, P(&HkGraphicsTable)},
                 {kListSetComputeRoot32BitConstant, P(&HkComputeConst)},
                 {kListSetGraphicsRoot32BitConstant, P(&HkGraphicsConst)},
                 {kListSetComputeRoot32BitConstants, P(&HkComputeConsts)},
                 {kListSetGraphicsRoot32BitConstants, P(&HkGraphicsConsts)},
                 {kListSetComputeRootConstantBufferView, P(&HkComputeCbv)},
                 {kListSetGraphicsRootConstantBufferView, P(&HkGraphicsCbv)},
                 {kListSetComputeRootShaderResourceView, P(&HkComputeSrv)},
                 {kListSetGraphicsRootShaderResourceView, P(&HkGraphicsSrv)},
                 {kListSetComputeRootUnorderedAccessView, P(&HkComputeUav)},
                 {kListSetGraphicsRootUnorderedAccessView, P(&HkGraphicsUav)},
                 {kListOMSetRenderTargets, P(&HkSetTargets)},
                 {kListExecuteIndirect, P(&HkExecuteIndirect)}});
}

void ListGone(void* object, void* ctx) {
    if (CommandList* w = static_cast<CommandList*>(ctx)) Invoke<addon_event::destroy_command_list>(static_cast<command_list*>(w));
    (void)object;
}

void NewList(void** out, bool game) {
    if (!out || !*out) return;
    ID3D12GraphicsCommandList* l = nullptr;
    if (FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&l)))) return;
    l->Release();  // the game holds it
    if (l->GetType() == D3D12_COMMAND_LIST_TYPE_BUNDLE) return;
    HookList(l);
    if (!game || L(l)) return;
    CommandList* w = CreateListWrapper(l);
    if (!w) return;
    OnDestroyed(l, &ListGone, w);
    Invoke<addon_event::init_command_list>(static_cast<command_list*>(w));
}

// ---------------------------------------------------------------- queues

Queue* g_firstDirectQueue = nullptr;

void STDMETHODCALLTYPE HkExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (GAME && Has(addon_event::execute_command_list))
        if (Queue* qw = QueueOf(q))
            for (UINT i = 0; i < n; ++i)
                if (CommandList* w = L(lists[i]))
                    Invoke<addon_event::execute_command_list>(static_cast<command_queue*>(qw), static_cast<command_list*>(w));
    O<decltype(&HkExecute)>(q, kQueueExecuteCommandLists)(q, n, lists);
}

// ---------------------------------------------------------------- device

HRESULT STDMETHODCALLTYPE HkCreateQueue(ID3D12Device* d, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCreateQueue)>(d, kDevCreateCommandQueue)(d, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && game) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&q)))) {
            q->Release();
            Patch(q, {{kQueueExecuteCommandLists, P(&HkExecute)}});
            if (!QueueOf(q)) {
                Queue* w = CreateQueueWrapper(q, desc->Type);
                if (w && desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT && !g_firstDirectQueue) g_firstDirectQueue = w;
                if (w) Invoke<addon_event::init_command_queue>(static_cast<command_queue*>(w));
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateList(ID3D12Device* d, UINT mask, D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator* a, ID3D12PipelineState* pso,
                                       REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCreateList)>(d, kDevCreateCommandList)(d, mask, t, a, pso, riid, out);
    if (SUCCEEDED(hr)) {
        NewList(out, game);
        if (game && pso && out && *out)
            if (CommandList* w = L(*out))
                Invoke<addon_event::bind_pipeline>(static_cast<command_list*>(w), pipeline_stage::all, pipeline{reinterpret_cast<uintptr_t>(pso)});
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateList1(ID3D12Device4* d, UINT mask, D3D12_COMMAND_LIST_TYPE t, D3D12_COMMAND_LIST_FLAGS f, REFIID riid,
                                        void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCreateList1)>(d, kDevCreateCommandList1)(d, mask, t, f, riid, out);
    if (SUCCEEDED(hr)) NewList(out, game);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateGfx(ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCreateGfx)>(d, kDevCreateGraphicsPipelineState)(d, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && game) {
        PsoInfo info;
        FromGfx(desc, info);
        FirePipeline(*out, info);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateCs(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCreateCs)>(d, kDevCreateComputePipelineState)(d, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && game) {
        PsoInfo info;
        FromCs(desc, info);
        FirePipeline(*out, info);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreatePso(ID3D12Device2* d, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCreatePso)>(d, kDevCreatePipelineState)(d, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && game) {
        PsoInfo info;
        FromStream(desc, info);
        FirePipeline(*out, info);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateLib(ID3D12Device1* d, const void* blob, SIZE_T size, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCreateLib)>(d, kDevCreatePipelineLibrary)(d, blob, size, riid, out);
    if (SUCCEEDED(hr) && out && *out && game) {
        IUnknown* lib = static_cast<IUnknown*>(*out);
        Patch(lib, {{kLibLoadGraphicsPipeline, P(&HkLibLoadGfx)}, {kLibLoadComputePipeline, P(&HkLibLoadCs)}});
        ID3D12PipelineLibrary1* lib1 = nullptr;
        if (SUCCEEDED(lib->QueryInterface(IID_PPV_ARGS(&lib1)))) {
            Patch(lib1, {{kLibLoadPipeline, P(&HkLibLoad)}});
            lib1->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateHeap(ID3D12Device* d, const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCreateHeap)>(d, kDevCreateDescriptorHeap)(d, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out && game) {
        ID3D12DescriptorHeap* h = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&h)))) {
            h->Release();
            TrackHeap(h, false);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateRootSig(ID3D12Device* d, UINT mask, const void* blob, SIZE_T size, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCreateRootSig)>(d, kDevCreateRootSignature)(d, mask, blob, size, riid, out);
    if (SUCCEEDED(hr) && out && *out && game) {
        ID3D12RootSignature* rs = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&rs)))) {
            rs->Release();
            TrackRootSignature(rs, blob, size);
        }
    }
    return hr;
}

bool ShaderVisible(uint64_t cpu) {
    uint32_t i = 0;
    HeapInfo* h = HeapOfCpu(cpu, &i);
    return h && h->gpu;
}

void ViewSlot(uint64_t h, ID3D12Resource* r, descriptor_type type, const resource_view_desc& vd) {
    Slot* s = SlotAt(h);
    if (!s) return;
    Slot x;
    x.used = true;
    x.origin = h;
    x.res = r;
    x.type = type;
    x.format = static_cast<uint32_t>(vd.format);
    x.dim = static_cast<uint8_t>(vd.type);
    *s = x;
}

void STDMETHODCALLTYPE HkCreateCbv(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const bool game = GAME;
    O<decltype(&HkCreateCbv)>(d, kDevCreateConstantBufferView)(d, desc, h);
    if (!game) return;
    Slot* s = SlotAt(h.ptr);
    if (!s) return;
    Slot x;
    x.used = true;
    x.type = descriptor_type::constant_buffer;
    if (desc) {
        x.res = ResourceAtVa(desc->BufferLocation, &x.offset);
        x.size = desc->SizeInBytes;
    }
    *s = x;
    if (desc && ShaderVisible(h.ptr)) {
        FullView fv;
        fv.kind = 3;
        fv.cbv = *desc;
        fv.res = x.res;
        RememberFullView(h.ptr, fv);
    }
}

void STDMETHODCALLTYPE HkCreateSrv(ID3D12Device* d, ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const bool game = GAME;
    O<decltype(&HkCreateSrv)>(d, kDevCreateShaderResourceView)(d, r, desc, h);
    if (!game) return;
    const resource_view_desc vd = desc ? ToViewDesc(*desc) : DefaultViewDesc(r, 1);
    ViewSlot(h.ptr, r, vd.type == resource_view_type::buffer ? descriptor_type::buffer_shader_resource_view : descriptor_type::shader_resource_view, vd);
    if (ShaderVisible(h.ptr)) {
        FullView fv;
        fv.kind = 1;
        fv.res = r;
        fv.hasDesc = desc != nullptr;
        if (desc) fv.srv = *desc;
        RememberFullView(h.ptr, fv);
    }
    if (Has(addon_event::init_resource_view))
        Invoke<addon_event::init_resource_view>(Dev(), Res(r), resource_usage::shader_resource, vd, resource_view{h.ptr});
}

void STDMETHODCALLTYPE HkCreateUav(ID3D12Device* d, ID3D12Resource* r, ID3D12Resource* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                   D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const bool game = GAME;
    O<decltype(&HkCreateUav)>(d, kDevCreateUnorderedAccessView)(d, r, counter, desc, h);
    if (!game) return;
    const resource_view_desc vd = desc ? ToViewDesc(*desc) : DefaultViewDesc(r, 2);
    ViewSlot(h.ptr, r, vd.type == resource_view_type::buffer ? descriptor_type::buffer_unordered_access_view : descriptor_type::unordered_access_view, vd);
    if (ShaderVisible(h.ptr)) {
        FullView fv;
        fv.kind = 2;
        fv.res = r;
        fv.counter = counter;
        fv.hasDesc = desc != nullptr;
        if (desc) fv.uav = *desc;
        RememberFullView(h.ptr, fv);
    }
    if (Has(addon_event::init_resource_view))
        Invoke<addon_event::init_resource_view>(Dev(), Res(r), resource_usage::unordered_access, vd, resource_view{h.ptr});
}

void STDMETHODCALLTYPE HkCreateRtv(ID3D12Device* d, ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const bool game = GAME;
    O<decltype(&HkCreateRtv)>(d, kDevCreateRenderTargetView)(d, r, desc, h);
    if (!game) return;
    const resource_view_desc vd = desc ? ToViewDesc(*desc) : DefaultViewDesc(r, 3);
    ViewSlot(h.ptr, r, descriptor_type::shader_resource_view, vd);
    if (Has(addon_event::init_resource_view))
        Invoke<addon_event::init_resource_view>(Dev(), Res(r), resource_usage::render_target, vd, resource_view{h.ptr});
}

void STDMETHODCALLTYPE HkCreateDsv(ID3D12Device* d, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const bool game = GAME;
    O<decltype(&HkCreateDsv)>(d, kDevCreateDepthStencilView)(d, r, desc, h);
    if (!game) return;
    const resource_view_desc vd = desc ? ToViewDesc(*desc) : DefaultViewDesc(r, 4);
    ViewSlot(h.ptr, r, descriptor_type::shader_resource_view, vd);
    if (Has(addon_event::init_resource_view))
        Invoke<addon_event::init_resource_view>(Dev(), Res(r), resource_usage::depth_stencil, vd, resource_view{h.ptr});
}

void STDMETHODCALLTYPE HkCreateSampler(ID3D12Device* d, const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const bool game = GAME;
    if (!game || !desc || !Dev()) {
        O<decltype(&HkCreateSampler)>(d, kDevCreateSampler)(d, desc, h);
        return;
    }
    sampler_desc sd = ToSamplerDesc(*desc);
    D3D12_SAMPLER_DESC changed;
    const D3D12_SAMPLER_DESC* use = desc;
    if (Invoke<addon_event::create_sampler>(Dev(), sd)) {
        changed = FromSamplerDesc(sd);
        use = &changed;
    }
    O<decltype(&HkCreateSampler)>(d, kDevCreateSampler)(d, use, h);
    if (Slot* s = SlotAt(h.ptr)) {
        Slot x;
        x.used = true;
        x.type = descriptor_type::sampler;
        x.origin = h.ptr;
        *s = x;
    }
    Invoke<addon_event::init_sampler>(Dev(), static_cast<const sampler_desc&>(sd), sampler{h.ptr});
}

void STDMETHODCALLTYPE HkCopyDescs(ID3D12Device* d, UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* ds, const UINT* dsz, UINT ns,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* ss, const UINT* ssz, D3D12_DESCRIPTOR_HEAP_TYPE t) {
    const bool game = GAME;
    O<decltype(&HkCopyDescs)>(d, kDevCopyDescriptors)(d, nd, ds, dsz, ns, ss, ssz, t);
    if (!game) return;
    // Both sides are lists of ranges over the same descriptors.
    const uint32_t inc = Dev() ? static_cast<Device*>(Dev())->Inc(t) : 0;
    UINT di = 0, si = 0, dOff = 0, sOff = 0;
    while (di < nd && si < ns) {
        const UINT dn = dsz ? dsz[di] : 1, sn = ssz ? ssz[si] : 1;
        const UINT run = std::min(dn - dOff, sn - sOff);
        if (run) CopySlots(ds[di].ptr + uint64_t(dOff) * inc, ss[si].ptr + uint64_t(sOff) * inc, run, inc);
        dOff += run, sOff += run;
        if (dOff >= dn) ++di, dOff = 0;
        if (sOff >= sn) ++si, sOff = 0;
    }
}

void STDMETHODCALLTYPE HkCopyDescsSimple(ID3D12Device* d, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                                         D3D12_DESCRIPTOR_HEAP_TYPE t) {
    const bool game = GAME;
    O<decltype(&HkCopyDescsSimple)>(d, kDevCopyDescriptorsSimple)(d, n, dst, src, t);
    if (game) CopySlots(dst.ptr, src.ptr, n, 0);
}

void NewResource(void** out, bool game, D3D12_RESOURCE_STATES state) {
    if (!game || !out || !*out) return;
    ID3D12Resource* r = nullptr;
    if (FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&r)))) return;
    r->Release();
    TrackResource(r, true, state);
}

HRESULT STDMETHODCALLTYPE HkCommitted(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC* desc,
                                      D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCommitted)>(d, kDevCreateCommittedResource)(d, hp, f, desc, s, c, riid, out);
    if (SUCCEEDED(hr)) NewResource(out, game, s);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkPlaced(ID3D12Device* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s,
                                   const D3D12_CLEAR_VALUE* c, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkPlaced)>(d, kDevCreatePlacedResource)(d, heap, off, desc, s, c, riid, out);
    if (SUCCEEDED(hr)) NewResource(out, game, s);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkReserved(ID3D12Device* d, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c,
                                     REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkReserved)>(d, kDevCreateReservedResource)(d, desc, s, c, riid, out);
    if (SUCCEEDED(hr)) NewResource(out, game, s);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCommitted1(ID3D12Device4* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC* desc,
                                       D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, ID3D12ProtectedResourceSession* ps, REFIID riid,
                                       void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCommitted1)>(d, kDevCreateCommittedResource1)(d, hp, f, desc, s, c, ps, riid, out);
    if (SUCCEEDED(hr)) NewResource(out, game, s);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkReserved1(ID3D12Device4* d, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c,
                                      ID3D12ProtectedResourceSession* ps, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkReserved1)>(d, kDevCreateReservedResource1)(d, desc, s, c, ps, riid, out);
    if (SUCCEEDED(hr)) NewResource(out, game, s);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCommitted2(ID3D12Device8* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC1* desc,
                                       D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, ID3D12ProtectedResourceSession* ps, REFIID riid,
                                       void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCommitted2)>(d, kDevCreateCommittedResource2)(d, hp, f, desc, s, c, ps, riid, out);
    if (SUCCEEDED(hr)) NewResource(out, game, s);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkPlaced1(ID3D12Device8* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES s,
                                    const D3D12_CLEAR_VALUE* c, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkPlaced1)>(d, kDevCreatePlacedResource1)(d, heap, off, desc, s, c, riid, out);
    if (SUCCEEDED(hr)) NewResource(out, game, s);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCommitted3(ID3D12Device10* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC1* desc,
                                       D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* c, ID3D12ProtectedResourceSession* ps, UINT nc,
                                       const DXGI_FORMAT* cf, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkCommitted3)>(d, kDevCreateCommittedResource3)(d, hp, f, desc, layout, c, ps, nc, cf, riid, out);
    if (SUCCEEDED(hr)) NewResource(out, game, D3D12_RESOURCE_STATE_COMMON);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkPlaced2(ID3D12Device10* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
                                    const D3D12_CLEAR_VALUE* c, UINT nc, const DXGI_FORMAT* cf, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkPlaced2)>(d, kDevCreatePlacedResource2)(d, heap, off, desc, layout, c, nc, cf, riid, out);
    if (SUCCEEDED(hr)) NewResource(out, game, D3D12_RESOURCE_STATE_COMMON);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkReserved2(ID3D12Device10* d, const D3D12_RESOURCE_DESC* desc, D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* c,
                                      ID3D12ProtectedResourceSession* ps, UINT nc, const DXGI_FORMAT* cf, REFIID riid, void** out) {
    const bool game = GAME;
    const HRESULT hr = O<decltype(&HkReserved2)>(d, kDevCreateReservedResource2)(d, desc, layout, c, ps, nc, cf, riid, out);
    if (SUCCEEDED(hr)) NewResource(out, game, D3D12_RESOURCE_STATE_COMMON);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateSig(ID3D12Device* d, const D3D12_COMMAND_SIGNATURE_DESC* desc, ID3D12RootSignature* rs, REFIID riid, void** out) {
    const HRESULT hr = O<decltype(&HkCreateSig)>(d, kDevCreateCommandSignature)(d, desc, rs, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12CommandSignature* s = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&s)))) {
            s->Release();
            TrackCommandSignature(s, desc);
        }
    }
    return hr;
}

void DeviceGone(void*, void*) {
    if (Dev()) Invoke<addon_event::destroy_device>(Dev());
}

template <typename I>
bool Supports(ID3D12Device* d) {
    I* x = nullptr;
    if (FAILED(d->QueryInterface(IID_PPV_ARGS(&x)))) return false;
    x->Release();
    return true;
}

void HookDevice(ID3D12Device* d) {
    Patch(d, {{kDevCreateCommandQueue, P(&HkCreateQueue)},
              {kDevCreateCommandList, P(&HkCreateList)},
              {kDevCreateGraphicsPipelineState, P(&HkCreateGfx)},
              {kDevCreateComputePipelineState, P(&HkCreateCs)},
              {kDevCreateDescriptorHeap, P(&HkCreateHeap)},
              {kDevCreateRootSignature, P(&HkCreateRootSig)},
              {kDevCreateConstantBufferView, P(&HkCreateCbv)},
              {kDevCreateShaderResourceView, P(&HkCreateSrv)},
              {kDevCreateUnorderedAccessView, P(&HkCreateUav)},
              {kDevCreateRenderTargetView, P(&HkCreateRtv)},
              {kDevCreateDepthStencilView, P(&HkCreateDsv)},
              {kDevCreateSampler, P(&HkCreateSampler)},
              {kDevCopyDescriptors, P(&HkCopyDescs)},
              {kDevCopyDescriptorsSimple, P(&HkCopyDescsSimple)},
              {kDevCreateCommittedResource, P(&HkCommitted)},
              {kDevCreatePlacedResource, P(&HkPlaced)},
              {kDevCreateReservedResource, P(&HkReserved)},
              {kDevCreateCommandSignature, P(&HkCreateSig)}});
    std::string versions = "0";
    if (Supports<ID3D12Device1>(d)) Patch(d, {{kDevCreatePipelineLibrary, P(&HkCreateLib)}}), versions += " 1";
    if (Supports<ID3D12Device2>(d)) Patch(d, {{kDevCreatePipelineState, P(&HkCreatePso)}}), versions += " 2";
    if (Supports<ID3D12Device4>(d))
        Patch(d, {{kDevCreateCommandList1, P(&HkCreateList1)}, {kDevCreateCommittedResource1, P(&HkCommitted1)}, {kDevCreateReservedResource1, P(&HkReserved1)}}),
            versions += " 4";
    if (Supports<ID3D12Device8>(d))
        Patch(d, {{kDevCreateCommittedResource2, P(&HkCommitted2)}, {kDevCreatePlacedResource1, P(&HkPlaced1)}}), versions += " 8";
    if (Supports<ID3D12Device10>(d))
        Patch(d, {{kDevCreateCommittedResource3, P(&HkCommitted3)}, {kDevCreatePlacedResource2, P(&HkPlaced2)}, {kDevCreateReservedResource2, P(&HkReserved2)}}),
            versions += " 10";
    Log("device hooked (interfaces %s)", versions.c_str());
}

// ---------------------------------------------------------------- swapchains

CreateSwapChainFn g_fgCreate = nullptr;
CreateSwapChainForHwndFn g_fgCreateForHwnd = nullptr;
thread_local int t_scDepth = 0;

color_space FromDxgi(DXGI_COLOR_SPACE_TYPE cs) {
    switch (cs) {
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return color_space::scrgb;
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return color_space::hdr10_pq;
        case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020: return color_space::hdr10_hlg;
        default: return color_space::srgb;
    }
}

HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (Swapchain* w = SwapchainOf(sc))
        if (!(flags & DXGI_PRESENT_TEST))
            Invoke<addon_event::present>(static_cast<command_queue*>(w->queue()), static_cast<swapchain*>(w), static_cast<const rect*>(nullptr),
                                         static_cast<const rect*>(nullptr), 0u, static_cast<const rect*>(nullptr));
    return O<decltype(&HkPresent)>(sc, kScPresent)(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE HkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (Swapchain* w = SwapchainOf(sc))
        if (!(flags & DXGI_PRESENT_TEST))
            Invoke<addon_event::present>(static_cast<command_queue*>(w->queue()), static_cast<swapchain*>(w), static_cast<const rect*>(nullptr),
                                         static_cast<const rect*>(nullptr), 0u, static_cast<const rect*>(nullptr));
    return O<decltype(&HkPresent1)>(sc, kScPresent1)(sc, sync, flags, p);
}

HRESULT STDMETHODCALLTYPE HkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags) {
    Swapchain* sw = SwapchainOf(sc);
    if (sw) Invoke<addon_event::destroy_swapchain>(static_cast<swapchain*>(sw), true);
    const HRESULT hr = O<decltype(&HkResize)>(sc, kScResizeBuffers)(sc, n, w, h, f, flags);
    if (sw) Log("swapchain resized: %u buffers %ux%u format %d -> 0x%08lx", n, w, h, f, static_cast<unsigned long>(hr));
    if (sw && SUCCEEDED(hr)) Invoke<addon_event::init_swapchain>(static_cast<swapchain*>(sw), true);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkResize1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags, const UINT* mask, IUnknown* const* queues) {
    Swapchain* sw = SwapchainOf(sc);
    if (sw) Invoke<addon_event::destroy_swapchain>(static_cast<swapchain*>(sw), true);
    const HRESULT hr = O<decltype(&HkResize1)>(sc, kScResizeBuffers1)(sc, n, w, h, f, flags, mask, queues);
    if (sw) Log("swapchain resized (1): %u buffers %ux%u format %d -> 0x%08lx", n, w, h, f, static_cast<unsigned long>(hr));
    if (sw && SUCCEEDED(hr)) Invoke<addon_event::init_swapchain>(static_cast<swapchain*>(sw), true);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkSetColorSpace(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE cs) {
    const HRESULT hr = O<decltype(&HkSetColorSpace)>(sc, kScSetColorSpace1)(sc, cs);
    if (SUCCEEDED(hr))
        if (Swapchain* w = SwapchainOf(sc)) {
            w->colorSpace = FromDxgi(cs);
            Log("swapchain colour space %d", static_cast<int>(cs));
        }
    return hr;
}

void SwapchainGone(void*, void* ctx) {
    if (Swapchain* w = static_cast<Swapchain*>(ctx)) Invoke<addon_event::destroy_swapchain>(static_cast<swapchain*>(w), false);
}

void NewSwapchain(IUnknown* obj, IUnknown* queue, HWND wnd) {
    IDXGISwapChain* sc = nullptr;
    if (!obj || FAILED(obj->QueryInterface(IID_PPV_ARGS(&sc)))) return;
    sc->Release();
    if (SwapchainOf(sc) || !GameDevice()) return;
    // With ReShade installed it unwraps the game's queue before creating the swapchain: then the
    // game's first direct queue stands in for it.
    IUnknown* q = QueueOf(queue) ? queue : (g_firstDirectQueue ? reinterpret_cast<IUnknown*>(g_firstDirectQueue->d3d()) : queue);
    Swapchain* w = CreateSwapchainWrapper(sc, q, wnd);
    if (!w) return;
    Patch(sc, {{kScPresent, P(&HkPresent)}, {kScResizeBuffers, P(&HkResize)}});
    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        Patch(sc3, {{kScPresent1, P(&HkPresent1)}, {kScResizeBuffers1, P(&HkResize1)}, {kScSetColorSpace1, P(&HkSetColorSpace)}});
        sc3->Release();
    }
    OnDestroyedDxgi(sc, &SwapchainGone, w);
    void** vt = *reinterpret_cast<void***>(sc);
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(vt[0]), &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    Log("swapchain created (%s), queue %s", strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name, QueueOf(q) ? "known" : "unknown");
    Invoke<addon_event::init_swapchain>(static_cast<swapchain*>(w), false);
}

HRESULT STDMETHODCALLTYPE HkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    auto orig = O<decltype(&HkCreateSwapChain)>(f, kFactoryCreateSwapChain);
    if (t_scDepth > 0) return orig(f, dev, desc, out);
    ++t_scDepth;
    const HRESULT hr = g_fgCreate ? g_fgCreate(f, dev, desc, out) : orig(f, dev, desc, out);
    --t_scDepth;
    if (SUCCEEDED(hr) && out && *out) NewSwapchain(*out, dev, desc ? desc->OutputWindow : nullptr);
    return hr;
}

HRESULT STDMETHODCALLTYPE HkCreateForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND wnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output, IDXGISwapChain1** out) {
    auto orig = O<decltype(&HkCreateForHwnd)>(f, kFactoryCreateSwapChainForHwnd);
    if (t_scDepth > 0) return orig(f, dev, wnd, desc, fs, output, out);
    ++t_scDepth;
    const HRESULT hr = g_fgCreateForHwnd ? g_fgCreateForHwnd(f, dev, wnd, desc, fs, output, out) : orig(f, dev, wnd, desc, fs, output, out);
    --t_scDepth;
    if (SUCCEEDED(hr) && out && *out) NewSwapchain(*out, dev, wnd);
    return hr;
}

// The original swapchain creation for whichever factory class it's called on (handed to framegen).
HRESULT STDMETHODCALLTYPE OrigCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    auto orig = O<decltype(&HkCreateSwapChain)>(f, kFactoryCreateSwapChain);
    return orig ? orig(f, dev, desc, out) : f->CreateSwapChain(dev, desc, out);
}

HRESULT STDMETHODCALLTYPE OrigCreateForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND wnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output, IDXGISwapChain1** out) {
    auto orig = O<decltype(&HkCreateForHwnd)>(f, kFactoryCreateSwapChainForHwnd);
    return orig ? orig(f, dev, wnd, desc, fs, output, out) : f->CreateSwapChainForHwnd(dev, wnd, desc, fs, output, out);
}

// The system DXGI factory's swapchain creation, hooked once (before the game makes its factory:
// with ReShade installed, its factory creates the swapchain through the system one).
std::once_flag g_factoryOnce;
void HookSystemFactory() {
    std::call_once(g_factoryOnce, [] {
        wchar_t sys[MAX_PATH] = {};
        GetSystemDirectoryW(sys, MAX_PATH);
        HMODULE dxgi = LoadLibraryW((std::wstring(sys) + L"\\dxgi.dll").c_str());
        using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
        Fn create = dxgi ? reinterpret_cast<Fn>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
        IDXGIFactory2* f = nullptr;
        if (!create || FAILED(create(0, IID_PPV_ARGS(&f)))) {
            Log("couldn't create a DXGI factory: swapchain events unavailable");
            return;
        }
        Patch(f, {{kFactoryCreateSwapChain, P(&HkCreateSwapChain)}, {kFactoryCreateSwapChainForHwnd, P(&HkCreateForHwnd)}});
        f->Release();
        Log("DXGI swapchain creation hooked");
    });
}

// ---------------------------------------------------------------- the game's imports

using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
CreateDeviceFn g_realCreateDevice = nullptr;

// The module an address is in (nullptr: none).
HMODULE ModuleAt(void* p) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(p), &m);
    return m;
}

HRESULT WINAPI HkD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    void* const caller = _ReturnAddress();
    const HRESULT hr = g_realCreateDevice(adapter, fl, riid, out);
    {
        static std::atomic<int> n{0};
        if (n++ < 8) {
            char by[MAX_PATH] = "?";
            if (HMODULE m = ModuleAt(caller)) GetModuleFileNameA(m, by, MAX_PATH);
            Log("D3D12CreateDevice from %s (level %x, %s) -> 0x%08lx", strrchr(by, '\\') ? strrchr(by, '\\') + 1 : by, fl,
                out ? "device" : "support check", static_cast<unsigned long>(hr));
        }
    }
    if (FAILED(hr) || !out || !*out) return hr;
    // REFramework's own test device (its D3D12 hook setup) isn't the game's. The game may create its
    // device itself or through a helper library (AMD's AGS), so other callers count.
    if (HMODULE m = ModuleAt(caller); m && m == GetModuleHandleW(L"dinput8.dll")) return hr;
    ID3D12Device* d = nullptr;
    if (FAILED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&d)))) return hr;
    d->Release();
    if (GameDevice()) {
        if (GameDevice()->d3d() != d) Log("the game created a second D3D12 device (not followed)");
        return hr;
    }
    HookSystemFactory();
    HookDevice(d);
    CreateDeviceWrapper(d);
    OnDestroyed(d, &DeviceGone, nullptr);
    char by[MAX_PATH] = "?";
    if (HMODULE m = ModuleAt(caller)) GetModuleFileNameA(m, by, MAX_PATH);
    Log("game's D3D12 device %p (created by %s)", static_cast<void*>(d), strrchr(by, '\\') ? strrchr(by, '\\') + 1 : by);
    Invoke<addon_event::init_device>(Dev());
    return hr;
}

using GetProcFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcFn g_realGetProc = nullptr;

FARPROC WINAPI HkGetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC p = g_realGetProc(m, name);
    if (reinterpret_cast<uintptr_t>(name) > 0xFFFF && !strncmp(name, "D3D12", 5)) {
        static std::atomic<int> n{0};
        if (n++ < 12) {
            char mod[MAX_PATH] = "?";
            GetModuleFileNameA(m, mod, MAX_PATH);
            Log("game GetProcAddress(%s, %s) -> %p", strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod, name, reinterpret_cast<void*>(p));
        }
    }
    if (p && reinterpret_cast<uintptr_t>(name) > 0xFFFF && !strcmp(name, "D3D12CreateDevice")) {
        if (!g_realCreateDevice) g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(p);
        if (reinterpret_cast<CreateDeviceFn>(p) == g_realCreateDevice) return reinterpret_cast<FARPROC>(&HkD3D12CreateDevice);
    }
    return p;
}

using Factory1Fn = HRESULT(WINAPI*)(REFIID, void**);
using Factory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
Factory1Fn g_realFactory1 = nullptr;
Factory2Fn g_realFactory2 = nullptr;

HRESULT WINAPI HkFactory1(REFIID riid, void** out) {
    HookSystemFactory();
    return g_realFactory1(riid, out);
}

HRESULT WINAPI HkFactory2(UINT flags, REFIID riid, void** out) {
    HookSystemFactory();
    return g_realFactory2(flags, riid, out);
}

// Executable memory within 2 GB above <addr> (the module it's in), for jumps with 32-bit offsets.
uint8_t* AllocNear(void* addr) {
    HMODULE m = ModuleAt(addr);
    if (!m) return nullptr;
    auto* base = reinterpret_cast<uint8_t*>(m);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const uintptr_t start = (reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage + 0xFFFF) & ~uintptr_t(0xFFFF);
    for (uintptr_t a = start; a < reinterpret_cast<uintptr_t>(base) + 0x7FFF0000; a += 0x10000)
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
            return static_cast<uint8_t*>(p);
    return nullptr;
}

// Length of a whole x64 prologue instruction that can be copied elsewhere unchanged (no RIP-relative
// operand); 0 for anything else. Covers what D3D12 / DXGI exports start with.
size_t CopyableLength(const uint8_t* p) {
    if (p[0] >= 0x50 && p[0] <= 0x57) return 1;                                     // push r64
    if ((p[0] == 0x40 || p[0] == 0x41) && p[1] >= 0x50 && p[1] <= 0x57) return 2;   // push r64 (REX)
    if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && (p[2] & 0xC7) == 0x44 && p[3] == 0x24) return 5;  // mov [rsp+d8], r64
    if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) return 4;                     // sub rsp, imm8
    if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) return 7;                     // sub rsp, imm32
    if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4) return 3;                     // mov rax, rsp
    return 0;
}

// Puts a jump to <hook> at the start of <fn>, for callers that hold its address from before
// (Village resolved D3D12CreateDevice before plugins load). If <fn> starts with another hook's
// relative jump (ReShade's), that jump is pointed at <hook> instead and *orig is where it went;
// else its first instructions move to a trampoline (*orig) that jumps back.
bool InlineHook(void* fn, void* hook, void** orig) {
    auto* p = static_cast<uint8_t*>(fn);
    uint8_t* stub = AllocNear(fn);
    if (!stub) return false;
    // stub: jmp [rip+0] -> hook (the 5-byte jump at fn lands here); trampoline after it.
    const uint8_t abs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(stub, abs, 6);
    memcpy(stub + 6, &hook, 8);
    uint8_t* tramp = stub + 16;
    if (p[0] == 0xE9) {
        *orig = p + 5 + *reinterpret_cast<int32_t*>(p + 1);
    } else if (p[0] == 0xFF && p[1] == 0x25) {
        *orig = *reinterpret_cast<void**>(p + 6 + *reinterpret_cast<int32_t*>(p + 2));
    } else {
        size_t n = 0;
        while (n < 5) {
            const size_t len = CopyableLength(p + n);
            if (!len) {
                Log("inline hook: unknown instruction %02x %02x %02x at +%zu", p[n], p[n + 1], p[n + 2], n);
                return false;
            }
            n += len;
        }
        memcpy(tramp, p, n);
        memcpy(tramp + n, abs, 6);
        void* back = p + n;
        memcpy(tramp + n + 6, &back, 8);
        *orig = tramp;
    }
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    const int32_t rel = static_cast<int32_t>(stub - (p + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

// Points <name> in a module's export table at <hook> (through a jump stub within 2 GB above the
// module: export entries are 32-bit offsets from its base), so whoever looks it up from now on
// (GetProcAddress, from any module) gets the hook. *orig gets the real function.
bool PatchExport(HMODULE m, const char* name, void* hook, void** orig) {
    auto* base = reinterpret_cast<uint8_t*>(m);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return false;
    auto* exp = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto* names = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
    auto* ords = reinterpret_cast<WORD*>(base + exp->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<DWORD*>(base + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        if (strcmp(reinterpret_cast<const char*>(base + names[i]), name)) continue;
        DWORD* slot = &funcs[ords[i]];
        // A stub above the module, within reach of a 32-bit offset.
        uint8_t* stub = nullptr;
        const uintptr_t start = (reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage + 0xFFFF) & ~uintptr_t(0xFFFF);
        for (uintptr_t a = start; a < reinterpret_cast<uintptr_t>(base) + 0x7FFF0000 && !stub; a += 0x10000)
            stub = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(a), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (!stub) return false;
        const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
        memcpy(stub, jmp, 6);
        memcpy(stub + 6, &hook, sizeof(void*));
        FlushInstructionCache(GetCurrentProcess(), stub, 16);
        *orig = base + *slot;
        DWORD old;
        if (!VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &old)) return false;
        *slot = static_cast<DWORD>(stub - base);
        VirtualProtect(slot, sizeof(DWORD), old, &old);
        return true;
    }
    return false;
}

// Replaces every slot of the game's import table that holds <target>.
int PatchIat(void* target, void* hook) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int n = 0;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); dir.VirtualAddress && imp->Name; ++imp) {
        for (auto* t = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk); t->u1.Function; ++t) {
            if (reinterpret_cast<void*>(t->u1.Function) != target) continue;
            DWORD old;
            if (VirtualProtect(&t->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                t->u1.Function = reinterpret_cast<ULONGLONG>(hook);
                VirtualProtect(&t->u1.Function, sizeof(void*), old, &old);
                ++n;
            }
        }
    }
    return n;
}

}  // namespace

void SetSwapchainCreators(CreateSwapChainFn create, CreateSwapChainForHwndFn createForHwnd, CreateSwapChainFn* origCreate,
                          CreateSwapChainForHwndFn* origCreateForHwnd) {
    g_fgCreate = create;
    g_fgCreateForHwnd = createForHwnd;
    if (origCreate) *origCreate = &OrigCreateSwapChain;
    if (origCreateForHwnd) *origCreateForHwnd = &OrigCreateForHwnd;
}

void InstallEarly(HMODULE) {
    RdxFillSlots(g_slot);
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    g_exeBase = reinterpret_cast<uintptr_t>(base);
    g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;

    g_realGetProc = reinterpret_cast<GetProcFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress"));
    int n = PatchIat(reinterpret_cast<void*>(g_realGetProc), reinterpret_cast<void*>(&HkGetProcAddress));
    if (HMODULE kb = GetModuleHandleW(L"kernelbase.dll")) {
        void* p = reinterpret_cast<void*>(GetProcAddress(kb, "GetProcAddress"));
        if (p != reinterpret_cast<void*>(g_realGetProc)) n += PatchIat(p, reinterpret_cast<void*>(&HkGetProcAddress));
    }
    int f1 = 0, f2 = 0;
    if (HMODULE dxgi = GetModuleHandleW(L"dxgi.dll")) {
        g_realFactory1 = reinterpret_cast<Factory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        g_realFactory2 = reinterpret_cast<Factory2Fn>(GetProcAddress(dxgi, "CreateDXGIFactory2"));
        if (g_realFactory1) f1 = PatchIat(reinterpret_cast<void*>(g_realFactory1), reinterpret_cast<void*>(&HkFactory1));
        if (g_realFactory2) f2 = PatchIat(reinterpret_cast<void*>(g_realFactory2), reinterpret_cast<void*>(&HkFactory2));
    }
    // d3d12.dll loaded already (REFramework or ReShade loaded it): D3D12CreateDevice itself is hooked
    // (in its code), for games that hold its address from before plugins load (Village). If that
    // can't be done, its export is pointed at the hook (later lookups from any module).
    const char* how = "through the game's GetProcAddress (d3d12.dll not loaded yet)";
    if (HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll")) {
        void* fn = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"));
        const uint8_t first = fn ? *static_cast<const uint8_t*>(fn) : 0;
        void* real = nullptr;
        if (fn && InlineHook(fn, reinterpret_cast<void*>(&HkD3D12CreateDevice), &real)) {
            g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(real);
            how = first == 0xE9 ? "in its code (chained to another hook, ReShade's)" : "in its code";
        } else if (PatchExport(d3d12, "D3D12CreateDevice", reinterpret_cast<void*>(&HkD3D12CreateDevice), &real)) {
            g_realCreateDevice = reinterpret_cast<CreateDeviceFn>(real);
            how = "in d3d12.dll's export table (its code couldn't be hooked)";
        } else {
            how = "NOT hooked in d3d12.dll (only the game's GetProcAddress)";
        }
    }
    Log("hooks installed (GetProcAddress imports %d, CreateDXGIFactory1/2 %d/%d); D3D12CreateDevice %s", n, f1, f2, how);
}

void Uninstall() {}

}  // namespace rdx

// For main.cpp (which doesn't include rdx.hpp).
reshade::api::device* rdx_GameDevice() { return rdx::GameDevice(); }
void rdx_InstallEarly(HMODULE self) { rdx::InstallEarly(self); }
