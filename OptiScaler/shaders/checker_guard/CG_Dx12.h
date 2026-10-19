#pragma once

#include "SysUtils.h"
#include "CG_Common.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

#define CG_NUM_OF_HEAPS 4

class CG_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[CG_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[CG_NUM_OF_HEAPS] {};

  public:
    // Colour and motion readable by a compute shader, the output in UNORDERED_ACCESS.
    // depth may be null (the surface test is then skipped).
    bool Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* motion,
                  ID3D12Resource* depth, ID3D12Resource* output, const CGConstants& constants);

    CG_Dx12(std::string name, ID3D12Device* device);
    ~CG_Dx12();
};
