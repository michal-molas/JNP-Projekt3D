#pragma once

#include "stdafx.h"
#include <stdexcept>

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().
using Microsoft::WRL::ComPtr;
using namespace DirectX;



inline void GetAssetsPath(_Out_writes_(pathSize) WCHAR* path, UINT pathSize)
{
    if (path == nullptr)
    {
        throw std::exception();
    }

    DWORD size = GetModuleFileName(nullptr, path, pathSize);
    if (size == 0 || size == pathSize)
    {
        // Method failed or path was truncated.
        throw std::exception();
    }

    WCHAR* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash)
    {
        *(lastSlash + 1) = L'\0';
    }
}

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
    struct Vertex
    {
        FLOAT position[3];
        FLOAT color[4];
    };

    std::wstring title;
    //std::wstring assetsPath;
    //FLOAT aspectRatio;
    Vertex triangleVertices[3] =
    {
        { 0.0f, 1.0f, 0.5f,         0.0f, 1.0f, 0.0f, 1.0f },
        { 1.0f, 0.0f, 0.5f,         1.0f, 0.0f, 0.0f, 1.0f },
        { -1.0f, -1.0f, 0.5f,       0.0f, 0.0f, 1.0f, 1.0f }
    };

    const UINT vertexBufferSize = sizeof(triangleVertices);
    size_t const numVertices = vertexBufferSize / sizeof(Vertex);

    UINT width;
    UINT height;
    static const UINT FrameCount = 2;

    // Pipeline objects.
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissorRect;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> renderTargets[FrameCount];
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12RootSignature> rootSignature;
    UINT rtvDescriptorSize;
    UINT currentBackBufferIndex;

    // App resources.
    ComPtr<ID3D12Resource> vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;

    // Synchronization objects.
    UINT frameIndex;
    HANDLE fenceEvent;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue;

    void LoadPipeline();
    void LoadAssets();
    void PopulateCommandList();
    void WaitForPreviousFrame();
    std::wstring GetAssetFullPath(LPCWSTR assetName);
};


