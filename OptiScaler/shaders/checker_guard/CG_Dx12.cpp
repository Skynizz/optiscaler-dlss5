#include "pch.h"
#include "CG_Dx12.h"

#include <State.h>

#include "precompiled/CG_Shader.h"

bool CG_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* motion,
                       ID3D12Resource* depth, ID3D12Resource* output, const CGConstants& constants)
{
    if (!_init || _device == nullptr || cmdList == nullptr || colour == nullptr || motion == nullptr ||
        output == nullptr)
        return false;

    _counter = (_counter + 1) % CG_NUM_OF_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[_counter];

    CreateShaderResourceView(_device, colour, heap.GetSrvCPU(0));
    CreateShaderResourceView(_device, motion, heap.GetSrvCPU(1));

    // Depth in a readable view: the generic typeless translation turns R32_FLOAT_X8X24 into a depth-stencil format,
    // invalid for a shader view (the device was removed on it once, in The Witcher 3). Without depth, the colour
    // stands in and the shader is told there is none.
    if (depth != nullptr)
    {
        DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;

        switch (depth->GetDesc().Format)
        {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            break;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            break;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
            view = DXGI_FORMAT_R32_FLOAT;
            break;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:
            view = DXGI_FORMAT_R16_UNORM;
            break;
        default:
            break;
        }

        if (view != DXGI_FORMAT_UNKNOWN)
            CreateShaderResourceView(_device, depth, heap.GetSrvCPU(2), view, false);
        else
            CreateShaderResourceView(_device, depth, heap.GetSrvCPU(2));
    }
    else
    {
        CreateShaderResourceView(_device, colour, heap.GetSrvCPU(2));
    }
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
    cmdList->Dispatch((constants.width + 7) / 8, (constants.height + 7) / 8, 1);

    return true;
}

CG_Dx12::CG_Dx12(std::string name, ID3D12Device* device) : Shader_Dx12(name, device)
{
    if (device == nullptr)
    {
        LOG_ERROR("device is nullptr!");
        return;
    }

    if (!SetupRootSignature(device, 3, 1, 1))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(CGConstants));
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

    if (!CreateComputePipeline(device, &_pipelineState, CG_cso, sizeof(CG_cso), cgCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(device, _frameHeaps, CG_NUM_OF_HEAPS);
}

CG_Dx12::~CG_Dx12()
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
