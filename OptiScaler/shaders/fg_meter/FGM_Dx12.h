#pragma once

#include "SysUtils.h"
#include "FGM_Common.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

#define FGM_NUM_OF_HEAPS 8

class FGM_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[FGM_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[FGM_NUM_OF_HEAPS] {};

  public:
    // The source readable by a compute shader, the thumbnail in UNORDERED_ACCESS.
    bool Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source, ID3D12Resource* thumbnail,
                  const FGMConstants& constants);

    FGM_Dx12(std::string name, ID3D12Device* device);
    ~FGM_Dx12();
};
