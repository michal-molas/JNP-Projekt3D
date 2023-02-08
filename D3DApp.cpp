#include "d3dx12.h"
#include <cmath>

//#include "exceptions.h"
#include "WinApp.h"
#include "D3DApp.h"

#include "vertex_shader.h"
#include "pixel_shader.h"

#include <wincodec.h>

namespace {
    float pi() { return static_cast<float>(std::atan(1.f)) * 4.f; }
}

D3DApp::D3DApp(UINT width, UINT height, CONST TCHAR* name) :
    width(width),
    height(height),
    title(name),
    angle(0),
    scissorRect(CD3DX12_RECT(0, 0, LONG_MAX, LONG_MAX)),
    viewport(CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)))
{
}

void D3DApp::init()
{
    tempMatrix = XMMatrixIdentity();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(IWICImagingFactory),
        reinterpret_cast<LPVOID*>(&wic_factory)
    );

    LoadBitmapFromFile(
        TEXT("textures.jpg"), bmp_width, bmp_height, &bmp_bits
    );

    loadPipeline();
    loadAssets();
}

void D3DApp::loadPipeline()
{
    UINT dxgiFactoryFlags = 0;

    ComPtr<IDXGIFactory7> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    ThrowIfFailed(D3D12CreateDevice(
        nullptr,
        D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&device)
    ));

    createCommandQueue();
    createSwapChain(factory);
    createHeaps();
    createRenderTargets();
    createCommandAllocator();
}

void D3DApp::loadAssets()
{
    createRootSignature();
    createPipelineState();
    createCommandList();
    createBuffers();
    createFence();

    // Budowa w³aœciwego zasobu tekstury
    D3D12_HEAP_PROPERTIES tex_heap_prop = {
    .Type = D3D12_HEAP_TYPE_DEFAULT,
    .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
    .CreationNodeMask = 1,
    .VisibleNodeMask = 1
    };
    D3D12_RESOURCE_DESC tex_resource_desc = {
    .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Alignment = 0,
    .Width = bmp_width,
    .Height = bmp_height,
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
    .SampleDesc = {.Count = 1, .Quality = 0 },
    .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
    .Flags = D3D12_RESOURCE_FLAG_NONE
    };

    device->CreateCommittedResource(
        &tex_heap_prop, D3D12_HEAP_FLAG_NONE,
        &tex_resource_desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&texture_resource)
    );

    // Budowa pomocniczego bufora wczytania tekstury do GPU
    ComPtr<ID3D12Resource> texture_upload_buffer = nullptr;
    // - ustalenie rozmiaru tego pom. bufora
    UINT64 RequiredSize = 0;
    auto Desc = texture_resource.Get()->GetDesc();
    ID3D12Device* pDevice = nullptr;
    texture_resource.Get()->GetDevice(
        __uuidof(*pDevice), reinterpret_cast<void**>(&pDevice)
    );
    pDevice->GetCopyableFootprints(
        &Desc, 0, 1, 0, nullptr, nullptr, nullptr, &RequiredSize
    );
    pDevice->Release();
    
    D3D12_HEAP_PROPERTIES tex_upload_heap_prop = {
        .Type = D3D12_HEAP_TYPE_UPLOAD,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1
    };
    D3D12_RESOURCE_DESC tex_upload_resource_desc = {
    .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment = 0,
    .Width = RequiredSize,
    .Height = 1,
    .DepthOrArraySize = 1,
    .MipLevels = 1,
    .Format = DXGI_FORMAT_UNKNOWN,
    .SampleDesc = {.Count = 1, .Quality = 0 },
    .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags = D3D12_RESOURCE_FLAG_NONE
    };

    device->CreateCommittedResource(
        &tex_upload_heap_prop, D3D12_HEAP_FLAG_NONE,
        &tex_upload_resource_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&texture_upload_buffer)
    );

    // - skopiowanie danych tekstury do pom. bufora
    D3D12_SUBRESOURCE_DATA texture_data = {
        .pData = bmp_bits,
        .RowPitch = static_cast<LONG_PTR>(bmp_width) * static_cast<LONG_PTR>(bmp_px_size),
        .SlicePitch = static_cast<LONG_PTR>(bmp_width) * static_cast<LONG_PTR>(bmp_height) * static_cast<LONG_PTR>(bmp_px_size)
    };

    ThrowIfFailed(commandAllocator->Reset());
    ThrowIfFailed(commandList->Reset(commandAllocator.Get(), pipelineState.Get()));

    // ... ID3D12GraphicsCommandList::Reset() - to dlatego lista
// poleceñ i obiekt stanu potoku musz¹ byæ wczeœniej utworzone
    UINT const MAX_SUBRESOURCES = 1;
    RequiredSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layouts[MAX_SUBRESOURCES];
    UINT NumRows[MAX_SUBRESOURCES];
    UINT64 RowSizesInBytes[MAX_SUBRESOURCES];
    Desc = texture_resource.Get()->GetDesc();
    pDevice = nullptr;
    texture_resource.Get()->GetDevice(
        __uuidof(*pDevice), reinterpret_cast<void**>(&pDevice)
    );
    pDevice->GetCopyableFootprints(
        &Desc, 0, 1, 0, Layouts, NumRows,
        RowSizesInBytes, &RequiredSize
    );
    pDevice->Release();
    BYTE* map_tex_data = nullptr;
    texture_upload_buffer->Map(
        0, nullptr, reinterpret_cast<void**>(&map_tex_data)
    );
    D3D12_MEMCPY_DEST DestData = {
    .pData = map_tex_data + Layouts[0].Offset,
    .RowPitch = Layouts[0].Footprint.RowPitch,
    .SlicePitch =
    SIZE_T(Layouts[0].Footprint.RowPitch) * SIZE_T(NumRows[0])
    };
    for (UINT z = 0; z < Layouts[0].Footprint.Depth; ++z) {
        auto pDestSlice =
            static_cast<UINT8*>(DestData.pData)
            + DestData.SlicePitch * z;
        auto pSrcSlice =
            static_cast<const UINT8*>(texture_data.pData)
            + texture_data.SlicePitch * LONG_PTR(z);
        for (UINT y = 0; y < NumRows[0]; ++y) {
            memcpy(
                pDestSlice + DestData.RowPitch * y,
                pSrcSlice + texture_data.RowPitch * LONG_PTR(y),
                static_cast<SIZE_T>(RowSizesInBytes[0])
            );
        }
    }
    texture_upload_buffer->Unmap(0, nullptr);

    // - zlecenie procesorowi GPU jego skopiowania do w³aœciwego
    // zasobu tekstury
    D3D12_TEXTURE_COPY_LOCATION Dst = {
    .pResource = texture_resource.Get(),
    .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    .SubresourceIndex = 0
    };
    D3D12_TEXTURE_COPY_LOCATION Src = {
    .pResource = texture_upload_buffer.Get(),
    .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
    .PlacedFootprint = Layouts[0]
    };
    commandList->CopyTextureRegion(
        &Dst, 0, 0, 0, &Src, nullptr
    );
    D3D12_RESOURCE_BARRIER tex_upload_resource_barrier = {
    .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
    .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
    .Transition = {
    .pResource = texture_resource.Get(),
    .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
    .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
    .StateAfter =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
    };
    commandList->ResourceBarrier(
        1, &tex_upload_resource_barrier
    );
    commandList->Close();
    ID3D12CommandList* cmd_list = commandList.Get();
    commandQueue->ExecuteCommandLists(1, &cmd_list);

    // - tworzy SRV (widok zasobu shadera) dla tekstury
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
    .Format = tex_resource_desc.Format,
    .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
    .Shader4ComponentMapping =
    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
    .Texture2D = {
    .MostDetailedMip = 0,
    .MipLevels = 1,
    .PlaneSlice = 0,
    .ResourceMinLODClamp = 0.0f
    },
    };
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_desc_handle =
        constBufferHeap->GetCPUDescriptorHandleForHeapStart();
    
    cpu_desc_handle.ptr +=
        device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
        );
    
    device->CreateShaderResourceView(
        texture_resource.Get(), &srv_desc, cpu_desc_handle
    );

    // Schedule a Signal command in the queue.
    ThrowIfFailed(commandQueue->Signal(fence.Get(), fenceValue));

    // Wait until the fence has been processed.
    ThrowIfFailed(fence->SetEventOnCompletion(fenceValue, fenceEvent));
    WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);

    //// Increment the fence value for the current frame.
    fenceValue++;
//}
    // ... WaitForGPU() - nie mo¿na usuwaæ texture_upload_buffer
    // zanim nie skopiuje siê jego zawartoœci




    
}

void D3DApp::update()
{
    vs_const_buffer_t vsConstBuffer;
    vsConstBuffer.colLight = { 1.f, 1.f, 1.f, 1.f };
    vsConstBuffer.dirLight = { 0.f, 0.f, 1.f, 0.f };

    //XMMATRIX wvp_matrix = XMMatrixTranspose(XMMatrixIdentity());
    XMMATRIX wvp_matrix = XMMatrixIdentity();

    XMStoreFloat4x4(
        &vsConstBuffer.matView,
        XMMatrixTranspose(wvp_matrix)
    );

    //wvp_matrix *= XMMatrixRotationY(2.f * angle);

    wvp_matrix *= XMMatrixTranslation(0.0f, -1.5f, 8.0f);

    if ((GetAsyncKeyState(VK_LEFT) & 0x8000) | (GetAsyncKeyState('A') & 0x8000))
        tempMatrix *= XMMatrixTranslation(0.1f, 0.0f, 0.0f);
    if ((GetAsyncKeyState(VK_RIGHT) & 0x8000) | (GetAsyncKeyState('D') & 0x8000))
        tempMatrix *= XMMatrixTranslation(-0.1f, 0.0f, 0.0f);
    if ((GetAsyncKeyState(VK_UP) & 0x8000) | (GetAsyncKeyState('W') & 0x8000))
        tempMatrix *= XMMatrixTranslation(0.0f, 0.0f, -0.1f);
    if ((GetAsyncKeyState(VK_DOWN) & 0x8000) | (GetAsyncKeyState('S') & 0x8000))
        tempMatrix *= XMMatrixTranslation(0.0f, 0.0f, 0.1f);
    if (GetAsyncKeyState('Q') & 0x8000)
        tempMatrix *= XMMatrixRotationY(0.02f);
    if (GetAsyncKeyState('E') & 0x8000)
        tempMatrix *= XMMatrixRotationY(-0.02f);

    wvp_matrix *= tempMatrix;

    XMStoreFloat4x4(
        &vsConstBuffer.matWorldView,
        XMMatrixTranspose(wvp_matrix)
    );

    // Projection space
    wvp_matrix = XMMatrixMultiply(
        wvp_matrix,
        XMMatrixPerspectiveFovLH(
            pi() / 4.f, viewport.Width / viewport.Height, 0.1f, 100.0f
        )
    );

    XMStoreFloat4x4(
        &vsConstBuffer.matWorldViewProj,
        XMMatrixTranspose(wvp_matrix)
    );

    memcpy(
        constBufferData,
        &vsConstBuffer,
        sizeof(vsConstBuffer)
    );

    angle += 1. / 128.;
}

void D3DApp::render()
{
    populateCommandList();

    ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    ThrowIfFailed(swapChain->Present(1, 0));

    waitForPreviousFrame();
}

void D3DApp::destroy()
{
    waitForPreviousFrame();

    CloseHandle(fenceEvent);
}

void D3DApp::populateCommandList()
{
    ThrowIfFailed(commandAllocator->Reset());
    ThrowIfFailed(commandList->Reset(commandAllocator.Get(), pipelineState.Get()));

    commandList->SetGraphicsRootSignature(rootSignature.Get());
    //commandList->SetGraphicsRootDescriptorTable(
    //    0, constBufferHeap->GetGPUDescriptorHandleForHeapStart()
    //);

    D3D12_GPU_DESCRIPTOR_HANDLE gpu_desc_handle =
        constBufferHeap->
        GetGPUDescriptorHandleForHeapStart();
    commandList->SetGraphicsRootDescriptorTable(
        0, gpu_desc_handle
    );
    gpu_desc_handle.ptr +=
        device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
        );
    commandList->SetGraphicsRootDescriptorTable(
        1, gpu_desc_handle
    );

    ID3D12DescriptorHeap* descHeaps[] = { constBufferHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(descHeaps), descHeaps);

    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);

    auto barriers = CD3DX12_RESOURCE_BARRIER::Transition(
        renderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &barriers);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(
        rtvHeap->GetCPUDescriptorHandleForHeapStart(), frameIndex, rtvDescriptorSize);

    auto dh = depthBufferHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE,
        &dh);

    const float clearColor[] = { 0.61f, 0.80f, 0.83f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    commandList->ClearDepthStencilView(
        depthBufferHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
    commandList->DrawInstanced(static_cast<UINT>(getVertices().second / sizeof(Vertex)), 1, 0, 0);

    commandList->IASetVertexBuffers(0, 1, &houseVertexBufferView);
    commandList->DrawInstanced(static_cast<UINT>(getHouseVertices().second / sizeof(Vertex)), 1, 0, 0);

    commandList->IASetVertexBuffers(0, 1, &groundVertexBufferView);
    commandList->DrawInstanced(static_cast<UINT>(getGroundVertices().second / sizeof(Vertex)), 1, 0, 0);



    barriers = CD3DX12_RESOURCE_BARRIER::Transition(
        renderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    commandList->ResourceBarrier(1, &barriers);

    ThrowIfFailed(commandList->Close());
}

void D3DApp::waitForPreviousFrame()
{
    const UINT64 fenceValueTmp = fenceValue;
    ThrowIfFailed(commandQueue->Signal(fence.Get(), fenceValueTmp));
    fenceValue++;

    if (fence->GetCompletedValue() < fenceValueTmp)
    {
        ThrowIfFailed(fence->SetEventOnCompletion(fenceValueTmp, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
    }

    frameIndex = swapChain->GetCurrentBackBufferIndex();
}

void D3DApp::createCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)));
}

void D3DApp::createSwapChain(ComPtr<IDXGIFactory7> factory)
{
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = 0;
    swapChainDesc.Height = 0;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChainTmp;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        commandQueue.Get(),
        WinApp::GetHwnd(),
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChainTmp
    ));

    ThrowIfFailed(factory->MakeWindowAssociation(WinApp::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

    ThrowIfFailed(swapChainTmp.As(&swapChain));
    frameIndex = swapChain->GetCurrentBackBufferIndex();
}

void D3DApp::createHeaps()
{
    // render target view (RTV) descriptor heap.
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)));

        rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    // Const buffer.
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 2;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        heapDesc.NodeMask = 0;
        ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&constBufferHeap)));
    }

    // Depth buffer.
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NodeMask = 0;
        ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&depthBufferHeap)));
    }
}

void D3DApp::createRenderTargets()
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());

    for (UINT i = 0; i < FrameCount; i++)
    {
        ThrowIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i])));
        device->CreateRenderTargetView(renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, rtvDescriptorSize);
    }
}

void D3DApp::createCommandAllocator()
{
    ThrowIfFailed(device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)));
}

void D3DApp::createRootSignature()
{
    D3D12_DESCRIPTOR_RANGE descriptorRanges[] = {
        {
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart =
                D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        },
        {
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart =
                D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        }
    };

    D3D12_ROOT_PARAMETER rootParameters[] = {
        {
            .ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = { 1, &descriptorRanges[0]},
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX
        },
        {
            .ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = { 1, &descriptorRanges[1]},
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL
        }
    };

    D3D12_STATIC_SAMPLER_DESC tex_sampler_desc = {
        .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        //D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_FILTER_ANISOTROPIC
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        //_MODE_MIRROR, _MODE_CLAMP, _MODE_BORDER
        .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        .MipLODBias = 0,
        .MaxAnisotropy = 0,
        .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
        .BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
        .MinLOD = 0.0f,
        .MaxLOD = D3D12_FLOAT32_MAX,
        .ShaderRegister = 0,
        .RegisterSpace = 0,
        .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL
    };

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {
        .NumParameters = _countof(rootParameters),
        .pParameters = rootParameters,
        .NumStaticSamplers = 1,
        .pStaticSamplers = &tex_sampler_desc,
        .Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS
    };

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    ThrowIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature)));
}

void D3DApp::createPipelineState()
{
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, 
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_RENDER_TARGET_BLEND_DESC renderTargetBlendDesc;
    renderTargetBlendDesc.BlendEnable = FALSE;
    renderTargetBlendDesc.LogicOpEnable = FALSE;
    renderTargetBlendDesc.SrcBlend = D3D12_BLEND_ONE;
    renderTargetBlendDesc.DestBlend = D3D12_BLEND_ZERO;
    renderTargetBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
    renderTargetBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
    renderTargetBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
    renderTargetBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    renderTargetBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
    renderTargetBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_BLEND_DESC blendStateDesc;
    blendStateDesc.AlphaToCoverageEnable = FALSE;
    blendStateDesc.IndependentBlendEnable = FALSE;
    blendStateDesc.RenderTarget[0] = renderTargetBlendDesc;

    D3D12_RASTERIZER_DESC rasterizerDesc;
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizerDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizerDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.MultisampleEnable = FALSE;
    rasterizerDesc.AntialiasedLineEnable = FALSE;
    rasterizerDesc.ForcedSampleCount = 0;
    rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc;
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDesc.StencilEnable = FALSE;
    depthStencilDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depthStencilDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depthStencilDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depthStencilDesc.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    depthStencilDesc.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;


    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.VS = { vs_main, sizeof(vs_main) };
    psoDesc.PS = { ps_main, sizeof(ps_main) };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendStateDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState)));
}

void D3DApp::createCommandList()
{
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        commandAllocator.Get(), pipelineState.Get(), IID_PPV_ARGS(&commandList)));
    ThrowIfFailed(commandList->Close());
}

void D3DApp::createBuffers()
{
    createVertexBuffer(vertexBuffer, getVertices(), vertexBufferView);
    createVertexBuffer(houseVertexBuffer, getHouseVertices(), houseVertexBufferView);
    createVertexBuffer(groundVertexBuffer, getGroundVertices(), groundVertexBufferView);
    createConstBuffer();
    createDepthBuffer();
}

void D3DApp::createVertexBuffer(ComPtr<ID3D12Resource>& buffer, 
    std::pair<Vertex*, size_t> vertices, D3D12_VERTEX_BUFFER_VIEW& bufferView)
{
    D3D12_HEAP_PROPERTIES heD3DApprops;
    heD3DApprops.Type = D3D12_HEAP_TYPE_UPLOAD;
    heD3DApprops.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heD3DApprops.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heD3DApprops.CreationNodeMask = 1;
    heD3DApprops.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc;
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = vertices.second;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ThrowIfFailed(device->CreateCommittedResource(
        &heD3DApprops,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&buffer)));

    UINT8* pVertexDataBegin;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(buffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin)));
    memcpy(pVertexDataBegin, vertices.first, vertices.second);
    buffer->Unmap(0, nullptr);

    bufferView.BufferLocation = buffer->GetGPUVirtualAddress();
    bufferView.StrideInBytes = sizeof(Vertex);
    bufferView.SizeInBytes = static_cast<UINT>(vertices.second);
}

void D3DApp::createConstBuffer()
{
    D3D12_HEAP_PROPERTIES heD3DApprops;
    heD3DApprops.Type = D3D12_HEAP_TYPE_UPLOAD;
    heD3DApprops.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heD3DApprops.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heD3DApprops.CreationNodeMask = 1;
    heD3DApprops.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc;
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = 2560;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ThrowIfFailed(device->CreateCommittedResource(
        &heD3DApprops,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&constBuffer)));

    D3D12_CONSTANT_BUFFER_VIEW_DESC constantBufferViewDesc;
    constantBufferViewDesc.BufferLocation = constBuffer->GetGPUVirtualAddress();
    constantBufferViewDesc.SizeInBytes = 2560;

    device->CreateConstantBufferView(&constantBufferViewDesc,
        constBufferHeap->GetCPUDescriptorHandleForHeapStart());

    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(constBuffer->Map(0, &readRange, reinterpret_cast<void**>(&constBufferData)));
}

void D3DApp::createFence()
{
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    fenceValue = 1;

    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent == nullptr)
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

    waitForPreviousFrame();
}

void D3DApp::createDepthBuffer()
{
    D3D12_HEAP_PROPERTIES heD3DApprops;
    heD3DApprops.Type = D3D12_HEAP_TYPE_DEFAULT;
    heD3DApprops.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heD3DApprops.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heD3DApprops.CreationNodeMask = 1;
    heD3DApprops.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc;
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = width;
    resourceDesc.Height = height;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 0;
    resourceDesc.Format = DXGI_FORMAT_D32_FLOAT;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue;
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    D3D12_DEPTH_STENCIL_VIEW_DESC depthViewDesc;
    depthViewDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    depthViewDesc.Flags = D3D12_DSV_FLAG_NONE;
    depthViewDesc.Texture2D = {};

    ThrowIfFailed(device->CreateCommittedResource(
        &heD3DApprops,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&depthBuffer))
    );

    device->CreateDepthStencilView(depthBuffer.Get(), &depthViewDesc,
        depthBufferHeap->GetCPUDescriptorHandleForHeapStart());
}

void D3DApp::resize()
{
    RECT rect;
    GetClientRect(WinApp::GetHwnd(), &rect);

    height = rect.bottom;
    width = rect.right;

    viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));

    waitForPreviousFrame();
    createDepthBuffer();
}

std::pair<Vertex*, size_t> D3DApp::getVertices()
{
    static Vertex data[] = {
        {0.00000f, 0.00000f, 0.00000f, -1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -1.33333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 0.00000f, -1.33333f, -1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, 1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, -1.33333f, 1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 1.33333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, -0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.94281f, 0.00000f, -0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.94281f, 0.00000f, -0.94281f, -0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, 0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.94281f, 0.00000f, -0.94281f, 0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.94281f, 0.00000f, 0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, -0.00000f, 0.00000f, -1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.00000f, 0.00000f, -1.33333f, 1.0f, 1.0f, 1.0f, 1.f},
        {1.33333f, 0.00000f, -0.00000f, -0.00000f, 0.00000f, -1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, 0.00000f, 0.00000f, 1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {1.33333f, 0.00000f, -0.00000f, 0.00000f, 0.00000f, 1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.00000f, 0.00000f, 1.33333f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, 0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.94281f, 0.00000f, -0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.94281f, 0.00000f, 0.94281f, 0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, -0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.94281f, 0.00000f, 0.94281f, -0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.94281f, 0.00000f, 0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, 1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 1.33333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 0.00000f, 1.33333f, 1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, -1.33333f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, 1.33333f, -1.33333f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -1.33333f, 0.00000f, 0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, 0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.94281f, 0.00000f, 0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {-0.94281f, 0.00000f, 0.94281f, 0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, -0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {-0.94281f, 0.00000f, 0.94281f, -0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.94281f, 0.00000f, -0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, 0.00000f, 0.00000f, 1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.00000f, 0.00000f, 1.33333f, 1.0f, 1.0f, 1.0f, 1.f},
        {-1.33333f, 0.00000f, 0.00000f, 0.00000f, 0.00000f, 1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, -0.00000f, 0.00000f, -1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {-1.33333f, 0.00000f, 0.00000f, -0.00000f, 0.00000f, -1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.00000f, 0.00000f, -1.33333f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, -0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.94281f, 0.00000f, 0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {-0.94281f, 0.00000f, -0.94281f, -0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, 0.00000f, 0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {-0.94281f, 0.00000f, -0.94281f, 0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.94281f, 0.00000f, -0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.80000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 2.00000f, -0.80000f, -0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, -0.80000f, 0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.80000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.56569f, 0.00000f, -0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.56569f, 2.00000f, -0.56569f, -0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.56569f, 2.00000f, -0.56569f, 0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.56569f, 0.00000f, 0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.00000f, 0.00000f, -0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.00000f, 0.00000f, -0.80000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.80000f, 2.00000f, -0.00000f, -0.00000f, 0.00000f, -0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.00000f, 0.00000f, 0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.80000f, 2.00000f, -0.00000f, 0.00000f, 0.00000f, 0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.00000f, 0.00000f, 0.80000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.56569f, 0.00000f, -0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.56569f, 2.00000f, 0.56569f, 0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.56569f, 2.00000f, 0.56569f, -0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.56569f, 0.00000f, 0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.80000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 2.00000f, 0.80000f, 0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.80000f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.80000f, -0.80000f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.80000f, 0.00000f, 0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.56569f, 0.00000f, 0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {-0.56569f, 2.00000f, 0.56569f, 0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {-0.56569f, 2.00000f, 0.56569f, -0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.56569f, 0.00000f, -0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.00000f, 0.00000f, 0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.00000f, 0.00000f, 0.80000f, 1.0f, 1.0f, 1.0f, 1.f},
        {-0.80000f, 2.00000f, 0.00000f, 0.00000f, 0.00000f, 0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.00000f, 0.00000f, -0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {-0.80000f, 2.00000f, 0.00000f, -0.00000f, 0.00000f, -0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.00000f, 0.00000f, -0.80000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, -0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.56569f, 0.00000f, 0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {-0.56569f, 2.00000f, -0.56569f, -0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 2.00000f, 0.00000f, 0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {-0.56569f, 2.00000f, -0.56569f, 0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.56569f, 0.00000f, -0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, -0.53333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 3.20000f, -0.53333f, -0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, -0.53333f, 0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, 0.53333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, -0.37712f, 0.00000f, -0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.37712f, 3.20000f, -0.37712f, -0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.37712f, 3.20000f, -0.37712f, 0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, 0.37712f, 0.00000f, 0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.00000f, 0.00000f, -0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, -0.00000f, 0.00000f, -0.53333f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.53333f, 3.20000f, -0.00000f, -0.00000f, 0.00000f, -0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.00000f, 0.00000f, 0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.53333f, 3.20000f, -0.00000f, 0.00000f, 0.00000f, 0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, 0.00000f, 0.00000f, 0.53333f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, 0.37712f, 0.00000f, -0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.37712f, 3.20000f, 0.37712f, 0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.37712f, 3.20000f, 0.37712f, -0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, -0.37712f, 0.00000f, 0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, 0.53333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 3.20000f, 0.53333f, 0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.53333f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.53333f, -0.53333f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, -0.53333f, 0.00000f, 0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, 0.37712f, 0.00000f, 0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {-0.37712f, 3.20000f, 0.37712f, 0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {-0.37712f, 3.20000f, 0.37712f, -0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, -0.37712f, 0.00000f, -0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.00000f, 0.00000f, 0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, 0.00000f, 0.00000f, 0.53333f, 1.0f, 1.0f, 1.0f, 1.f},
        {-0.53333f, 3.20000f, 0.00000f, 0.00000f, 0.00000f, 0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.00000f, 0.00000f, -0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {-0.53333f, 3.20000f, 0.00000f, -0.00000f, 0.00000f, -0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, -0.00000f, 0.00000f, -0.53333f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, -0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, -0.37712f, 0.00000f, 0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {-0.37712f, 3.20000f, -0.37712f, -0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 3.20000f, 0.00000f, 0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {-0.37712f, 3.20000f, -0.37712f, 0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 4.00000f, 0.00000f, 0.37712f, 0.00000f, -0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
    };

    return { data, sizeof(data) };
}

std::pair<Vertex*, size_t> D3DApp::getHouseVertices() {
    static Vertex data[] = {
        {10.00000f, 0.00000f, 20.00000f, -10.00000f, 0.00000f, -0.00000f, 0.0f, 0.0f, 0.0f, 0.f},
        {10.00000f, 10.00000f, 20.00000f, -10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 0.00000f, 10.00000f, -10.00000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},

        {10.00000f, 0.00000f, 20.00000f, 10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, .5f},
        {10.00000f, 0.00000f, 10.00000f, 10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, .5f},
        {10.00000f, 10.00000f, 20.00000f, 10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.0f},

        {10.00000f, 10.00000f, 10.00000f, 10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 0.0f},
        {10.00000f, 10.00000f, 20.00000f, 10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.0f},
        {10.00000f, 0.00000f, 10.00000f, 10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, .5f},

        {10.00000f, 10.00000f, 10.00000f, -10.00000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, 10.00000f, -10.00000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 10.00000f, 20.00000f, -10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        //

        {0.00000f, 0.00000f, 10.00000f, -0.00000f, 0.00000f, -10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, .5f},
        {0.00000f, 10.00000f, 10.00000f, -0.00000f, 0.00000f, -10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 0.0f},
        {10.00000f, 0.00000f, 10.00000f, -0.00000f, 0.00000f, -10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, .5f},

        {0.00000f, 0.00000f, 10.00000f, 0.00000f, 0.00000f, 10.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, 10.00000f, 0.00000f, 0.00000f, 10.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 10.00000f, 10.00000f, 0.00000f, 0.00000f, 10.00000f, 1.0f, 1.0f, 1.0f, 1.f},

        {10.00000f, 10.00000f, 10.00000f, -0.00000f, 0.00000f, 10.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 10.00000f, 10.00000f, -0.00000f, 0.00000f, 10.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 0.00000f, 10.00000f, -0.00000f, 0.00000f, 10.00000f, 0.0f, 1.0f, 0.0f, 1.f},

        {10.00000f, 10.00000f, 10.00000f, 0.00000f, 0.00000f, -10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.0f},
        {10.00000f, 0.00000f, 10.00000f, 0.00000f, 0.00000f, -10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, .5f},
        {0.00000f, 10.00000f, 10.00000f, 0.00000f, 0.00000f, -10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 0.0f},
        //
        {0.00000f, 0.00000f, 10.00000f, 10.00000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 10.00000f, 10.00000f, 10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 0.00000f, 20.00000f, 10.00000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},

        {0.00000f, 0.00000f, 10.00000f, -10.00000f, 0.00000f, 0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, .5f},
        {0.00000f, 0.00000f, 20.00000f, -10.00000f, 0.00000f, 0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, .5f},
        {0.00000f, 10.00000f, 10.00000f, -10.00000f, 0.00000f, 0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.0f},

        {0.00000f, 10.00000f, 20.00000f, -10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 0.0f},
        {0.00000f, 10.00000f, 10.00000f, -10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.0f},
        {0.00000f, 0.00000f, 20.00000f, -10.00000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, .5f},

        {0.00000f, 10.00000f, 20.00000f, 10.00000f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, 20.00000f, 10.00000f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 10.00000f, 10.00000f, 10.00000f, 0.00000f, 0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        //
        {10.00000f, 0.00000f, 20.00000f, 0.00000f, 0.00000f, 10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, .5f},
        {10.00000f, 10.00000f, 20.00000f, 0.00000f, 0.00000f, 10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 0.0f},
        {0.00000f, 0.00000f, 20.00000f, 0.00000f, 0.00000f, 10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, .5f},

        {10.00000f, 0.00000f, 20.00000f, -0.00000f, 0.00000f, -10.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {0.00000f, 0.00000f, 20.00000f, -0.00000f, 0.00000f, -10.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 10.00000f, 20.00000f, -0.00000f, 0.00000f, -10.00000f, 1.0f, 1.0f, 1.0f, 1.f},

        {0.00000f, 10.00000f, 20.00000f, 0.00000f, 0.00000f, -10.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 10.00000f, 20.00000f, 0.00000f, 0.00000f, -10.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {0.00000f, 0.00000f, 20.00000f, 0.00000f, 0.00000f, -10.00000f, 0.0f, 1.0f, 0.0f, 1.f},

        {0.00000f, 10.00000f, 20.00000f, -0.00000f, 0.00000f, 10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.0f},
        {0.00000f, 0.00000f, 20.00000f, -0.00000f, 0.00000f, 10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, .5f},
        {10.00000f, 10.00000f, 20.00000f, -0.00000f, 0.00000f, 10.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 0.0f}
    };

    return { data, sizeof(data) };
}

std::pair<Vertex*, size_t> D3DApp::getGroundVertices() {
    const FLOAT max_dist = 20.0f;


    static Vertex data[] = {
        { - max_dist, 0.00000f, 0.00000f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 1.0f, 1},
        { 0.00000f, 0.00000f, -max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.5f, 1},
        { - max_dist, 0.00000f, - max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 0.5f, 1},

        { -max_dist, 0.00000f, 0.00000f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 1.0f, 1},
        { 0.0f, 0.00000f, 0.0f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 1.0f, 1},
        { 0.00000f, 0.00000f, -max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.5f, 1},
        //
        { -max_dist, 0.00000f, max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 1.0f, 1},
        { 0.00000f, 0.00000f, 0.0f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.5f, 1},
        { -max_dist, 0.00000f, 0.0f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 0.5f, 1},
        
        { -max_dist, 0.00000f, max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 1.0f, 1},
        { 0.0f, 0.00000f, max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 1.0f, 1},
        { 0.00000f, 0.00000f, 0.0f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.5f, 1},
        //
        { 0.0f, 0.00000f, max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 1.0f, 1},
        { max_dist, 0.00000f, 0.0f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.5f, 1},
        { 0.0f, 0.00000f, 0.0f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 0.5f, 1},

        { 0.0f, 0.00000f, max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 1.0f, 1},
        { max_dist, 0.00000f, max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 1.0f, 1},
        { max_dist, 0.00000f, 0.0f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.5f, 1},
        //
        { 0.0f, 0.00000f, 0.00000f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 1.0f, 1},
        { max_dist, 0.00000f, -max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.5f, 1},
        { 0.0f, 0.00000f, -max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 0.5f, 1},

        { 0.0f, 0.00000f, 0.00000f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 0.0f, 1.0f, 1},
        { max_dist, 0.00000f, 0.0f, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 1.0f, 1},
        { max_dist, 0.00000f, -max_dist, 1.00000f, 0.00000f, 1.00000f, 1.0f, 1.0f, 1.0f, 1.f, 1.0f, 0.5f, 1},
    };

    return { data, sizeof(data)};
}

HRESULT D3DApp::LoadBitmapFromFile(
    PCWSTR uri, UINT& width, UINT& height, BYTE** ppBits
) {
    HRESULT hr;
    
    IWICBitmapDecoder* pDecoder = nullptr;
    IWICBitmapFrameDecode* pSource = nullptr;
    IWICFormatConverter* pConverter = nullptr;
    hr = wic_factory->CreateDecoderFromFilename(
        uri, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
        &pDecoder
    );
    if (SUCCEEDED(hr)) {
        hr = pDecoder->GetFrame(0, &pSource);
    }
    if (SUCCEEDED(hr)) {
        hr = wic_factory->CreateFormatConverter(&pConverter);
    }
    if (SUCCEEDED(hr)) {
        hr = pConverter->Initialize(
            pSource,
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeMedianCut
        );
    }
    if (SUCCEEDED(hr)) {
        hr = pConverter->GetSize(&width, &height);
    }
    if (SUCCEEDED(hr)) {
        *ppBits = new BYTE[4 * width * height];
        hr = pConverter->CopyPixels(
            nullptr, 4 * width, 4 * width * height, *ppBits
        );
    }
    if (pDecoder) pDecoder->Release();
    if (pSource) pSource->Release();
    if (pConverter) pConverter->Release();
    return hr;
}
