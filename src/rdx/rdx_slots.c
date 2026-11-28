/* rdx: vtable slot numbers from the Windows SDK's C interface declarations. */
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "rdx_slots.h"

#define SLOT(vtbl, m) (int)(offsetof(vtbl, m) / sizeof(void*))

void RdxFillSlots(int* s) {
    s[kDevCreateCommandQueue] = SLOT(ID3D12DeviceVtbl, CreateCommandQueue);
    s[kDevCreateCommandList] = SLOT(ID3D12DeviceVtbl, CreateCommandList);
    s[kDevCreateGraphicsPipelineState] = SLOT(ID3D12DeviceVtbl, CreateGraphicsPipelineState);
    s[kDevCreateComputePipelineState] = SLOT(ID3D12DeviceVtbl, CreateComputePipelineState);
    s[kDevCreateDescriptorHeap] = SLOT(ID3D12DeviceVtbl, CreateDescriptorHeap);
    s[kDevCreateRootSignature] = SLOT(ID3D12DeviceVtbl, CreateRootSignature);
    s[kDevCreateConstantBufferView] = SLOT(ID3D12DeviceVtbl, CreateConstantBufferView);
    s[kDevCreateShaderResourceView] = SLOT(ID3D12DeviceVtbl, CreateShaderResourceView);
    s[kDevCreateUnorderedAccessView] = SLOT(ID3D12DeviceVtbl, CreateUnorderedAccessView);
    s[kDevCreateRenderTargetView] = SLOT(ID3D12DeviceVtbl, CreateRenderTargetView);
    s[kDevCreateDepthStencilView] = SLOT(ID3D12DeviceVtbl, CreateDepthStencilView);
    s[kDevCreateSampler] = SLOT(ID3D12DeviceVtbl, CreateSampler);
    s[kDevCopyDescriptors] = SLOT(ID3D12DeviceVtbl, CopyDescriptors);
    s[kDevCopyDescriptorsSimple] = SLOT(ID3D12DeviceVtbl, CopyDescriptorsSimple);
    s[kDevCreateCommittedResource] = SLOT(ID3D12DeviceVtbl, CreateCommittedResource);
    s[kDevCreatePlacedResource] = SLOT(ID3D12DeviceVtbl, CreatePlacedResource);
    s[kDevCreateReservedResource] = SLOT(ID3D12DeviceVtbl, CreateReservedResource);
    s[kDevCreateCommandSignature] = SLOT(ID3D12DeviceVtbl, CreateCommandSignature);
    s[kDevCreatePipelineLibrary] = SLOT(ID3D12Device1Vtbl, CreatePipelineLibrary);
    s[kDevCreatePipelineState] = SLOT(ID3D12Device2Vtbl, CreatePipelineState);
    s[kDevCreateCommandList1] = SLOT(ID3D12Device4Vtbl, CreateCommandList1);
    s[kDevCreateCommittedResource1] = SLOT(ID3D12Device4Vtbl, CreateCommittedResource1);
    s[kDevCreateReservedResource1] = SLOT(ID3D12Device4Vtbl, CreateReservedResource1);
    s[kDevCreateCommittedResource2] = SLOT(ID3D12Device8Vtbl, CreateCommittedResource2);
    s[kDevCreatePlacedResource1] = SLOT(ID3D12Device8Vtbl, CreatePlacedResource1);
    s[kDevCreateCommittedResource3] = SLOT(ID3D12Device10Vtbl, CreateCommittedResource3);
    s[kDevCreatePlacedResource2] = SLOT(ID3D12Device10Vtbl, CreatePlacedResource2);
    s[kDevCreateReservedResource2] = SLOT(ID3D12Device10Vtbl, CreateReservedResource2);

    s[kLibLoadGraphicsPipeline] = SLOT(ID3D12PipelineLibraryVtbl, LoadGraphicsPipeline);
    s[kLibLoadComputePipeline] = SLOT(ID3D12PipelineLibraryVtbl, LoadComputePipeline);
    s[kLibLoadPipeline] = SLOT(ID3D12PipelineLibrary1Vtbl, LoadPipeline);

    s[kListReset] = SLOT(ID3D12GraphicsCommandListVtbl, Reset);
    s[kListDrawInstanced] = SLOT(ID3D12GraphicsCommandListVtbl, DrawInstanced);
    s[kListDrawIndexedInstanced] = SLOT(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced);
    s[kListDispatch] = SLOT(ID3D12GraphicsCommandListVtbl, Dispatch);
    s[kListCopyBufferRegion] = SLOT(ID3D12GraphicsCommandListVtbl, CopyBufferRegion);
    s[kListCopyTextureRegion] = SLOT(ID3D12GraphicsCommandListVtbl, CopyTextureRegion);
    s[kListCopyResource] = SLOT(ID3D12GraphicsCommandListVtbl, CopyResource);
    s[kListResolveSubresource] = SLOT(ID3D12GraphicsCommandListVtbl, ResolveSubresource);
    s[kListRSSetViewports] = SLOT(ID3D12GraphicsCommandListVtbl, RSSetViewports);
    s[kListRSSetScissorRects] = SLOT(ID3D12GraphicsCommandListVtbl, RSSetScissorRects);
    s[kListSetPipelineState] = SLOT(ID3D12GraphicsCommandListVtbl, SetPipelineState);
    s[kListResourceBarrier] = SLOT(ID3D12GraphicsCommandListVtbl, ResourceBarrier);
    s[kListSetDescriptorHeaps] = SLOT(ID3D12GraphicsCommandListVtbl, SetDescriptorHeaps);
    s[kListSetComputeRootSignature] = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootSignature);
    s[kListSetGraphicsRootSignature] = SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootSignature);
    s[kListSetComputeRootDescriptorTable] = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootDescriptorTable);
    s[kListSetGraphicsRootDescriptorTable] = SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootDescriptorTable);
    s[kListSetComputeRoot32BitConstant] = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstant);
    s[kListSetGraphicsRoot32BitConstant] = SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRoot32BitConstant);
    s[kListSetComputeRoot32BitConstants] = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRoot32BitConstants);
    s[kListSetGraphicsRoot32BitConstants] = SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRoot32BitConstants);
    s[kListSetComputeRootConstantBufferView] = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootConstantBufferView);
    s[kListSetGraphicsRootConstantBufferView] = SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootConstantBufferView);
    s[kListSetComputeRootShaderResourceView] = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootShaderResourceView);
    s[kListSetGraphicsRootShaderResourceView] = SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootShaderResourceView);
    s[kListSetComputeRootUnorderedAccessView] = SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootUnorderedAccessView);
    s[kListSetGraphicsRootUnorderedAccessView] = SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootUnorderedAccessView);
    s[kListOMSetRenderTargets] = SLOT(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets);
    s[kListExecuteIndirect] = SLOT(ID3D12GraphicsCommandListVtbl, ExecuteIndirect);

    s[kQueueExecuteCommandLists] = SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists);

    s[kFactoryCreateSwapChain] = SLOT(IDXGIFactory2Vtbl, CreateSwapChain);
    s[kFactoryCreateSwapChainForHwnd] = SLOT(IDXGIFactory2Vtbl, CreateSwapChainForHwnd);

    s[kScPresent] = SLOT(IDXGISwapChain3Vtbl, Present);
    s[kScResizeBuffers] = SLOT(IDXGISwapChain3Vtbl, ResizeBuffers);
    s[kScPresent1] = SLOT(IDXGISwapChain3Vtbl, Present1);
    s[kScResizeBuffers1] = SLOT(IDXGISwapChain3Vtbl, ResizeBuffers1);
    s[kScSetColorSpace1] = SLOT(IDXGISwapChain3Vtbl, SetColorSpace1);
}
