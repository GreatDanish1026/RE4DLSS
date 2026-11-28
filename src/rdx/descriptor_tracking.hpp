// RE4DLSS 2.0: ReShade's descriptor_tracking utility (examples/utils), same public interface, served
// by the rdx hook layer's own descriptor tracking (rdx_track.cpp): every descriptor the game writes
// (views created into a heap, CopyDescriptors / CopyDescriptorsSimple) is followed there already.
#pragma once

#include "reshade_api.hpp"

class __declspec(uuid("f02b21ba-f5dd-44af-9e90-c54535a98ce3")) descriptor_tracking {
public:
    static void register_events() {}
    static void unregister_events() {}

    // The constant buffer (buffer and range) in a descriptor heap slot; {0} if it holds something else.
    reshade::api::buffer_range get_buffer_range(reshade::api::descriptor_heap heap, uint32_t offset) const;
    reshade::api::sampler get_sampler(reshade::api::descriptor_heap heap, uint32_t offset) const;
    // The view in a slot: the handle it was created at (as ReShade reports it), so it can be
    // copied from and looked up like any view the game created.
    reshade::api::resource_view get_resource_view(reshade::api::descriptor_heap heap, uint32_t offset) const;
    // A root signature parameter as ReShade describes it (descriptor tables as plain tables).
    reshade::api::pipeline_layout_param get_pipeline_layout_param(reshade::api::pipeline_layout layout, uint32_t param) const;
};
