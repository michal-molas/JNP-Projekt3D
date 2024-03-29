#pragma once

#include <utility>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <wincodec.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        exit(hr);
    }
}

struct Vertex
{
    FLOAT position[3];
    FLOAT normal[3];
    FLOAT color[4];
    FLOAT tex_coord[2];
    UINT is_no_light;
};

struct vs_const_buffer_t {
    XMFLOAT4X4 matWorldViewProj;
    XMFLOAT4X4 matWorldView;
    XMFLOAT4X4 matView;
    XMFLOAT4 colLight;
    XMFLOAT4 dirLight;

    XMFLOAT4 padding[(256 - 3 * sizeof(XMFLOAT4X4) - 2 * sizeof(XMFLOAT4)) / sizeof(XMFLOAT4)];
};

class D3DApp
{
public:
    D3DApp(UINT width, UINT height, CONST TCHAR* name);

    void init();
    void update();
    void render();
    void resize();
    void destroy();

    UINT GetWidth() const { return width; }
    UINT GetHeight() const { return height; }
    const TCHAR* GetTitle() const { return title; }

private:
    UINT width;
    UINT height;
    FLOAT aspectRatio;
    CONST TCHAR* title;

    static const UINT FrameCount = 2;

    IWICImagingFactory* wic_factory = nullptr;

    // Pipeline objects.
    D3D12_VIEWPORT viewport;
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> constBufferHeap;
    ComPtr<ID3D12DescriptorHeap> depthBufferHeap;
    ComPtr<ID3D12Resource> renderTargets[FrameCount];
    D3D12_RECT scissorRect;

    UINT rtvDescriptorSize;

    // App resources.
    ComPtr<ID3D12Resource> vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;

    ComPtr<ID3D12Resource> houseVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW houseVertexBufferView;

    ComPtr<ID3D12Resource> groundVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW groundVertexBufferView;

    ComPtr<ID3D12Resource> rockVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW rockVertexBufferView;

    ComPtr<ID3D12Resource> constBuffer;
    UINT8* constBufferData;

    ComPtr<ID3D12Resource> depthBuffer;

    ComPtr<ID3D12Resource> texture_resource;

    // Synchronization objects.
    UINT frameIndex;
    HANDLE fenceEvent;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue;

    UINT const bmp_px_size = 4;
    UINT bmp_width = 0, bmp_height = 0;
    BYTE* bmp_bits = nullptr;

    XMMATRIX tempMatrix;

    void loadPipeline();
    void loadAssets();
    void populateCommandList();
    void waitForPreviousFrame();

    void createCommandQueue();
    void createSwapChain(ComPtr<IDXGIFactory7> factory);
    void createHeaps();
    void createRenderTargets();
    void createCommandAllocator();

    void createRootSignature();
    void createPipelineState();
    void createCommandList();
    void createBuffers();
    void createVertexBuffer(ComPtr<ID3D12Resource>& buffer, std::pair<Vertex*, size_t> verticies,
        D3D12_VERTEX_BUFFER_VIEW& bufferView);
    void createConstBuffer();
    void createDepthBuffer();
    void createTexture();
    void createFence();
    std::pair<Vertex*, size_t> getVertices();
    std::pair<Vertex*, size_t> getHouseVertices();
    std::pair<Vertex*, size_t> getGroundVertices();
    std::pair<Vertex*, size_t> getRockVertices();

    HRESULT LoadBitmapFromFile(
        PCWSTR uri, UINT& width, UINT& height, BYTE** ppBits
    );
};