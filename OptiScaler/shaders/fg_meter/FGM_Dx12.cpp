#include "pch.h"
#include "FGM_Dx12.h"

#include <State.h>

#include "precompiled/FGM_Shader.h"

bool FGM_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source, ID3D12Resource* thumbnail,
                        const FGMConstants& constants)
{
    if (!_init || _device == nullptr || cmdList == nullptr || source == nullptr || thumbnail == nullptr)
        return false;

    _counter = (_counter + 1) % FGM_NUM_OF_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[_counter];

    CreateShaderResourceView(_device, source, heap.GetSrvCPU(0));
    CreateUnorderedAccessView(_device, thumbnail, heap.GetUavCPU(0), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[_counter], constants, heap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineState);
    cmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    cmdList->Dispatch((constants.thumbWidth + 7) / 8, (constants.thumbHeight + 3) / 4, 1);

    return true;
}

FGM_Dx12::FGM_Dx12(std::string name, ID3D12Device* device) : Shader_Dx12(name, device)
{
    if (device == nullptr)
    {
        LOG_ERROR("device is nullptr!");
        return;
    }

    if (!SetupRootSignature(device, 1, 1, 1))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(FGMConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (auto& buffer : _constantBuffers)
    {
        if (device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&buffer)) != S_OK)
        {
            LOG_ERROR("[{0}] Failed to create a constant buffer", _name);
            return;
        }
    }

    if (!CreateComputePipeline(device, &_pipelineState, FGM_cso, sizeof(FGM_cso), fgmCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(device, _frameHeaps, FGM_NUM_OF_HEAPS);
}

FGM_Dx12::~FGM_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    for (auto& buffer : _constantBuffers)
    {
        if (buffer != nullptr)
        {
            buffer->Release();
            buffer = nullptr;
        }
    }

    if (!_init)
        return;

    for (auto& heap : _frameHeaps)
        heap.ReleaseHeaps();
}
