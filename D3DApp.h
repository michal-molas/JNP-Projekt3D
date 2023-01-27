#pragma once

#include "stdafx.h"
#include <stdexcept>

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().
using Microsoft::WRL::ComPtr;

//inline std::string HrToString(HRESULT hr)
//{
//    char s_str[64] = {};
//    sprintf_s(s_str, "HRESULT of 0x%08X", static_cast<UINT>(hr));
//    return std::string(s_str);
//}
//
//class HrException : public std::runtime_error
//{
//public:
//    HrException(HRESULT hr) : std::runtime_error(HrToString(hr)), hr(hr) {}
//    HRESULT Error() const { return hr; }
//private:
//    const HRESULT hr;
//};

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        // ssss
        exit(hr);
    }
}

class D3DApp
{
public:
    D3DApp(UINT width, UINT height, std::wstring name);

    void OnInit();
    void OnUpdate();
    void OnRender();
    void OnDestroy();

    UINT GetWidth() const { return width; }
    UINT GetHeight() const { return height; }
    const WCHAR* GetTitle() const { return title.c_str(); }

private:
    std::wstring title;
    UINT width;
    UINT height;
    static const UINT FrameCount = 2;

    // Pipeline objects.
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> renderTargets[FrameCount];
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    UINT rtvDescriptorSize;
    UINT currentBackBufferIndex;

    // Synchronization objects.
    UINT frameIndex;
    HANDLE fenceEvent;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue;

    void LoadPipeline();
    void LoadAssets();
    void PopulateCommandList();
    void WaitForPreviousFrame();
};


