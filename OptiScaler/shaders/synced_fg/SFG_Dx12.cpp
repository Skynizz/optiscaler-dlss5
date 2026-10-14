#include "pch.h"
#include "SFG_Dx12.h"

#include <State.h>

#include "precompiled/SFG_Shader.h"

bool SFG_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* prevColor, ID3D12Resource* curColor,
                        ID3D12Resource* velocity, ID3D12Resource* depth, ID3D12Resource* output,
                        const SFGConstants& constants, ID3D12Resource* hudlessPrev, ID3D12Resource* hudlessCur)
{
    if (!_init || _device == nullptr || cmdList == nullptr || prevColor == nullptr || curColor == nullptr ||
        velocity == nullptr || depth == nullptr || output == nullptr)
        return false;

    _counter = (_counter + 1) % SFG_NUM_OF_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[_counter];

    CreateShaderResourceView(_device, prevColor, heap.GetSrvCPU(0));
    CreateShaderResourceView(_device, curColor, heap.GetSrvCPU(1));
    CreateShaderResourceView(_device, velocity, heap.GetSrvCPU(2));
    CreateShaderResourceView(_device, depth, heap.GetSrvCPU(3));
    // Every slot gets a view: without HUD-less frames the frames themselves stand in (and are not read).
    CreateShaderResourceView(_device, hudlessPrev != nullptr ? hudlessPrev : prevColor, heap.GetSrvCPU(4));
    CreateShaderResourceView(_device, hudlessCur != nullptr ? hudlessCur : curColor, heap.GetSrvCPU(5));
    CreateUnorderedAccessView(_device, output, heap.GetUavCPU(0), 0);

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

    cmdList->Dispatch((constants.outWidth + _numThreadsX - 1) / _numThreadsX,
                      (constants.outHeight + _numThreadsY - 1) / _numThreadsY, 1);

    return true;
}

SFG_Dx12::SFG_Dx12(std::string name, ID3D12Device* device) : Shader_Dx12(name, device)
{
    if (device == nullptr)
    {
        LOG_ERROR("device is nullptr!");
        return;
    }

    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(device, 6, 1, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(SFGConstants));
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

    if (!CreateComputePipeline(device, &_pipelineState, SFG_cso, sizeof(SFG_cso), sfgCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(device, _frameHeaps, SFG_NUM_OF_HEAPS);
}

SFG_Dx12::~SFG_Dx12()
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
