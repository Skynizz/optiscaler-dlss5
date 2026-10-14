#pragma once

#include "SysUtils.h"
#include "SFG_Common.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

// Several generated frames per real frame, each with its own t, and frames in flight: one heap and one
// constant buffer per slot, so a dispatch never reads constants written for another.
#define SFG_NUM_OF_HEAPS 16

class SFG_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[SFG_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[SFG_NUM_OF_HEAPS] {};

    uint32_t _numThreadsX = 8;
    uint32_t _numThreadsY = 8;

  public:
    // Every input in NON_PIXEL_SHADER_RESOURCE, the output in UNORDERED_ACCESS.
    bool Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* prevColor, ID3D12Resource* curColor,
                  ID3D12Resource* velocity, ID3D12Resource* depth, ID3D12Resource* output,
                  const SFGConstants& constants);

    SFG_Dx12(std::string name, ID3D12Device* device);
    ~SFG_Dx12();
};
