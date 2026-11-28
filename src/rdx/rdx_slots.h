/* rdx: the vtable slots rdx hooks, filled from the Windows SDK's C declarations (rdx_slots.c),
   so no slot number is counted by hand. C and C++. */
#pragma once

enum RdxSlot {
    /* ID3D12Device and later */
    kDevCreateCommandQueue,
    kDevCreateCommandList,
    kDevCreateGraphicsPipelineState,
    kDevCreateComputePipelineState,
    kDevCreateDescriptorHeap,
    kDevCreateRootSignature,
    kDevCreateConstantBufferView,
    kDevCreateShaderResourceView,
    kDevCreateUnorderedAccessView,
    kDevCreateRenderTargetView,
    kDevCreateDepthStencilView,
    kDevCreateSampler,
    kDevCopyDescriptors,
    kDevCopyDescriptorsSimple,
    kDevCreateCommittedResource,
    kDevCreatePlacedResource,
    kDevCreateReservedResource,
    kDevCreateCommandSignature,
    kDevCreatePipelineLibrary,      /* 1 */
    kDevCreatePipelineState,        /* 2 */
    kDevCreateCommandList1,         /* 4 */
    kDevCreateCommittedResource1,   /* 4 */
    kDevCreateReservedResource1,    /* 4 */
    kDevCreateCommittedResource2,   /* 8 */
    kDevCreatePlacedResource1,      /* 8 */
    kDevCreateCommittedResource3,   /* 10 */
    kDevCreatePlacedResource2,      /* 10 */
    kDevCreateReservedResource2,    /* 10 */
    /* ID3D12PipelineLibrary(1) */
    kLibLoadGraphicsPipeline,
    kLibLoadComputePipeline,
    kLibLoadPipeline,
    /* ID3D12GraphicsCommandList */
    kListReset,
    kListDrawInstanced,
    kListDrawIndexedInstanced,
    kListDispatch,
    kListCopyBufferRegion,
    kListCopyTextureRegion,
    kListCopyResource,
    kListResolveSubresource,
    kListRSSetViewports,
    kListRSSetScissorRects,
    kListSetPipelineState,
    kListResourceBarrier,
    kListSetDescriptorHeaps,
    kListSetComputeRootSignature,
    kListSetGraphicsRootSignature,
    kListSetComputeRootDescriptorTable,
    kListSetGraphicsRootDescriptorTable,
    kListSetComputeRoot32BitConstant,
    kListSetGraphicsRoot32BitConstant,
    kListSetComputeRoot32BitConstants,
    kListSetGraphicsRoot32BitConstants,
    kListSetComputeRootConstantBufferView,
    kListSetGraphicsRootConstantBufferView,
    kListSetComputeRootShaderResourceView,
    kListSetGraphicsRootShaderResourceView,
    kListSetComputeRootUnorderedAccessView,
    kListSetGraphicsRootUnorderedAccessView,
    kListOMSetRenderTargets,
    kListExecuteIndirect,
    /* ID3D12CommandQueue */
    kQueueExecuteCommandLists,
    /* IDXGIFactory2 */
    kFactoryCreateSwapChain,
    kFactoryCreateSwapChainForHwnd,
    /* IDXGISwapChain3 */
    kScPresent,
    kScResizeBuffers,
    kScPresent1,
    kScResizeBuffers1,
    kScSetColorSpace1,
    kSlotCount
};

#ifdef __cplusplus
extern "C" {
#endif
void RdxFillSlots(int* slots); /* kSlotCount entries */
#ifdef __cplusplus
}
#endif
