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

    commandList->IASetVertexBuffers(0, 1, &rockVertexBufferView);
    commandList->DrawInstanced(static_cast<UINT>(getRockVertices().second / sizeof(Vertex)), 1, 0, 0);



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
    createVertexBuffer(rockVertexBuffer, getRockVertices(), rockVertexBufferView);
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
        {10.00000f, 0.00000f, 0.00000f, -1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -1.33333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 0.00000f, -1.33333f, -1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, 1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, -1.33333f, 1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 1.33333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, -0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.94281f, 0.00000f, -0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.94281f, 0.00000f, -0.94281f, -0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, 0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.94281f, 0.00000f, -0.94281f, 0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.94281f, 0.00000f, 0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, -0.00000f, 0.00000f, -1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.00000f, 0.00000f, -1.33333f, 1.0f, 1.0f, 1.0f, 1.f},
        {11.33333f, 0.00000f, -0.00000f, -0.00000f, 0.00000f, -1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, 0.00000f, 0.00000f, 1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {11.33333f, 0.00000f, -0.00000f, 0.00000f, 0.00000f, 1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.00000f, 0.00000f, 1.33333f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, 0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.94281f, 0.00000f, -0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.94281f, 0.00000f, 0.94281f, 0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, -0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.94281f, 0.00000f, 0.94281f, -0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.94281f, 0.00000f, 0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, 1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 1.33333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 0.00000f, 1.33333f, 1.33333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, -1.33333f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, 1.33333f, -1.33333f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -1.33333f, 0.00000f, 0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, 0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.94281f, 0.00000f, 0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.0f -0.94281f, 0.00000f, 0.94281f, 0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, -0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.0f -0.94281f, 0.00000f, 0.94281f, -0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.94281f, 0.00000f, -0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, 0.00000f, 0.00000f, 1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.00000f, 0.00000f, 1.33333f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.0f -1.33333f, 0.00000f, 0.00000f, 0.00000f, 0.00000f, 1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, -0.00000f, 0.00000f, -1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.0f -1.33333f, 0.00000f, 0.00000f, -0.00000f, 0.00000f, -1.33333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.00000f, 0.00000f, -1.33333f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, -0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.94281f, 0.00000f, 0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.0f -0.94281f, 0.00000f, -0.94281f, -0.94281f, 0.00000f, 0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 0.00000f, 0.00000f, 0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.0f -0.94281f, 0.00000f, -0.94281f, 0.94281f, 0.00000f, -0.94281f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.94281f, 0.00000f, -0.94281f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.80000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 2.00000f, -0.80000f, -0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, -0.80000f, 0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.80000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.56569f, 0.00000f, -0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.56569f, 2.00000f, -0.56569f, -0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.56569f, 2.00000f, -0.56569f, 0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.56569f, 0.00000f, 0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.00000f, 0.00000f, -0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.00000f, 0.00000f, -0.80000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.80000f, 2.00000f, -0.00000f, -0.00000f, 0.00000f, -0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.00000f, 0.00000f, 0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.80000f, 2.00000f, -0.00000f, 0.00000f, 0.00000f, 0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.00000f, 0.00000f, 0.80000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.56569f, 0.00000f, -0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.56569f, 2.00000f, 0.56569f, 0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.56569f, 2.00000f, 0.56569f, -0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.56569f, 0.00000f, 0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.80000f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 2.00000f, 0.80000f, 0.80000f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.80000f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.80000f, -0.80000f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.80000f, 0.00000f, 0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.56569f, 0.00000f, 0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.0f -0.56569f, 2.00000f, 0.56569f, 0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.0f -0.56569f, 2.00000f, 0.56569f, -0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.56569f, 0.00000f, -0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.00000f, 0.00000f, 0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.00000f, 0.00000f, 0.80000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.0f -0.80000f, 2.00000f, 0.00000f, 0.00000f, 0.00000f, 0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.00000f, 0.00000f, -0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.0f -0.80000f, 2.00000f, 0.00000f, -0.00000f, 0.00000f, -0.80000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.00000f, 0.00000f, -0.80000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, -0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.56569f, 0.00000f, 0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.0f -0.56569f, 2.00000f, -0.56569f, -0.56569f, 0.00000f, 0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 2.00000f, 0.00000f, 0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.0f -0.56569f, 2.00000f, -0.56569f, 0.56569f, 0.00000f, -0.56569f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.56569f, 0.00000f, -0.56569f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, -0.53333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 3.20000f, -0.53333f, -0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, -0.53333f, 0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, 0.53333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, -0.37712f, 0.00000f, -0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.37712f, 3.20000f, -0.37712f, -0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.37712f, 3.20000f, -0.37712f, 0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, 0.37712f, 0.00000f, 0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.00000f, 0.00000f, -0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, -0.00000f, 0.00000f, -0.53333f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.53333f, 3.20000f, -0.00000f, -0.00000f, 0.00000f, -0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.00000f, 0.00000f, 0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.53333f, 3.20000f, -0.00000f, 0.00000f, 0.00000f, 0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, 0.00000f, 0.00000f, 0.53333f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, 0.37712f, 0.00000f, -0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.37712f, 3.20000f, 0.37712f, 0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.37712f, 3.20000f, 0.37712f, -0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, -0.37712f, 0.00000f, 0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, 0.53333f, 0.00000f, -0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 3.20000f, 0.53333f, 0.53333f, 0.00000f, -0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.53333f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.53333f, -0.53333f, 0.00000f, 0.00000f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, -0.53333f, 0.00000f, 0.00000f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, 0.37712f, 0.00000f, 0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        { 10.0f -0.37712f, 3.20000f, 0.37712f, 0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        { 10.0f -0.37712f, 3.20000f, 0.37712f, -0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, -0.37712f, 0.00000f, -0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.00000f, 0.00000f, 0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, 0.00000f, 0.00000f, 0.53333f, 1.0f, 1.0f, 1.0f, 1.f},
        { 10.0f -0.53333f, 3.20000f, 0.00000f, 0.00000f, 0.00000f, 0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.00000f, 0.00000f, -0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        { 10.0f -0.53333f, 3.20000f, 0.00000f, -0.00000f, 0.00000f, -0.53333f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, -0.00000f, 0.00000f, -0.53333f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, -0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, -0.37712f, 0.00000f, 0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
        {10.0f-0.37712f, 3.20000f, -0.37712f, -0.37712f, 0.00000f, 0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 3.20000f, 0.00000f, 0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        { 10.0f -0.37712f, 3.20000f, -0.37712f, 0.37712f, 0.00000f, -0.37712f, 0.0f, 1.0f, 0.0f, 1.f},
        {10.00000f, 4.00000f, 0.00000f, 0.37712f, 0.00000f, -0.37712f, 1.0f, 1.0f, 1.0f, 1.f},
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

std::pair<Vertex*, size_t> D3DApp::getRockVertices() {
    static Vertex data[] = {
        { 0.431201f, 0.609609f, -0.431201f, 0.3450f, 0.7908f, 0.5056f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.256078f, 0.574043f, -0.256078f, 0.3450f, 0.7908f, 0.5056f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.362647f, 0.518982f, -0.242676f, 0.3450f, 0.7908f, 0.5056f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.336864f, 0.745681f, -0.507358f, 0.7895f, 0.3581f, 0.4985f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185726f, 0.895799f, -0.375838f, 0.7895f, 0.3581f, 0.4985f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.256078f, 0.574043f, -0.256078f, 0.7895f, 0.3581f, 0.4985f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.173045f, 0.780299f, -0.526111f, -0.8128f, 0.4930f, -0.3103f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.647736f, -0.283424f, -0.8128f, 0.4930f, -0.3103f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185726f, 0.895799f, -0.375838f, -0.8128f, 0.4930f, -0.3103f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.179752f, 0.812846f, -0.546567f, 0.3148f, 0.8859f, 0.3408f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.647736f, -0.283424f, 0.3148f, 0.8859f, 0.3408f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.728611f, -0.493650f, 0.3148f, 0.8859f, 0.3408f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.301697f, 0.661039f, -0.453327f, -0.7758f, 0.6308f, 0.0124f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.171405f, 0.819202f, -0.347109f, -0.7758f, 0.6308f, 0.0124f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.179752f, 0.812846f, -0.546567f, -0.7758f, 0.6308f, 0.0124f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.458239f, 0.650889f, -0.458239f, -0.0165f, 0.6297f, -0.7767f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.342433f, 0.796756f, -0.342433f, -0.0165f, 0.6297f, -0.7767f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.301697f, 0.661039f, -0.453327f, -0.0165f, 0.6297f, -0.7767f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.362647f, 0.518982f, -0.242676f, 0.9948f, 0.0621f, 0.0812f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.339102f, 0.797856f, -0.167415f, 0.9948f, 0.0621f, 0.0812f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.353197f, 0.505187f, -0.116353f, 0.9948f, 0.0621f, 0.0812f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.256078f, 0.574043f, -0.256078f, -0.5304f, 0.4749f, -0.7022f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.133430f, 0.618410f, -0.133430f, -0.5304f, 0.4749f, -0.7022f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.339102f, 0.797856f, -0.167415f, -0.5304f, 0.4749f, -0.7022f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.185726f, 0.895799f, -0.375838f, 0.5401f, 0.4937f, 0.6815f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.811828f, -0.167809f, 0.5401f, 0.4937f, 0.6815f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.133430f, 0.618410f, -0.133430f, 0.5401f, 0.4937f, 0.6815f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.171405f, 0.819202f, -0.347109f, 0.6606f, 0.4324f, -0.6137f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.811828f, -0.167809f, 0.6606f, 0.4324f, -0.6137f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.647736f, -0.283424f, 0.6606f, 0.4324f, -0.6137f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.342433f, 0.796756f, -0.342433f, -0.1222f, 0.9726f, 0.1978f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.162598f, 0.782778f, -0.162598f, -0.1222f, 0.9726f, 0.1978f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.171405f, 0.819202f, -0.347109f, -0.1222f, 0.9726f, 0.1978f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.315001f, 0.444342f, -0.211664f, -0.9969f, -0.0781f, -0.0012f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.343691f, 0.810090f, -0.169702f, -0.9969f, -0.0781f, -0.0012f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.342433f, 0.796756f, -0.342433f, -0.9969f, -0.0781f, -0.0012f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.353197f, 0.505187f, -0.116353f, -0.2124f, 0.5776f, -0.7882f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.279382f, 0.636835f, -0.000000f, -0.2124f, 0.5776f, -0.7882f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.483113f, 0.711750f, 0.000000f, -0.2124f, 0.5776f, -0.7882f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.339102f, 0.797856f, -0.167415f, 0.6340f, 0.4319f, 0.6415f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.166254f, 0.802906f, -0.000000f, 0.6340f, 0.4319f, 0.6415f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.279382f, 0.636835f, -0.000000f, 0.6340f, 0.4319f, 0.6415f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.133430f, 0.618410f, -0.133430f, 0.6620f, 0.3583f, -0.6583f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 1.110028f, -0.000000f, 0.6620f, 0.3583f, -0.6583f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.166254f, 0.802906f, -0.000000f, 0.6620f, 0.3583f, -0.6583f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.162598f, 0.782778f, -0.162598f, -0.1148f, 0.4872f, -0.8657f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 1.110028f, -0.000000f, -0.1148f, 0.4872f, -0.8657f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.811828f, -0.167809f, -0.1148f, 0.4872f, -0.8657f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.343691f, 0.810090f, -0.169702f, 0.1258f, 0.6357f, -0.7616f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.197909f, 0.984574f, -0.000000f, 0.1258f, 0.6357f, -0.7616f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.162598f, 0.782778f, -0.162598f, 0.1258f, 0.6357f, -0.7616f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.491815f, 0.725733f, -0.161801f, -0.4235f, 0.7859f, 0.4506f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.313565f, 0.729030f, -0.000000f, -0.4235f, 0.7859f, 0.4506f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.343691f, 0.810090f, -0.169702f, -0.4235f, 0.7859f, 0.4506f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.279382f, 0.636835f, -0.000000f, -0.3451f, 0.9385f, 0.0122f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.481310f, 0.709020f, 0.158357f, -0.3451f, 0.9385f, 0.0122f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.483113f, 0.711750f, 0.000000f, -0.3451f, 0.9385f, 0.0122f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.166254f, 0.802906f, -0.000000f, 0.5557f, 0.3785f, -0.7402f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.364005f, 0.864249f, 0.179827f, 0.5557f, 0.3785f, -0.7402f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.279382f, 0.636835f, -0.000000f, 0.5557f, 0.3785f, -0.7402f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 1.110028f, -0.000000f, 0.8753f, 0.4738f, -0.0972f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170876f, 0.829425f, 0.170876f, 0.8753f, 0.4738f, -0.0972f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.166254f, 0.802906f, -0.000000f, 0.8753f, 0.4738f, -0.0972f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 1.110028f, -0.000000f, 0.1474f, 0.3044f, 0.9411f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.150869f, 0.716685f, 0.150869f, 0.1474f, 0.3044f, 0.9411f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.668472f, 0.142829f, 0.1474f, 0.3044f, 0.9411f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.197909f, 0.984574f, -0.000000f, 0.1481f, 0.5049f, 0.8504f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.322342f, 0.753172f, 0.159061f, 0.1481f, 0.5049f, 0.8504f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.150869f, 0.716685f, 0.150869f, 0.1481f, 0.5049f, 0.8504f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.313565f, 0.729030f, -0.000000f, 0.5564f, 0.8255f, -0.0946f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.653224f, 0.982542f, 0.214721f, 0.5564f, 0.8255f, -0.0946f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.322342f, 0.753172f, 0.159061f, 0.5564f, 0.8255f, -0.0946f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.364005f, 0.864249f, 0.179827f, 0.7939f, 0.6063f, -0.0465f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.487157f, 0.714036f, 0.323716f, 0.7939f, 0.6063f, -0.0465f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.481310f, 0.709020f, 0.158357f, 0.7939f, 0.6063f, -0.0465f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.170876f, 0.829425f, 0.170876f, -0.0832f, 0.2112f, 0.9739f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.240776f, 0.534579f, 0.240776f, -0.0832f, 0.2112f, 0.9739f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.364005f, 0.864249f, 0.179827f, -0.0832f, 0.2112f, 0.9739f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.668472f, 0.142829f, -0.6777f, 0.6647f, 0.3143f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.157657f, 0.745663f, 0.319526f, -0.6777f, 0.6647f, 0.3143f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170876f, 0.829425f, 0.170876f, -0.6777f, 0.6647f, 0.3143f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.668472f, 0.142829f, -0.7916f, 0.6002f, -0.1144f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.117652f, 0.531685f, 0.239267f, -0.7916f, 0.6002f, -0.1144f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.698882f, 0.302387f, -0.7916f, 0.6002f, -0.1144f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.150869f, 0.716685f, 0.150869f, 0.2565f, 0.4538f, 0.8534f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.257172f, 0.576865f, 0.257172f, 0.2565f, 0.4538f, 0.8534f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.117652f, 0.531685f, 0.239267f, 0.2565f, 0.4538f, 0.8534f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.322342f, 0.753172f, 0.159061f, 0.5601f, 0.5512f, 0.6185f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.504213f, 0.740755f, 0.334817f, 0.5601f, 0.5512f, 0.6185f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.257172f, 0.576865f, 0.257172f, 0.5601f, 0.5512f, 0.6185f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.240776f, 0.534579f, 0.240776f, -0.4808f, 0.2753f, 0.8325f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.317692f, 0.436313f, 0.317692f, -0.4808f, 0.2753f, 0.8325f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.487157f, 0.714036f, 0.323716f, -0.4808f, 0.2753f, 0.8325f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.157657f, 0.745663f, 0.319526f, 0.6965f, 0.4749f, -0.5378f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313369f, 0.689132f, 0.471260f, 0.6965f, 0.4749f, -0.5378f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.240776f, 0.534579f, 0.240776f, 0.6965f, 0.4749f, -0.5378f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.698882f, 0.302387f, -0.1765f, 0.2457f, 0.9531f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.121374f, 0.529553f, 0.368511f, -0.1765f, 0.2457f, 0.9531f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.157657f, 0.745663f, 0.319526f, -0.1765f, 0.2457f, 0.9531f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.698882f, 0.302387f, -0.5833f, 0.7478f, -0.3171f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.145037f, 0.644381f, 0.440683f, -0.5833f, 0.7478f, -0.3171f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.798499f, 0.537322f, -0.5833f, 0.7478f, -0.3171f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.117652f, 0.531685f, 0.239267f, -0.7268f, 0.5526f, -0.4080f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.219242f, 0.462580f, 0.326643f, -0.7268f, 0.5526f, -0.4080f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.145037f, 0.644381f, 0.440683f, -0.7268f, 0.5526f, -0.4080f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.257172f, 0.576865f, 0.257172f, 0.6533f, 0.5396f, 0.5311f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.436330f, 0.617440f, 0.436330f, 0.6533f, 0.5396f, 0.5311f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.219242f, 0.462580f, 0.326643f, 0.6533f, 0.5396f, 0.5311f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.421482f, 0.421482f, -0.421482f, -0.1545f, -0.0430f, -0.9870f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.431201f, 0.609609f, -0.431201f, -0.1545f, -0.0430f, -0.9870f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.583262f, 0.583262f, -0.453862f, -0.1545f, -0.0430f, -0.9870f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.325094f, 0.406197f, -0.406197f, 0.3247f, -0.2804f, -0.9033f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.336864f, 0.745681f, -0.507358f, 0.3247f, -0.2804f, -0.9033f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.431201f, 0.609609f, -0.431201f, 0.3247f, -0.2804f, -0.9033f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.280869f, 0.540470f, -0.540470f, 0.1381f, 0.1209f, -0.9830f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.173045f, 0.780299f, -0.526111f, 0.1381f, 0.1209f, -0.9830f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.336864f, 0.745681f, -0.507358f, 0.1381f, 0.1209f, -0.9830f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.143451f, 0.559224f, -0.559224f, -0.2316f, 0.1744f, -0.9571f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.728611f, -0.493650f, -0.2316f, 0.1744f, -0.9571f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.173045f, 0.780299f, -0.526111f, -0.2316f, 0.1744f, -0.9571f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.161804f, 0.633768f, -0.633768f, -0.4823f, 0.8753f, -0.0355f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.728611f, -0.493650f, -0.4823f, 0.8753f, -0.0355f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.719448f, -0.719448f, -0.4823f, 0.8753f, -0.0355f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.193485f, 0.360709f, -0.360709f, -0.9892f, -0.0291f, -0.1439f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.179752f, 0.812846f, -0.546567f, -0.9892f, -0.0291f, -0.1439f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.161804f, 0.633768f, -0.633768f, -0.9892f, -0.0291f, -0.1439f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.429708f, 0.550049f, -0.550049f, 0.6146f, -0.0217f, -0.7885f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.301697f, 0.661039f, -0.453327f, 0.6146f, -0.0217f, -0.7885f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.193485f, 0.360709f, -0.360709f, 0.6146f, -0.0217f, -0.7885f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.271962f, 0.271962f, -0.271962f, -0.9139f, -0.3824f, 0.1360f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.458239f, 0.650889f, -0.458239f, -0.9139f, -0.3824f, 0.1360f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.429708f, 0.550049f, -0.550049f, -0.9139f, -0.3824f, 0.1360f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.477370f, 0.477370f, -0.376854f, -0.6170f, 0.3889f, 0.6842f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.315001f, 0.444342f, -0.211664f, -0.6170f, 0.3889f, 0.6842f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.458239f, 0.650889f, -0.458239f, -0.6170f, 0.3889f, 0.6842f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.601862f, 0.601862f, -0.310713f, 0.5011f, 0.4462f, -0.7415f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.491815f, 0.725733f, -0.161801f, 0.5011f, 0.4462f, -0.7415f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.315001f, 0.444342f, -0.211664f, 0.5011f, 0.4462f, -0.7415f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.755952f, 0.755952f, -0.191887f, 0.1517f, 0.8818f, -0.4466f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.549233f, 0.817561f, 0.000000f, 0.1517f, 0.8818f, -0.4466f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.491815f, 0.725733f, -0.161801f, 0.1517f, 0.8818f, -0.4466f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.549233f, 0.817561f, 0.000000f, -0.9134f, -0.0498f, -0.4041f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.606308f, 0.606308f, 0.155044f, -0.9134f, -0.0498f, -0.4041f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.653224f, 0.982542f, 0.214721f, -0.9134f, -0.0498f, -0.4041f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.653224f, 0.982542f, 0.214721f, -0.8136f, -0.2379f, 0.5305f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.482247f, 0.482247f, 0.252566f, -0.8136f, -0.2379f, 0.5305f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.504213f, 0.740755f, 0.334817f, -0.8136f, -0.2379f, 0.5305f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.504213f, 0.740755f, 0.334817f, -0.8578f, -0.0513f, 0.5113f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467203f, 0.467203f, 0.369460f, -0.8578f, -0.0513f, 0.5113f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.436330f, 0.617440f, 0.436330f, -0.8578f, -0.0513f, 0.5113f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.436330f, 0.617440f, 0.436330f, -0.9663f, 0.1453f, -0.2127f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.465606f, 0.465606f, 0.465606f, -0.9663f, 0.1453f, -0.2127f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.475932f, 0.613611f, 0.613611f, -0.9663f, 0.1453f, -0.2127f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.219242f, 0.462580f, 0.326643f, 0.0940f, 0.9132f, -0.3965f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.475932f, 0.613611f, 0.613611f, 0.0940f, 0.9132f, -0.3965f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.300945f, 0.581767f, 0.581767f, 0.0940f, 0.9132f, -0.3965f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.145037f, 0.644381f, 0.440683f, 0.6386f, 0.1219f, 0.7598f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.300945f, 0.581767f, 0.581767f, 0.6386f, 0.1219f, 0.7598f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.116025f, 0.447827f, 0.447827f, 0.6386f, 0.1219f, 0.7598f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.798499f, 0.537322f, -0.9324f, 0.2394f, 0.2707f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.116025f, 0.447827f, 0.447827f, -0.9324f, 0.2394f, 0.2707f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.659898f, 0.659898f, -0.9324f, 0.2394f, 0.2707f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.798499f, 0.537322f, 0.6941f, 0.5805f, -0.4258f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.180243f, 0.708659f, 0.708659f, 0.6941f, 0.5805f, -0.4258f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.121374f, 0.529553f, 0.368511f, 0.6941f, 0.5805f, -0.4258f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.121374f, 0.529553f, 0.368511f, -0.6628f, 0.3833f, 0.6433f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.274601f, 0.527575f, 0.527575f, -0.6628f, 0.3833f, 0.6433f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313369f, 0.689132f, 0.471260f, -0.6628f, 0.3833f, 0.6433f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.313369f, 0.689132f, 0.471260f, 0.8864f, -0.2292f, 0.4023f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.281453f, 0.346188f, 0.346188f, 0.8864f, -0.2292f, 0.4023f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.317692f, 0.436313f, 0.317692f, 0.8864f, -0.2292f, 0.4023f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.317692f, 0.436313f, 0.317692f, -0.2480f, 0.9495f, -0.1922f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.538827f, 0.538827f, 0.538827f, -0.2480f, 0.9495f, -0.1922f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.498710f, 0.498710f, 0.392372f, -0.2480f, 0.9495f, -0.1922f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.487157f, 0.714036f, 0.323716f, -0.2564f, 0.2811f, 0.9248f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.498710f, 0.498710f, 0.392372f, -0.2564f, 0.2811f, 0.9248f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.755718f, 0.755718f, 0.385504f, -0.2564f, 0.2811f, 0.9248f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.481310f, 0.709020f, 0.158357f, 0.4370f, 0.6167f, -0.6547f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.755718f, 0.755718f, 0.385504f, 0.4370f, 0.6167f, -0.6547f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.613645f, 0.613645f, 0.156850f, 0.4370f, 0.6167f, -0.6547f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.483113f, 0.711750f, 0.000000f, 0.6934f, 0.7079f, -0.1343f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.613645f, 0.613645f, 0.156850f, 0.6934f, 0.7079f, -0.1343f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.598613f, 0.598613f, -0.000000f, 0.6934f, 0.7079f, -0.1343f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.713197f, 0.713197f, -0.181360f, 0.5218f, 0.5327f, 0.6663f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.483113f, 0.711750f, 0.000000f, 0.5218f, 0.5327f, 0.6663f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.598613f, 0.598613f, -0.000000f, 0.5218f, 0.5327f, 0.6663f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.462871f, 0.462871f, -0.243147f, -0.4738f, 0.6265f, -0.6189f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.353197f, 0.505187f, -0.116353f, -0.4738f, 0.6265f, -0.6189f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.713197f, 0.713197f, -0.181360f, -0.4738f, 0.6265f, -0.6189f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.583262f, 0.583262f, -0.453862f, 0.3877f, 0.6873f, 0.6142f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.362647f, 0.518982f, -0.242676f, 0.3877f, 0.6873f, 0.6142f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.462871f, 0.462871f, -0.243147f, 0.3877f, 0.6873f, 0.6142f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.382515f, -0.382515f, 0.535280f, 0.0518f, -0.9670f, 0.2495f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.327745f, -0.327745f, 0.758876f, 0.0518f, -0.9670f, 0.2495f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.256950f, -0.384578f, 0.553339f, 0.0518f, -0.9670f, 0.2495f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.404388f, -0.269844f, 0.584372f, 0.8818f, -0.3961f, 0.2559f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.363809f, -0.179730f, 0.863728f, 0.8818f, -0.3961f, 0.2559f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.327745f, -0.327745f, 0.758876f, 0.8818f, -0.3961f, 0.2559f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.313143f, -0.103221f, 0.441460f, 0.9806f, 0.1762f, -0.0857f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.321650f, -0.000000f, 0.750837f, 0.9806f, 0.1762f, -0.0857f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.363809f, -0.179730f, 0.863728f, 0.9806f, 0.1762f, -0.0857f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.504643f, 0.166007f, 0.746144f, 0.5190f, -0.5537f, 0.6512f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.321650f, -0.000000f, 0.750837f, 0.5190f, -0.5537f, 0.6512f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.445738f, -0.000000f, 0.651939f, 0.5190f, -0.5537f, 0.6512f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.537106f, 0.356226f, 0.792284f, 0.5786f, -0.2842f, 0.7645f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.361990f, 0.178823f, 0.858877f, 0.5786f, -0.2842f, 0.7645f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.504643f, 0.166007f, 0.746144f, 0.5786f, -0.2842f, 0.7645f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.487306f, 0.487306f, 0.695266f, -0.6918f, 0.2410f, 0.6807f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.239416f, 0.239416f, 0.531072f, -0.6918f, 0.2410f, 0.6807f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.537106f, 0.356226f, 0.792284f, -0.6918f, 0.2410f, 0.6807f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.256950f, -0.384578f, 0.553339f, 0.8433f, -0.0006f, 0.5375f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.152390f, -0.308959f, 0.717490f, 0.8433f, -0.0006f, 0.5375f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.158186f, -0.480788f, 0.708190f, 0.8433f, -0.0006f, 0.5375f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.327745f, -0.327745f, 0.758876f, -0.2365f, -0.0728f, 0.9689f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.153123f, -0.153123f, 0.729385f, -0.2365f, -0.0728f, 0.9689f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.152390f, -0.308959f, 0.717490f, -0.2365f, -0.0728f, 0.9689f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.363809f, -0.179730f, 0.863728f, -0.4714f, -0.6332f, 0.6139f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185262f, -0.000000f, 0.911990f, -0.4714f, -0.6332f, 0.6139f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.153123f, -0.153123f, 0.729385f, -0.4714f, -0.6332f, 0.6139f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.361990f, 0.178823f, 0.858877f, 0.6653f, -0.4903f, 0.5631f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185262f, -0.000000f, 0.911990f, 0.6653f, -0.4903f, 0.5631f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.321650f, -0.000000f, 0.750837f, 0.6653f, -0.4903f, 0.5631f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.239416f, 0.239416f, 0.531072f, -0.0643f, 0.9767f, 0.2046f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.171598f, 0.171598f, 0.833493f, -0.0643f, 0.9767f, 0.2046f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.361990f, 0.178823f, 0.858877f, -0.0643f, 0.9767f, 0.2046f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.288454f, 0.432981f, 0.629165f, 0.8492f, -0.3936f, 0.3521f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170735f, 0.345763f, 0.815613f, 0.8492f, -0.3936f, 0.3521f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.239416f, 0.239416f, 0.531072f, 0.8492f, -0.3936f, 0.3521f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.158186f, -0.480788f, 0.708190f, 0.5027f, 0.2108f, 0.8384f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.326629f, 0.764267f, 0.5027f, 0.2108f, 0.8384f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.551223f, 0.820744f, 0.5027f, 0.2108f, 0.8384f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.152390f, -0.308959f, 0.717490f, 0.2726f, 0.1601f, 0.9487f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.154457f, 0.735207f, 0.2726f, 0.1601f, 0.9487f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.326629f, 0.764267f, 0.2726f, 0.1601f, 0.9487f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.153123f, -0.153123f, 0.729385f, 0.0279f, -0.8406f, 0.5410f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.000000f, 0.975207f, 0.0279f, -0.8406f, 0.5410f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.154457f, 0.735207f, 0.0279f, -0.8406f, 0.5410f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.171598f, 0.171598f, 0.833493f, 0.2935f, 0.4169f, 0.8603f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.000000f, 0.975207f, 0.2935f, 0.4169f, 0.8603f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185262f, -0.000000f, 0.911990f, 0.2935f, 0.4169f, 0.8603f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.170735f, 0.345763f, 0.815613f, -0.8405f, 0.0512f, 0.5394f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.125747f, 0.570440f, -0.8405f, 0.0512f, 0.5394f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.171598f, 0.171598f, 0.833493f, -0.8405f, 0.0512f, 0.5394f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.123834f, 0.376013f, 0.541488f, 0.4001f, 0.9159f, 0.0326f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.413812f, 0.999413f, 0.4001f, 0.9159f, 0.0326f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170735f, 0.345763f, 0.815613f, 0.4001f, 0.9159f, 0.0326f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.326629f, 0.764267f, -0.8393f, 0.1326f, 0.5272f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.130535f, -0.396451f, 0.574005f, -0.8393f, 0.1326f, 0.5272f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.551223f, 0.820744f, -0.8393f, 0.1326f, 0.5272f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.154457f, 0.735207f, 0.0805f, 0.1659f, 0.9829f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.163810f, -0.331870f, 0.778574f, 0.0805f, 0.1659f, 0.9829f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.326629f, 0.764267f, 0.0805f, 0.1659f, 0.9829f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.000000f, 0.975207f, 0.6099f, -0.6664f, 0.4289f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.192247f, -0.192247f, 0.949852f, 0.6099f, -0.6664f, 0.4289f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.154457f, 0.735207f, 0.6099f, -0.6664f, 0.4289f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.000000f, 0.975207f, 0.0186f, 0.0746f, 0.9970f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.194806f, 0.194806f, 0.964270f, 0.0186f, 0.0746f, 0.9970f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.196918f, 0.000000f, 0.978883f, 0.0186f, 0.0746f, 0.9970f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.125747f, 0.570440f, 0.4249f, 0.9038f, 0.0517f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.087252f, 0.178279f, 0.369084f, 0.4249f, 0.9038f, 0.0517f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.194806f, 0.194806f, 0.964270f, 0.4249f, 0.9038f, 0.0517f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.413812f, 0.999413f, -0.8482f, -0.4469f, 0.2844f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.133444f, 0.405324f, 0.588123f, -0.8482f, -0.4469f, 0.2844f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.087252f, 0.178279f, 0.369084f, -0.8482f, -0.4469f, 0.2844f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.163810f, -0.331870f, 0.778574f, -0.1113f, -0.9527f, 0.2827f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.258315f, -0.386675f, 0.556625f, -0.1113f, -0.9527f, 0.2827f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.130535f, -0.396451f, 0.574005f, -0.1113f, -0.9527f, 0.2827f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.192247f, -0.192247f, 0.949852f, 0.6173f, -0.5565f, 0.5561f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.411064f, -0.411064f, 0.973758f, 0.6173f, -0.5565f, 0.5561f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.163810f, -0.331870f, 0.778574f, 0.6173f, -0.5565f, 0.5561f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.196918f, 0.000000f, 0.978883f, -0.0798f, -0.1507f, 0.9853f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.390072f, -0.192820f, 0.933746f, -0.0798f, -0.1507f, 0.9853f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.192247f, -0.192247f, 0.949852f, -0.0798f, -0.1507f, 0.9853f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.196918f, 0.000000f, 0.978883f, -0.6589f, 0.4114f, 0.6298f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.320007f, 0.157896f, 0.746945f, -0.6589f, 0.4114f, 0.6298f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.347708f, 0.000000f, 0.821120f, -0.6589f, 0.4114f, 0.6298f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.194806f, 0.194806f, 0.964270f, -0.3842f, 0.9210f, 0.0649f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.218973f, 0.218973f, 0.478349f, -0.3842f, 0.9210f, 0.0649f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.320007f, 0.157896f, 0.746945f, -0.3842f, 0.9210f, 0.0649f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.087252f, 0.178279f, 0.369084f, 0.5178f, -0.3808f, 0.7661f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.291068f, 0.436997f, 0.635456f, 0.5178f, -0.3808f, 0.7661f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.218973f, 0.218973f, 0.478349f, 0.5178f, -0.3808f, 0.7661f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.411064f, -0.411064f, 0.973758f, 0.4737f, -0.8721f, 0.1225f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.493679f, -0.493679f, 0.704996f, 0.4737f, -0.8721f, 0.1225f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.258315f, -0.386675f, 0.556625f, 0.4737f, -0.8721f, 0.1225f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.390072f, -0.192820f, 0.933746f, -0.4308f, 0.2027f, 0.8794f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.593284f, -0.392791f, 0.880291f, -0.4308f, 0.2027f, 0.8794f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.411064f, -0.411064f, 0.973758f, -0.4308f, 0.2027f, 0.8794f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.347708f, 0.000000f, 0.821120f, 0.1285f, 0.4790f, 0.8684f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.654998f, -0.215302f, 0.985363f, 0.1285f, 0.4790f, 0.8684f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.390072f, -0.192820f, 0.933746f, 0.1285f, 0.4790f, 0.8684f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.347708f, 0.000000f, 0.821120f, 0.3785f, -0.0709f, 0.9229f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.636257f, 0.209158f, 0.955546f, 0.3785f, -0.0709f, 0.9229f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.621680f, -0.000000f, 0.933497f, 0.3785f, -0.0709f, 0.9229f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.320007f, 0.157896f, 0.746945f, 0.3940f, 0.8308f, 0.3931f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.409826f, 0.273383f, 0.592892f, 0.3940f, 0.8308f, 0.3931f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.636257f, 0.209158f, 0.955546f, 0.3940f, 0.8308f, 0.3931f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.218973f, 0.218973f, 0.478349f, 0.3882f, -0.3931f, 0.8335f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.545989f, 0.545989f, 0.784859f, 0.3882f, -0.3931f, 0.8335f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.409826f, 0.273383f, 0.592892f, 0.3882f, -0.3931f, 0.8335f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.349401f, -0.349401f, 0.349401f, 0.9188f, -0.3266f, -0.2219f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.382515f, -0.382515f, 0.535280f, 0.9188f, -0.3266f, -0.2219f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.340510f, -0.427395f, 0.427396f, 0.9188f, -0.3266f, -0.2219f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.384191f, -0.309091f, 0.384191f, 0.9845f, -0.1617f, -0.0676f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.404388f, -0.269844f, 0.584372f, 0.9845f, -0.1617f, -0.0676f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.382515f, -0.382515f, 0.535280f, 0.9845f, -0.1617f, -0.0676f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.657647f, -0.337831f, 0.657647f, -0.0535f, 0.6331f, 0.7722f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313143f, -0.103221f, 0.441460f, -0.0535f, 0.6331f, 0.7722f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.404388f, -0.269844f, 0.584372f, -0.0535f, 0.6331f, 0.7722f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.781988f, -0.198297f, 0.781988f, -0.5621f, -0.5455f, 0.6216f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.445738f, -0.000000f, 0.651939f, -0.5621f, -0.5455f, 0.6216f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313143f, -0.103221f, 0.441460f, -0.5621f, -0.5455f, 0.6216f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.653571f, 0.166680f, 0.653571f, -0.3626f, 0.4442f, 0.8193f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.445738f, -0.000000f, 0.651939f, -0.3626f, 0.4442f, 0.8193f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.815698f, -0.000000f, 0.815698f, -0.3626f, 0.4442f, 0.8193f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.465944f, 0.244641f, 0.465945f, 0.1498f, 0.9571f, 0.2479f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.504643f, 0.166007f, 0.746144f, 0.1498f, 0.9571f, 0.2479f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.653571f, 0.166680f, 0.653571f, 0.1498f, 0.9571f, 0.2479f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.432840f, 0.344470f, 0.432840f, 0.9354f, 0.2178f, -0.2785f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.537106f, 0.356226f, 0.792284f, 0.9354f, 0.2178f, -0.2785f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.465944f, 0.244641f, 0.465945f, 0.9354f, 0.2178f, -0.2785f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.538827f, 0.538827f, 0.538827f, 0.8470f, -0.5205f, 0.1075f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.487306f, 0.487306f, 0.695266f, 0.8470f, -0.5205f, 0.1075f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.432840f, 0.344470f, 0.432840f, 0.8470f, -0.5205f, 0.1075f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.281453f, 0.346188f, 0.346188f, -0.1631f, 0.9444f, -0.2856f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.288454f, 0.432981f, 0.629165f, -0.1631f, 0.9444f, -0.2856f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.487306f, 0.487306f, 0.695266f, -0.1631f, 0.9444f, -0.2856f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.274601f, 0.527575f, 0.527575f, -0.5298f, 0.5835f, 0.6155f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.123834f, 0.376013f, 0.541488f, -0.5298f, 0.5835f, 0.6155f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.288454f, 0.432981f, 0.629165f, -0.5298f, 0.5835f, 0.6155f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.180243f, 0.708659f, 0.708659f, -0.1622f, -0.4210f, 0.8924f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.357065f, 0.510037f, -0.1622f, -0.4210f, 0.8924f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.123834f, 0.376013f, 0.541488f, -0.1622f, -0.4210f, 0.8924f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.357065f, 0.510037f, 0.4779f, 0.8224f, 0.3085f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.116025f, 0.447827f, 0.447827f, 0.4779f, 0.8224f, 0.3085f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.133444f, 0.405324f, 0.588123f, 0.4779f, 0.8224f, 0.3085f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.133444f, 0.405324f, 0.588123f, 0.3331f, 0.3478f, 0.8764f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.300945f, 0.581767f, 0.581767f, 0.3331f, 0.3478f, 0.8764f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.291068f, 0.436997f, 0.635456f, 0.3331f, 0.3478f, 0.8764f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.291068f, 0.436997f, 0.635456f, 0.5699f, 0.6575f, 0.4928f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.475932f, 0.613611f, 0.613611f, 0.5699f, 0.6575f, 0.4928f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.545989f, 0.545989f, 0.784859f, 0.5699f, 0.6575f, 0.4928f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.545989f, 0.545989f, 0.784859f, -0.4458f, 0.8350f, -0.3225f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.465606f, 0.465606f, 0.465606f, -0.4458f, 0.8350f, -0.3225f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.576827f, 0.449182f, 0.576827f, -0.4458f, 0.8350f, -0.3225f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.409826f, 0.273383f, 0.592892f, -0.7160f, -0.6591f, 0.2303f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.576827f, 0.449182f, 0.576827f, -0.7160f, -0.6591f, 0.2303f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.418188f, 0.221426f, 0.418188f, -0.7160f, -0.6591f, 0.2303f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.636257f, 0.209158f, 0.955546f, -0.1088f, 0.9938f, -0.0215f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.418188f, 0.221426f, 0.418188f, -0.1088f, 0.9938f, -0.0215f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.755523f, 0.191781f, 0.755523f, -0.1088f, 0.9938f, -0.0215f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.621680f, -0.000000f, 0.933497f, -0.8687f, -0.4746f, 0.1419f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.755523f, 0.191781f, 0.755523f, -0.8687f, -0.4746f, 0.1419f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.665454f, -0.000000f, 0.665454f, -0.8687f, -0.4746f, 0.1419f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.621680f, -0.000000f, 0.933497f, -0.9874f, 0.1317f, -0.0877f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.614249f, -0.156999f, 0.614249f, -0.9874f, 0.1317f, -0.0877f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.654998f, -0.215302f, 0.985363f, -0.9874f, 0.1317f, -0.0877f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.654998f, -0.215302f, 0.985363f, -0.9331f, -0.1247f, -0.3374f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.462077f, -0.242762f, 0.462077f, -0.9331f, -0.1247f, -0.3374f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.593284f, -0.392791f, 0.880291f, -0.9331f, -0.1247f, -0.3374f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.593284f, -0.392791f, 0.880291f, -0.8889f, -0.3356f, -0.3119f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.456319f, -0.361544f, 0.456319f, -0.8889f, -0.3356f, -0.3119f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.493679f, -0.493679f, 0.704996f, -0.8889f, -0.3356f, -0.3119f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.493679f, -0.493679f, 0.704996f, 0.6296f, -0.7719f, 0.0885f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.574719f, -0.574719f, 0.574719f, 0.6296f, -0.7719f, 0.0885f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.303095f, -0.375947f, 0.375947f, 0.6296f, -0.7719f, 0.0885f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.258315f, -0.386675f, 0.556625f, -0.9622f, 0.1183f, 0.2455f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.303095f, -0.375947f, 0.375947f, -0.9622f, 0.1183f, 0.2455f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.281239f, -0.541230f, 0.541230f, -0.9622f, 0.1183f, 0.2455f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.130535f, -0.396451f, 0.574005f, -0.4521f, 0.2787f, 0.8473f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.281239f, -0.541230f, 0.541230f, -0.4521f, 0.2787f, 0.8473f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.162303f, -0.635792f, 0.635792f, -0.4521f, 0.2787f, 0.8473f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.551223f, 0.820744f, -0.0150f, -0.9043f, 0.4266f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.162303f, -0.635792f, 0.635792f, -0.0150f, -0.9043f, 0.4266f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.637618f, 0.637618f, -0.0150f, -0.9043f, 0.4266f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.134213f, -0.521701f, 0.521700f, 0.7545f, -0.5936f, 0.2800f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.551223f, 0.820744f, 0.7545f, -0.5936f, 0.2800f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.637618f, 0.637618f, 0.7545f, -0.5936f, 0.2800f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.244147f, -0.464927f, 0.464927f, 0.5046f, -0.8546f, 0.1226f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.158186f, -0.480788f, 0.708190f, 0.5046f, -0.8546f, 0.1226f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.134213f, -0.521701f, 0.521700f, 0.5046f, -0.8546f, 0.1226f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.340510f, -0.427395f, 0.427396f, 0.4804f, -0.6826f, 0.5508f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.256950f, -0.384578f, 0.553339f, 0.4804f, -0.6826f, 0.5508f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.244147f, -0.464927f, 0.464927f, 0.4804f, -0.6826f, 0.5508f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.780100f, -0.542872f, 0.542872f, -0.7448f, 0.5887f, 0.3143f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.691180f, -0.301497f, 0.301497f, -0.7448f, 0.5887f, 0.3143f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.884736f, -0.596122f, 0.394638f, -0.7448f, 0.5887f, 0.3143f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.598564f, -0.275740f, 0.413447f, -0.6951f, -0.3131f, 0.6471f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.741145f, -0.156812f, 0.317831f, -0.6951f, -0.3131f, 0.6471f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.691180f, -0.301497f, 0.301497f, -0.6951f, -0.3131f, 0.6471f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.598545f, -0.135591f, 0.411875f, -0.5479f, 0.3805f, 0.7450f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.668534f, -0.000000f, 0.291135f, -0.5479f, 0.3805f, 0.7450f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.741145f, -0.156812f, 0.317831f, -0.5479f, 0.3805f, 0.7450f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -1.010385f, 0.220458f, 0.670724f, -0.7881f, -0.3484f, -0.5074f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.668534f, -0.000000f, 0.291135f, -0.7881f, -0.3484f, -0.5074f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.846255f, 0.000000f, 0.567164f, -0.7881f, -0.3484f, -0.5074f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.654709f, 0.299067f, 0.449287f, -0.4149f, 0.8305f, -0.3717f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.818719f, 0.171315f, 0.346927f, -0.4149f, 0.8305f, -0.3717f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.010385f, 0.220458f, 0.670724f, -0.4149f, 0.8305f, -0.3717f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.467288f, 0.337980f, 0.337981f, 0.1336f, 0.8437f, 0.5199f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.892758f, 0.379657f, 0.379657f, 0.1336f, 0.8437f, 0.5199f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.654709f, 0.299067f, 0.449287f, 0.1336f, 0.8437f, 0.5199f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.884736f, -0.596122f, 0.394638f, -0.8502f, 0.2453f, -0.4658f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.656247f, -0.285988f, 0.140940f, -0.8502f, 0.2453f, -0.4658f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.727620f, -0.493001f, 0.162190f, -0.8502f, 0.2453f, -0.4658f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.691180f, -0.301497f, 0.301497f, -0.9297f, -0.2876f, -0.2301f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.700677f, -0.148029f, 0.148029f, -0.9297f, -0.2876f, -0.2301f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.656247f, -0.285988f, 0.140940f, -0.9297f, -0.2876f, -0.2301f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.741145f, -0.156812f, 0.317831f, -0.6343f, -0.7494f, -0.1899f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.885302f, -0.000000f, 0.180611f, -0.6343f, -0.7494f, -0.1899f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.700677f, -0.148029f, 0.148029f, -0.6343f, -0.7494f, -0.1899f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.818719f, 0.171315f, 0.346927f, -0.3742f, -0.5670f, 0.7338f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.885302f, -0.000000f, 0.180611f, -0.3742f, -0.5670f, 0.7338f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.668534f, -0.000000f, 0.291135f, -0.3742f, -0.5670f, 0.7338f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.892758f, 0.379657f, 0.379657f, -0.7281f, -0.3512f, 0.5887f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.953661f, 0.192923f, 0.192923f, -0.7281f, -0.3512f, 0.5887f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.818719f, 0.171315f, 0.346927f, -0.7281f, -0.3512f, 0.5887f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.738815f, 0.502975f, 0.334011f, -0.5020f, 0.3309f, -0.7991f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.570152f, 0.253695f, 0.124843f, -0.5020f, 0.3309f, -0.7991f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.892758f, 0.379657f, 0.379657f, -0.5020f, 0.3309f, -0.7991f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.727620f, -0.493001f, 0.162190f, -0.5335f, 0.0200f, 0.8456f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.981479f, -0.407163f, -0.000000f, -0.5335f, 0.0200f, 0.8456f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.990879f, -0.657537f, -0.000000f, -0.5335f, 0.0200f, 0.8456f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.656247f, -0.285988f, 0.140940f, -0.4883f, 0.4222f, 0.7637f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.767887f, -0.160152f, -0.000000f, -0.4883f, 0.4222f, 0.7637f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.981479f, -0.407163f, -0.000000f, -0.4883f, 0.4222f, 0.7637f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.700677f, -0.148029f, 0.148029f, -0.5388f, -0.7838f, 0.3088f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.000884f, -0.000000f, 0.000000f, -0.5388f, -0.7838f, 0.3088f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.767887f, -0.160152f, -0.000000f, -0.5388f, -0.7838f, 0.3088f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.953661f, 0.192923f, 0.192923f, -0.7992f, -0.3158f, 0.5114f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.000884f, -0.000000f, 0.000000f, -0.7992f, -0.3158f, 0.5114f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.885302f, -0.000000f, 0.180611f, -0.7992f, -0.3158f, 0.5114f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.570152f, 0.253695f, 0.124843f, -0.1984f, 0.9396f, -0.2790f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.806074f, 0.166806f, -0.000000f, -0.1984f, 0.9396f, -0.2790f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.953661f, 0.192923f, 0.192923f, -0.1984f, 0.9396f, -0.2790f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.873176f, 0.584486f, 0.192184f, -0.3896f, -0.5126f, 0.7651f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.051441f, 0.433102f, -0.000000f, -0.3896f, -0.5126f, 0.7651f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.570152f, 0.253695f, 0.124843f, -0.3896f, -0.5126f, 0.7651f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.981479f, -0.407163f, -0.000000f, -0.3117f, 0.0117f, -0.9501f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.579688f, -0.400023f, -0.131706f, -0.3117f, 0.0117f, -0.9501f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.990879f, -0.657537f, -0.000000f, -0.3117f, 0.0117f, -0.9501f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.767887f, -0.160152f, -0.000000f, -0.4397f, 0.3802f, -0.8137f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.616712f, -0.271159f, -0.133548f, -0.4397f, 0.3802f, -0.8137f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.981479f, -0.407163f, -0.000000f, -0.4397f, 0.3802f, -0.8137f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -1.000884f, -0.000000f, 0.000000f, -0.5664f, -0.8240f, -0.0164f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.764446f, -0.159345f, -0.159345f, -0.5664f, -0.8240f, -0.0164f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.767887f, -0.160152f, -0.000000f, -0.5664f, -0.8240f, -0.0164f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -1.000884f, -0.000000f, 0.000000f, -0.5058f, 0.8277f, -0.2430f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.691145f, 0.146337f, -0.146337f, -0.5058f, 0.8277f, -0.2430f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.911905f, 0.000000f, -0.185247f, -0.5058f, 0.8277f, -0.2430f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.806074f, 0.166806f, -0.000000f, -0.5282f, 0.6791f, -0.5098f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.572381f, 0.254531f, -0.125260f, -0.5282f, 0.6791f, -0.5098f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.691145f, 0.146337f, -0.146337f, -0.5282f, 0.6791f, -0.5098f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -1.051441f, 0.433102f, -0.000000f, -0.3310f, -0.2498f, -0.9100f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.657275f, 0.448787f, -0.147694f, -0.3310f, -0.2498f, -0.9100f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.572381f, 0.254531f, -0.125260f, -0.3310f, -0.2498f, -0.9100f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.616712f, -0.271159f, -0.133548f, -0.7661f, -0.2114f, 0.6069f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.706616f, -0.482421f, -0.320633f, -0.7661f, -0.2114f, 0.6069f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.579688f, -0.400023f, -0.131706f, -0.7661f, -0.2114f, 0.6069f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.764446f, -0.159345f, -0.159345f, -0.5184f, -0.5301f, 0.6711f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.816309f, -0.350015f, -0.350014f, -0.5184f, -0.5301f, 0.6711f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.616712f, -0.271159f, -0.133548f, -0.5184f, -0.5301f, 0.6711f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.911905f, 0.000000f, -0.185247f, -0.6896f, -0.5644f, 0.4537f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.884195f, -0.183556f, -0.371486f, -0.6896f, -0.5644f, 0.4537f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.764446f, -0.159345f, -0.159345f, -0.6896f, -0.5644f, 0.4537f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.911905f, 0.000000f, -0.185247f, -0.2647f, -0.7027f, -0.6604f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.919143f, 0.190090f, -0.384594f, -0.2647f, -0.7027f, -0.6604f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.657742f, -0.000000f, -0.287134f, -0.2647f, -0.7027f, -0.6604f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.691145f, 0.146337f, -0.146337f, -0.5960f, 0.4637f, 0.6555f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.748195f, 0.323604f, -0.323604f, -0.5960f, 0.4637f, 0.6555f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.919143f, 0.190090f, -0.384594f, -0.5960f, 0.4637f, 0.6555f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.572381f, 0.254531f, -0.125260f, -0.5260f, 0.5423f, 0.6551f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.596287f, 0.411994f, -0.274794f, -0.5260f, 0.5423f, 0.6551f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.748195f, 0.323604f, -0.323604f, -0.5260f, 0.5423f, 0.6551f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.816309f, -0.350015f, -0.350014f, -0.2029f, -0.3693f, -0.9069f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.550241f, -0.392314f, -0.392315f, -0.2029f, -0.3693f, -0.9069f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.706616f, -0.482421f, -0.320633f, -0.2029f, -0.3693f, -0.9069f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.884195f, -0.183556f, -0.371486f, -0.3897f, -0.2724f, -0.8797f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.644876f, -0.294982f, -0.443010f, -0.3897f, -0.2724f, -0.8797f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.816309f, -0.350015f, -0.350014f, -0.3897f, -0.2724f, -0.8797f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.657742f, -0.000000f, -0.287134f, -0.6341f, 0.7730f, 0.0202f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.912052f, -0.200195f, -0.608920f, -0.6341f, 0.7730f, 0.0202f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.884195f, -0.183556f, -0.371486f, -0.6341f, 0.7730f, 0.0202f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.657742f, -0.000000f, -0.287134f, -0.7846f, -0.3964f, 0.4768f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.994485f, 0.217182f, -0.660731f, -0.7846f, -0.3964f, 0.4768f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.816676f, 0.000000f, -0.548680f, -0.7846f, -0.3964f, 0.4768f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.919143f, 0.190090f, -0.384594f, -0.2580f, 0.9522f, 0.1638f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.609280f, 0.280192f, -0.420287f, -0.2580f, 0.9522f, 0.1638f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.994485f, 0.217182f, -0.660731f, -0.2580f, 0.9522f, 0.1638f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.748195f, 0.323604f, -0.323604f, -0.4525f, 0.3645f, -0.8139f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.557279f, 0.396925f, -0.396925f, -0.4525f, 0.3645f, -0.8139f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.609280f, 0.280192f, -0.420287f, -0.4525f, 0.3645f, -0.8139f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.574719f, -0.574719f, 0.574719f, -0.1460f, -0.9882f, -0.0467f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.780100f, -0.542872f, 0.542872f, -0.1460f, -0.9882f, -0.0467f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.569328f, -0.569328f, 0.443728f, -0.1460f, -0.9882f, -0.0467f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.456319f, -0.361544f, 0.456319f, -0.0058f, 0.4392f, 0.8984f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.598564f, -0.275740f, 0.413447f, -0.0058f, 0.4392f, 0.8984f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.780100f, -0.542872f, 0.542872f, -0.0058f, 0.4392f, 0.8984f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.462077f, -0.242762f, 0.462077f, -0.3379f, 0.0106f, 0.9411f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.598545f, -0.135591f, 0.411875f, -0.3379f, 0.0106f, 0.9411f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.598564f, -0.275740f, 0.413447f, -0.3379f, 0.0106f, 0.9411f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.614249f, -0.156999f, 0.614249f, -0.5375f, -0.8332f, -0.1298f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.846255f, 0.000000f, 0.567164f, -0.5375f, -0.8332f, -0.1298f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.598545f, -0.135591f, 0.411875f, -0.5375f, -0.8332f, -0.1298f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.755523f, 0.191781f, 0.755523f, -0.4028f, -0.5372f, 0.7410f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.846255f, 0.000000f, 0.567164f, -0.4028f, -0.5372f, 0.7410f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.665454f, -0.000000f, 0.665454f, -0.4028f, -0.5372f, 0.7410f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.418188f, 0.221426f, 0.418188f, 0.0617f, 0.9870f, 0.1484f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.010385f, 0.220458f, 0.670724f, 0.0617f, 0.9870f, 0.1484f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.755523f, 0.191781f, 0.755523f, 0.0617f, 0.9870f, 0.1484f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.576827f, 0.449182f, 0.576827f, -0.0986f, -0.6141f, 0.7830f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.654709f, 0.299067f, 0.449287f, -0.0986f, -0.6141f, 0.7830f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.418188f, 0.221426f, 0.418188f, -0.0986f, -0.6141f, 0.7830f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.465606f, 0.465606f, 0.465606f, -0.6280f, 0.5544f, -0.5461f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467288f, 0.337980f, 0.337981f, -0.6280f, 0.5544f, -0.5461f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.576827f, 0.449182f, 0.576827f, -0.6280f, 0.5544f, -0.5461f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.467203f, 0.467203f, 0.369460f, -0.1560f, -0.2337f, 0.9597f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.738815f, 0.502975f, 0.334011f, -0.1560f, -0.2337f, 0.9597f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467288f, 0.337980f, 0.337981f, -0.1560f, -0.2337f, 0.9597f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.482247f, 0.482247f, 0.252566f, 0.1857f, 0.9176f, 0.3514f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.873176f, 0.584486f, 0.192184f, 0.1857f, 0.9176f, 0.3514f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.738815f, 0.502975f, 0.334011f, 0.1857f, 0.9176f, 0.3514f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.606308f, 0.606308f, 0.155044f, -0.1593f, 0.5126f, -0.8437f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.535651f, 0.373071f, -0.000000f, -0.1593f, 0.5126f, -0.8437f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.873176f, 0.584486f, 0.192184f, -0.1593f, 0.5126f, -0.8437f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.535651f, 0.373071f, -0.000000f, -0.8022f, -0.1756f, 0.5706f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.755952f, 0.755952f, -0.191887f, -0.8022f, -0.1756f, 0.5706f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.657275f, 0.448787f, -0.147694f, -0.8022f, -0.1756f, 0.5706f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.657275f, 0.448787f, -0.147694f, -0.9081f, -0.1034f, -0.4058f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.601862f, 0.601862f, -0.310713f, -0.9081f, -0.1034f, -0.4058f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.596287f, 0.411994f, -0.274794f, -0.9081f, -0.1034f, -0.4058f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.596287f, 0.411994f, -0.274794f, -0.6410f, 0.7097f, -0.2923f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.477370f, 0.477370f, -0.376854f, -0.6410f, 0.7097f, -0.2923f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.557279f, 0.396925f, -0.396925f, -0.6410f, 0.7097f, -0.2923f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.557279f, 0.396925f, -0.396925f, 0.3887f, 0.9208f, 0.0333f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.271962f, 0.271962f, -0.271962f, 0.3887f, 0.9208f, 0.0333f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.437378f, 0.347770f, -0.437378f, 0.3887f, 0.9208f, 0.0333f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.609280f, 0.280192f, -0.420287f, -0.3203f, 0.8940f, 0.3133f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.437378f, 0.347770f, -0.437378f, -0.3203f, 0.8940f, 0.3133f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.674306f, 0.345929f, -0.674306f, -0.3203f, 0.8940f, 0.3133f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.994485f, 0.217182f, -0.660731f, -0.0595f, 0.0429f, -0.9973f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.674306f, 0.345929f, -0.674306f, -0.0595f, 0.0429f, -0.9973f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.681304f, 0.173508f, -0.681304f, -0.0595f, 0.0429f, -0.9973f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.816676f, 0.000000f, -0.548680f, 0.2384f, -0.7003f, -0.6729f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.681304f, 0.173508f, -0.681304f, 0.2384f, -0.7003f, -0.6729f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.401631f, 0.000000f, -0.401631f, 0.2384f, -0.7003f, -0.6729f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.816676f, 0.000000f, -0.548680f, 0.0397f, 0.2705f, -0.9619f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.580775f, -0.148757f, -0.580775f, 0.0397f, 0.2705f, -0.9619f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.912052f, -0.200195f, -0.608920f, 0.0397f, 0.2705f, -0.9619f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.912052f, -0.200195f, -0.608920f, 0.1255f, -0.7608f, -0.6367f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467353f, -0.245326f, -0.467352f, 0.1255f, -0.7608f, -0.6367f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.644876f, -0.294982f, -0.443010f, 0.1255f, -0.7608f, -0.6367f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.644876f, -0.294982f, -0.443010f, -0.7527f, -0.5245f, 0.3981f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.608913f, -0.472516f, -0.608913f, -0.7527f, -0.5245f, 0.3981f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.550241f, -0.392314f, -0.392315f, -0.7527f, -0.5245f, 0.3981f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.550241f, -0.392314f, -0.392315f, -0.1451f, -0.8128f, 0.5641f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.450496f, -0.450496f, -0.450496f, -0.1451f, -0.8128f, 0.5641f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.354508f, -0.354508f, -0.287505f, -0.1451f, -0.8128f, 0.5641f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.706616f, -0.482421f, -0.320633f, 0.0298f, 0.1730f, -0.9845f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.354508f, -0.354508f, -0.287505f, 0.0298f, 0.1730f, -0.9845f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.700848f, -0.700848f, -0.358831f, 0.0298f, 0.1730f, -0.9845f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.579688f, -0.400023f, -0.131706f, -0.8111f, 0.5236f, -0.2608f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.700848f, -0.700848f, -0.358831f, -0.8111f, 0.5236f, -0.2608f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.834707f, -0.834707f, -0.211277f, -0.8111f, 0.5236f, -0.2608f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.990879f, -0.657537f, -0.000000f, 0.1524f, -0.6989f, 0.6988f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.834707f, -0.834707f, -0.211277f, 0.1524f, -0.6989f, 0.6988f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.564597f, -0.564597f, -0.000000f, 0.1524f, -0.6989f, 0.6988f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.455396f, -0.455396f, 0.117888f, 0.1739f, -0.7975f, 0.5777f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.990879f, -0.657537f, -0.000000f, 0.1739f, -0.7975f, 0.5777f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.564597f, -0.564597f, -0.000000f, 0.1739f, -0.7975f, 0.5777f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.542685f, -0.542685f, 0.281946f, 0.0504f, -0.8926f, -0.4481f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.727620f, -0.493001f, 0.162190f, 0.0504f, -0.8926f, -0.4481f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.455396f, -0.455396f, 0.117888f, 0.0504f, -0.8926f, -0.4481f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.569328f, -0.569328f, 0.443728f, 0.1061f, -0.9838f, -0.1445f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.884736f, -0.596122f, 0.394638f, 0.1061f, -0.9838f, -0.1445f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.542685f, -0.542685f, 0.281946f, 0.1061f, -0.9838f, -0.1445f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.547855f, -0.787708f, -0.547855f, 0.6559f, -0.6039f, -0.4529f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.307622f, -0.706978f, -0.307622f, 0.6559f, -0.6039f, -0.4529f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.668695f, -0.998427f, -0.441874f, 0.6559f, -0.6039f, -0.4529f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.398933f, -0.895074f, -0.602721f, -0.4258f, -0.6975f, 0.5763f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.172235f, -0.823641f, -0.348774f, -0.4258f, -0.6975f, 0.5763f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.307622f, -0.706978f, -0.307622f, -0.4258f, -0.6975f, 0.5763f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.169974f, -0.765397f, -0.516744f, 0.1075f, -0.9398f, -0.3244f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.806215f, -0.342182f, 0.1075f, -0.9398f, -0.3244f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.172235f, -0.823641f, -0.348774f, 0.1075f, -0.9398f, -0.3244f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.102127f, -0.436152f, -0.309807f, 0.9619f, -0.2574f, -0.0918f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.806215f, -0.342182f, 0.9619f, -0.2574f, -0.0918f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.747885f, -0.505694f, 0.9619f, -0.2574f, -0.0918f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.347593f, -0.771503f, -0.523841f, -0.8185f, -0.2784f, -0.5025f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.130487f, -0.600341f, -0.265019f, -0.8185f, -0.2784f, -0.5025f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.102127f, -0.436152f, -0.309807f, -0.8185f, -0.2784f, -0.5025f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.410149f, -0.577469f, -0.410149f, 0.9266f, -0.3609f, 0.1060f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.328976f, -0.762050f, -0.328976f, 0.9266f, -0.3609f, 0.1060f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.347593f, -0.771503f, -0.523841f, 0.9266f, -0.3609f, 0.1060f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.668695f, -0.998427f, -0.441874f, -0.3602f, -0.4392f, 0.8230f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.322062f, -0.752426f, -0.158921f, -0.3602f, -0.4392f, 0.8230f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.432186f, -0.630861f, -0.142251f, -0.3602f, -0.4392f, 0.8230f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.307622f, -0.706978f, -0.307622f, -0.8894f, -0.4059f, -0.2104f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.200605f, -0.996950f, -0.200605f, -0.8894f, -0.4059f, -0.2104f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.322062f, -0.752426f, -0.158921f, -0.8894f, -0.4059f, -0.2104f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.172235f, -0.823641f, -0.348774f, -0.2250f, -0.6116f, -0.7585f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -1.058182f, -0.210736f, -0.2250f, -0.6116f, -0.7585f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.200605f, -0.996950f, -0.200605f, -0.2250f, -0.6116f, -0.7585f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.130487f, -0.600341f, -0.265019f, 0.7818f, -0.2884f, -0.5528f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -1.058182f, -0.210736f, 0.7818f, -0.2884f, -0.5528f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.806215f, -0.342182f, 0.7818f, -0.2884f, -0.5528f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.328976f, -0.762050f, -0.328976f, -0.6140f, -0.5180f, -0.5956f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.157842f, -0.755973f, -0.157842f, -0.6140f, -0.5180f, -0.5956f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.130487f, -0.600341f, -0.265019f, -0.6140f, -0.5180f, -0.5956f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.400765f, -0.578697f, -0.267486f, 0.9003f, -0.4055f, 0.1580f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313266f, -0.728973f, -0.154536f, 0.9003f, -0.4055f, 0.1580f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.328976f, -0.762050f, -0.328976f, 0.9003f, -0.4055f, 0.1580f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.432186f, -0.630861f, -0.142251f, -0.9311f, -0.2146f, -0.2949f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.400537f, -0.963609f, 0.000000f, -0.9311f, -0.2146f, -0.2949f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.465203f, -0.683089f, 0.000000f, -0.9311f, -0.2146f, -0.2949f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.322062f, -0.752426f, -0.158921f, 0.6067f, -0.6095f, -0.5103f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.150966f, -0.715172f, 0.000000f, 0.6067f, -0.6095f, -0.5103f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.400537f, -0.963609f, 0.000000f, 0.6067f, -0.6095f, -0.5103f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.200605f, -0.996950f, -0.200605f, -0.4851f, -0.4488f, 0.7505f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.878321f, -0.000000f, -0.4851f, -0.4488f, 0.7505f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.150966f, -0.715172f, 0.000000f, -0.4851f, -0.4488f, 0.7505f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.157842f, -0.755973f, -0.157842f, 0.7781f, -0.4778f, 0.4078f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.878321f, -0.000000f, 0.7781f, -0.4778f, 0.4078f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -1.058182f, -0.210736f, 0.7781f, -0.4778f, 0.4078f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.313266f, -0.728973f, -0.154536f, 0.1094f, -0.5265f, -0.8431f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.200575f, -0.999869f, -0.000000f, 0.1094f, -0.5265f, -0.8431f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.157842f, -0.755973f, -0.157842f, 0.1094f, -0.5265f, -0.8431f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.267683f, -0.369131f, -0.088317f, 0.9908f, 0.1313f, -0.0314f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.320722f, -0.748334f, -0.000000f, 0.9908f, 0.1313f, -0.0314f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313266f, -0.728973f, -0.154536f, 0.9908f, 0.1313f, -0.0314f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.400537f, -0.963609f, 0.000000f, -0.8827f, -0.2035f, 0.4237f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.416949f, -0.606618f, 0.137255f, -0.8827f, -0.2035f, 0.4237f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.465203f, -0.683089f, 0.000000f, -0.8827f, -0.2035f, 0.4237f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.150966f, -0.715172f, 0.000000f, 0.4955f, -0.4977f, 0.7119f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.282534f, -0.647040f, 0.139218f, 0.4955f, -0.4977f, 0.7119f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.400537f, -0.963609f, 0.000000f, 0.4955f, -0.4977f, 0.7119f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.878321f, -0.000000f, -0.5998f, -0.5550f, -0.5763f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.178216f, -0.870786f, 0.178216f, -0.5998f, -0.5550f, -0.5763f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.150966f, -0.715172f, 0.000000f, -0.5998f, -0.5550f, -0.5763f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.878321f, -0.000000f, 0.7478f, -0.6529f, 0.1203f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.145275f, -0.685160f, 0.145275f, 0.7478f, -0.6529f, 0.1203f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.846304f, 0.173816f, 0.7478f, -0.6529f, 0.1203f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.200575f, -0.999869f, -0.000000f, -0.5037f, -0.4335f, 0.7473f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.370563f, -0.881734f, 0.183096f, -0.5037f, -0.4335f, 0.7473f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.145275f, -0.685160f, 0.145275f, -0.5037f, -0.4335f, 0.7473f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.320722f, -0.748334f, -0.000000f, 0.1997f, -0.7653f, -0.6119f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.559328f, -0.833150f, 0.183936f, 0.1997f, -0.7653f, -0.6119f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.370563f, -0.881734f, 0.183096f, 0.1997f, -0.7653f, -0.6119f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.282534f, -0.647040f, 0.139218f, -0.1895f, -0.6653f, -0.7222f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.552658f, -0.816648f, 0.366349f, -0.1895f, -0.6653f, -0.7222f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.416949f, -0.606618f, 0.137255f, -0.1895f, -0.6653f, -0.7222f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.178216f, -0.870786f, 0.178216f, -0.6786f, -0.4213f, -0.6017f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.358383f, -0.837893f, 0.358383f, -0.6786f, -0.4213f, -0.6017f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.282534f, -0.647040f, 0.139218f, -0.6786f, -0.4213f, -0.6017f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.846304f, 0.173816f, 0.1382f, -0.9744f, 0.1775f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.174969f, -0.838261f, 0.354257f, 0.1382f, -0.9744f, 0.1775f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.178216f, -0.870786f, 0.178216f, 0.1382f, -0.9744f, 0.1775f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.846304f, 0.173816f, 0.7772f, -0.6215f, -0.0989f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.145980f, -0.683205f, 0.296099f, 0.7772f, -0.6215f, -0.0989f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.877283f, 0.368531f, 0.7772f, -0.6215f, -0.0989f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.145275f, -0.685160f, 0.145275f, -0.5533f, -0.8328f, 0.0134f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.350838f, -0.818433f, 0.350838f, -0.5533f, -0.8328f, 0.0134f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.145980f, -0.683205f, 0.296099f, -0.5533f, -0.8328f, 0.0134f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.370563f, -0.881734f, 0.183096f, 0.9525f, -0.2304f, 0.1990f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.417581f, -0.605040f, 0.278431f, 0.9525f, -0.2304f, 0.1990f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.350838f, -0.818433f, 0.350838f, 0.9525f, -0.2304f, 0.1990f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.358383f, -0.837893f, 0.358383f, -0.0930f, -0.9556f, 0.2797f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.535477f, -0.768810f, 0.535477f, -0.0930f, -0.9556f, 0.2797f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.552658f, -0.816648f, 0.366349f, -0.0930f, -0.9556f, 0.2797f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.174969f, -0.838261f, 0.354257f, 0.0022f, -0.9824f, 0.1870f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.360523f, -0.802626f, 0.543708f, 0.0022f, -0.9824f, 0.1870f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.358383f, -0.837893f, 0.358383f, 0.0022f, -0.9824f, 0.1870f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.877283f, 0.368531f, -0.1991f, -0.9617f, -0.1887f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.193682f, -0.880448f, 0.589056f, -0.1991f, -0.9617f, -0.1887f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.174969f, -0.838261f, 0.354257f, -0.1991f, -0.9617f, -0.1887f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.877283f, 0.368531f, 0.4479f, -0.8687f, -0.2114f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.183011f, -0.828663f, 0.556508f, 0.4479f, -0.8687f, -0.2114f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.939839f, 0.625643f, 0.4479f, -0.8687f, -0.2114f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.145980f, -0.683205f, 0.296099f, 0.2847f, -0.8191f, -0.4980f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.336655f, -0.745179f, 0.507037f, 0.2847f, -0.8191f, -0.4980f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.183011f, -0.828663f, 0.556508f, 0.2847f, -0.8191f, -0.4980f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.350838f, -0.818433f, 0.350838f, 0.9964f, 0.0154f, 0.0833f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.346010f, -0.479546f, 0.346010f, 0.9964f, 0.0154f, 0.0833f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.336655f, -0.745179f, 0.507037f, 0.9964f, 0.0154f, 0.0833f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.450496f, -0.450496f, -0.450496f, -0.8856f, 0.1266f, 0.4469f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.547855f, -0.787708f, -0.547855f, -0.8856f, 0.1266f, 0.4469f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.354508f, -0.354508f, -0.287505f, -0.8856f, 0.1266f, 0.4469f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.449732f, -0.577583f, -0.577583f, -0.3319f, 0.0216f, -0.9431f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.398933f, -0.895074f, -0.602721f, -0.3319f, 0.0216f, -0.9431f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.547855f, -0.787708f, -0.547855f, -0.3319f, 0.0216f, -0.9431f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.245217f, -0.467128f, -0.467128f, 0.2333f, 0.2166f, -0.9480f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.169974f, -0.765397f, -0.516744f, 0.2333f, 0.2166f, -0.9480f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.398933f, -0.895074f, -0.602721f, 0.2333f, 0.2166f, -0.9480f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.148160f, -0.578352f, -0.578352f, 0.0944f, -0.3214f, -0.9422f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.747885f, -0.505694f, 0.0944f, -0.3214f, -0.9422f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.169974f, -0.765397f, -0.516744f, 0.0944f, -0.3214f, -0.9422f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.174764f, -0.686407f, -0.686407f, -0.5042f, -0.5423f, -0.6721f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.747885f, -0.505694f, -0.5042f, -0.5423f, -0.6721f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.613845f, -0.613845f, -0.5042f, -0.5423f, -0.6721f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.329947f, -0.641428f, -0.641428f, 0.0759f, -0.8237f, 0.5620f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.102127f, -0.436152f, -0.309807f, 0.0759f, -0.8237f, 0.5620f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.174764f, -0.686407f, -0.686407f, 0.0759f, -0.8237f, 0.5620f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.362015f, -0.456966f, -0.456966f, 0.9870f, -0.0111f, -0.1604f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.347593f, -0.771503f, -0.523841f, 0.9870f, -0.0111f, -0.1604f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.329947f, -0.641428f, -0.641428f, 0.9870f, -0.0111f, -0.1604f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.472812f, -0.472812f, -0.472812f, -0.1871f, -0.4198f, -0.8881f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.410149f, -0.577469f, -0.410149f, -0.1871f, -0.4198f, -0.8881f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.362015f, -0.456966f, -0.456966f, -0.1871f, -0.4198f, -0.8881f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.591253f, -0.591253f, -0.459673f, -0.0796f, -0.9967f, -0.0138f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.400765f, -0.578697f, -0.267486f, -0.0796f, -0.9967f, -0.0138f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.410149f, -0.577469f, -0.410149f, -0.0796f, -0.9967f, -0.0138f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.499946f, -0.499946f, -0.261170f, 0.3315f, -0.4826f, 0.8107f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.267683f, -0.369131f, -0.088317f, 0.3315f, -0.4826f, 0.8107f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.400765f, -0.578697f, -0.267486f, 0.3315f, -0.4826f, 0.8107f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.623072f, -0.623072f, -0.159171f, -0.4781f, -0.4604f, -0.7480f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.526876f, -0.781782f, 0.000000f, -0.4781f, -0.4604f, -0.7480f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.267683f, -0.369131f, -0.088317f, -0.4781f, -0.4604f, -0.7480f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.526876f, -0.781782f, 0.000000f, 0.7560f, -0.5836f, -0.2964f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.676028f, -0.676028f, 0.172209f, 0.7560f, -0.5836f, -0.2964f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.559328f, -0.833150f, 0.183936f, 0.7560f, -0.5836f, -0.2964f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.559328f, -0.833150f, 0.183936f, -0.0522f, -0.4097f, 0.9107f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.578778f, -0.578778f, 0.299491f, -0.0522f, -0.4097f, 0.9107f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.417581f, -0.605040f, 0.278431f, -0.0522f, -0.4097f, 0.9107f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.417581f, -0.605040f, 0.278431f, 0.2901f, -0.3202f, 0.9018f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.425849f, -0.425849f, 0.339386f, 0.2901f, -0.3202f, 0.9018f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.346010f, -0.479546f, 0.346010f, 0.2901f, -0.3202f, 0.9018f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.346010f, -0.479546f, 0.346010f, 0.9960f, -0.0282f, 0.0853f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.349401f, -0.349401f, 0.349401f, 0.9960f, -0.0282f, 0.0853f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.340510f, -0.427395f, 0.427396f, 0.9960f, -0.0282f, 0.0853f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.336655f, -0.745179f, 0.507037f, 0.2738f, 0.2307f, 0.9337f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.340510f, -0.427395f, 0.427396f, 0.2738f, 0.2307f, 0.9337f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.244147f, -0.464927f, 0.464927f, 0.2738f, 0.2307f, 0.9337f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.183011f, -0.828663f, 0.556508f, 0.3844f, 0.1641f, 0.9085f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.244147f, -0.464927f, 0.464927f, 0.3844f, 0.1641f, 0.9085f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.134213f, -0.521701f, 0.521700f, 0.3844f, 0.1641f, 0.9085f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.939839f, 0.625643f, 0.6678f, -0.0295f, 0.7437f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.134213f, -0.521701f, 0.521700f, 0.6678f, -0.0295f, 0.7437f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.637618f, 0.637618f, 0.6678f, -0.0295f, 0.7437f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.939839f, 0.625643f, -0.2289f, -0.1543f, 0.9612f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.162303f, -0.635792f, 0.635792f, -0.2289f, -0.1543f, 0.9612f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.193682f, -0.880448f, 0.589056f, -0.2289f, -0.1543f, 0.9612f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.193682f, -0.880448f, 0.589056f, -0.2274f, 0.0782f, 0.9707f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.281239f, -0.541230f, 0.541230f, -0.2274f, 0.0782f, 0.9707f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.360523f, -0.802626f, 0.543708f, -0.2274f, 0.0782f, 0.9707f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.360523f, -0.802626f, 0.543708f, 0.0263f, 0.3627f, 0.9315f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.303095f, -0.375947f, 0.375947f, 0.0263f, 0.3627f, 0.9315f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.535477f, -0.768810f, 0.535477f, 0.0263f, 0.3627f, 0.9315f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.535477f, -0.768810f, 0.535477f, -0.9809f, -0.1886f, -0.0481f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.574719f, -0.574719f, 0.574719f, -0.9809f, -0.1886f, -0.0481f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.569328f, -0.569328f, 0.443728f, -0.9809f, -0.1886f, -0.0481f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.552658f, -0.816648f, 0.366349f, -0.9862f, -0.0149f, -0.1649f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.569328f, -0.569328f, 0.443728f, -0.9862f, -0.0149f, -0.1649f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.542685f, -0.542685f, 0.281946f, -0.9862f, -0.0149f, -0.1649f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.416949f, -0.606618f, 0.137255f, -0.7827f, -0.2708f, -0.5605f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.542685f, -0.542685f, 0.281946f, -0.7827f, -0.2708f, -0.5605f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.455396f, -0.455396f, 0.117888f, -0.7827f, -0.2708f, -0.5605f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.465203f, -0.683089f, 0.000000f, -0.4660f, -0.3909f, 0.7938f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.455396f, -0.455396f, 0.117888f, -0.4660f, -0.3909f, 0.7938f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.564597f, -0.564597f, -0.000000f, -0.4660f, -0.3909f, 0.7938f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.834707f, -0.834707f, -0.211277f, -0.3719f, -0.3120f, 0.8743f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.465203f, -0.683089f, 0.000000f, -0.3719f, -0.3120f, 0.8743f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.564597f, -0.564597f, -0.000000f, -0.3719f, -0.3120f, 0.8743f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.700848f, -0.700848f, -0.358831f, 0.4712f, -0.8225f, -0.3187f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.432186f, -0.630861f, -0.142251f, 0.4712f, -0.8225f, -0.3187f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.834707f, -0.834707f, -0.211277f, 0.4712f, -0.8225f, -0.3187f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.354508f, -0.354508f, -0.287505f, -0.0635f, 0.2619f, -0.9630f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.668695f, -0.998427f, -0.441874f, -0.0635f, 0.2619f, -0.9630f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.700848f, -0.700848f, -0.358831f, -0.0635f, 0.2619f, -0.9630f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.467835f, -0.338339f, -0.338339f, -0.1208f, -0.1723f, -0.9776f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.904531f, -0.384222f, -0.384222f, -0.1208f, -0.1723f, -0.9776f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.760581f, -0.516869f, -0.343055f, -0.1208f, -0.1723f, -0.9776f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.793392f, -0.356687f, -0.537814f, 0.5497f, 0.7955f, -0.2552f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.591866f, -0.128903f, -0.261840f, 0.5497f, 0.7955f, -0.2552f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.904531f, -0.384222f, -0.384222f, 0.5497f, 0.7955f, -0.2552f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.783441f, -0.173692f, -0.528085f, 0.5462f, 0.7966f, 0.2590f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.364271f, -0.000000f, -0.178327f, 0.5462f, 0.7966f, 0.2590f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.591866f, -0.128903f, -0.261840f, 0.5462f, 0.7966f, 0.2590f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.688762f, 0.154182f, -0.468578f, 0.6289f, 0.1218f, 0.7678f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.364271f, -0.000000f, -0.178327f, 0.6289f, 0.1218f, 0.7678f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.815583f, -0.000000f, -0.547997f, 0.6289f, 0.1218f, 0.7678f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.850258f, 0.380313f, -0.574113f, 0.2495f, -0.5501f, -0.7969f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.984669f, 0.202341f, -0.409171f, 0.2495f, -0.5501f, -0.7969f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.688762f, 0.154182f, -0.468578f, 0.2495f, -0.5501f, -0.7969f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.860198f, 0.595336f, -0.595336f, 0.9032f, -0.0833f, -0.4210f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.934747f, 0.395938f, -0.395938f, 0.9032f, -0.0833f, -0.4210f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.850258f, 0.380313f, -0.574113f, 0.9032f, -0.0833f, -0.4210f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.760581f, -0.516869f, -0.343055f, 0.9059f, 0.3540f, -0.2323f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.729381f, -0.313419f, -0.154613f, 0.9059f, 0.3540f, -0.2323f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.814341f, -0.547506f, -0.180060f, 0.9059f, 0.3540f, -0.2323f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.904531f, -0.384222f, -0.384222f, 0.7334f, -0.2438f, 0.6346f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.786823f, -0.163316f, -0.163316f, 0.7334f, -0.2438f, 0.6346f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.729381f, -0.313419f, -0.154613f, 0.7334f, -0.2438f, 0.6346f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.591866f, -0.128903f, -0.261840f, 0.4654f, 0.1307f, -0.8754f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.752095f, 0.000000f, -0.157400f, 0.4654f, 0.1307f, -0.8754f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.786823f, -0.163316f, -0.163316f, 0.4654f, 0.1307f, -0.8754f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.984669f, 0.202341f, -0.409171f, 0.0328f, -0.7936f, -0.6075f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.752095f, 0.000000f, -0.157400f, 0.0328f, -0.7936f, -0.6075f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.364271f, -0.000000f, -0.178327f, 0.0328f, -0.7936f, -0.6075f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.934747f, 0.395938f, -0.395938f, 0.7605f, 0.1530f, 0.6310f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.788746f, 0.163657f, -0.163657f, 0.7605f, 0.1530f, 0.6310f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.984669f, 0.202341f, -0.409171f, 0.7605f, 0.1530f, 0.6310f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.757225f, 0.514726f, -0.341660f, 0.4655f, 0.3184f, 0.8258f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.549565f, 0.245974f, -0.120995f, 0.4655f, 0.3184f, 0.8258f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.934747f, 0.395938f, -0.395938f, 0.4655f, 0.3184f, 0.8258f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.814341f, -0.547506f, -0.180060f, 0.4874f, -0.1335f, 0.8629f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.575239f, -0.256545f, 0.000000f, 0.4874f, -0.1335f, 0.8629f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.542205f, -0.377167f, 0.000000f, 0.4874f, -0.1335f, 0.8629f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.729381f, -0.313419f, -0.154613f, 0.1558f, -0.8666f, 0.4741f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.942295f, -0.190542f, -0.000000f, 0.1558f, -0.8666f, 0.4741f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.575239f, -0.256545f, 0.000000f, 0.1558f, -0.8666f, 0.4741f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.786823f, -0.163316f, -0.163316f, 0.4065f, 0.8816f, -0.2400f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.529077f, -0.000000f, 0.000000f, 0.4065f, 0.8816f, -0.2400f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.942295f, -0.190542f, -0.000000f, 0.4065f, 0.8816f, -0.2400f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.788746f, 0.163657f, -0.163657f, 0.5739f, -0.0974f, 0.8131f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.529077f, -0.000000f, 0.000000f, 0.5739f, -0.0974f, 0.8131f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.752095f, 0.000000f, -0.157400f, 0.5739f, -0.0974f, 0.8131f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.549565f, 0.245974f, -0.120995f, 0.2346f, 0.8874f, -0.3968f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.955271f, 0.192803f, -0.000000f, 0.2346f, 0.8874f, -0.3968f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.788746f, 0.163657f, -0.163657f, 0.2346f, 0.8874f, -0.3968f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.670563f, 0.457139f, -0.150432f, 0.4243f, -0.3590f, -0.8313f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.896296f, 0.375580f, 0.000000f, 0.4243f, -0.3590f, -0.8313f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.549565f, 0.245974f, -0.120995f, 0.4243f, -0.3590f, -0.8313f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.575239f, -0.256545f, 0.000000f, 0.5738f, -0.1571f, -0.8038f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.739019f, -0.500165f, 0.164539f, 0.5738f, -0.1571f, -0.8038f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.542205f, -0.377167f, 0.000000f, 0.5738f, -0.1571f, -0.8038f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.942295f, -0.190542f, -0.000000f, 0.1334f, -0.7417f, -0.6573f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.864106f, -0.363951f, 0.179801f, 0.1334f, -0.7417f, -0.6573f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.575239f, -0.256545f, 0.000000f, 0.1334f, -0.7417f, -0.6573f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.529077f, -0.000000f, 0.000000f, 0.3813f, 0.8270f, 0.4132f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.687068f, -0.145614f, 0.145614f, 0.3813f, 0.8270f, 0.4132f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.942295f, -0.190542f, -0.000000f, 0.3813f, 0.8270f, 0.4132f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.529077f, -0.000000f, 0.000000f, 0.3426f, 0.6282f, -0.6986f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.554161f, 0.122028f, 0.122028f, 0.3426f, 0.6282f, -0.6986f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.903925f, 0.000000f, 0.183856f, 0.3426f, 0.6282f, -0.6986f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.955271f, 0.192803f, -0.000000f, 0.3155f, -0.1828f, 0.9311f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.609102f, 0.268305f, 0.132126f, 0.3155f, -0.1828f, 0.9311f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.554161f, 0.122028f, 0.122028f, 0.3155f, -0.1828f, 0.9311f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.896296f, 0.375580f, 0.000000f, 0.4904f, -0.3098f, 0.8146f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.699549f, 0.475357f, 0.156405f, 0.4904f, -0.3098f, 0.8146f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.609102f, 0.268305f, 0.132126f, 0.4904f, -0.3098f, 0.8146f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.864106f, -0.363951f, 0.179801f, 0.7092f, -0.6121f, -0.3498f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.799266f, -0.541563f, 0.359127f, 0.7092f, -0.6121f, -0.3498f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.739019f, -0.500165f, 0.164539f, 0.7092f, -0.6121f, -0.3498f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.687068f, -0.145614f, 0.145614f, 0.6465f, 0.5983f, 0.4734f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.719294f, -0.312397f, 0.312397f, 0.6465f, 0.5983f, 0.4734f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.864106f, -0.363951f, 0.179801f, 0.6465f, 0.5983f, 0.4734f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.903925f, 0.000000f, 0.183856f, 0.5472f, -0.6907f, -0.4728f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.829506f, -0.173332f, 0.350974f, 0.5472f, -0.6907f, -0.4728f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.687068f, -0.145614f, 0.145614f, 0.5472f, -0.6907f, -0.4728f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.903925f, 0.000000f, 0.183856f, 0.5340f, 0.8454f, 0.0091f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.673745f, 0.144211f, 0.292551f, 0.5340f, 0.8454f, 0.0091f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.900631f, 0.000000f, 0.377188f, 0.5340f, 0.8454f, 0.0091f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.554161f, 0.122028f, 0.122028f, 0.6126f, 0.6053f, -0.5083f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.535315f, 0.241061f, 0.241061f, 0.6126f, 0.6053f, -0.5083f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.673745f, 0.144211f, 0.292551f, 0.6126f, 0.6053f, -0.5083f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.609102f, 0.268305f, 0.132126f, 0.7744f, -0.4883f, 0.4024f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.632287f, 0.434974f, 0.289751f, 0.7744f, -0.4883f, 0.4024f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.535315f, 0.241061f, 0.241061f, 0.7744f, -0.4883f, 0.4024f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.719294f, -0.312397f, 0.312397f, 0.0319f, 0.2104f, 0.9771f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.445603f, -0.323777f, 0.323777f, 0.0319f, 0.2104f, 0.9771f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.799266f, -0.541563f, 0.359127f, 0.0319f, 0.2104f, 0.9771f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.829506f, -0.173332f, 0.350974f, 0.3923f, -0.5212f, 0.7580f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.589297f, -0.271890f, 0.407531f, 0.3923f, -0.5212f, 0.7580f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.719294f, -0.312397f, 0.312397f, 0.3923f, -0.5212f, 0.7580f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.900631f, 0.000000f, 0.377188f, 0.2318f, -0.2378f, 0.9433f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.604331f, -0.136784f, 0.415511f, 0.2318f, -0.2378f, 0.9433f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.829506f, -0.173332f, 0.350974f, 0.2318f, -0.2378f, 0.9433f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.900631f, 0.000000f, 0.377188f, 0.6493f, 0.6669f, 0.3656f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.690075f, 0.154453f, 0.469403f, 0.6493f, 0.6669f, 0.3656f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.807342f, 0.000000f, 0.542848f, 0.6493f, 0.6669f, 0.3656f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.673745f, 0.144211f, 0.292551f, 0.9686f, -0.2367f, -0.0757f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.736188f, 0.332920f, 0.501298f, 0.9686f, -0.2367f, -0.0757f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.690075f, 0.154453f, 0.469403f, 0.9686f, -0.2367f, -0.0757f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.535315f, 0.241061f, 0.241061f, 0.7426f, 0.1930f, -0.6413f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.676614f, 0.475089f, 0.475089f, 0.7426f, 0.1930f, -0.6413f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.736188f, 0.332920f, 0.501298f, 0.7426f, 0.1930f, -0.6413f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.472812f, -0.472812f, -0.472812f, 0.6240f, 0.5640f, -0.5409f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.467835f, -0.338339f, -0.338339f, 0.6240f, 0.5640f, -0.5409f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.591253f, -0.591253f, -0.459673f, 0.6240f, 0.5640f, -0.5409f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.549801f, -0.429528f, -0.549801f, 0.2335f, -0.8567f, 0.4600f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.793392f, -0.356687f, -0.537814f, 0.2335f, -0.8567f, 0.4600f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.467835f, -0.338339f, -0.338339f, 0.2335f, -0.8567f, 0.4600f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.646260f, -0.332295f, -0.646260f, 0.5997f, 0.0750f, -0.7967f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.783441f, -0.173692f, -0.528085f, 0.5997f, 0.0750f, -0.7967f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.793392f, -0.356687f, -0.537814f, 0.5997f, 0.0750f, -0.7967f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.665434f, -0.169601f, -0.665434f, 0.7385f, -0.2101f, -0.6407f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.815583f, -0.000000f, -0.547997f, 0.7385f, -0.2101f, -0.6407f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.783441f, -0.173692f, -0.528085f, 0.7385f, -0.2101f, -0.6407f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.717813f, 0.182497f, -0.717813f, 0.7733f, -0.1573f, -0.6142f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.815583f, -0.000000f, -0.547997f, 0.7733f, -0.1573f, -0.6142f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.697125f, -0.000000f, -0.697125f, 0.7733f, -0.1573f, -0.6142f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.657125f, 0.337577f, -0.657125f, 0.9393f, 0.3109f, 0.1448f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.688762f, 0.154182f, -0.468578f, 0.9393f, 0.3109f, 0.1448f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.717813f, 0.182497f, -0.717813f, 0.9393f, 0.3109f, 0.1448f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.521509f, 0.408953f, -0.521509f, 0.0031f, 0.8862f, -0.4633f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.850258f, 0.380313f, -0.574113f, 0.0031f, 0.8862f, -0.4633f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.657125f, 0.337577f, -0.657125f, 0.0031f, 0.8862f, -0.4633f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.421482f, 0.421482f, -0.421482f, -0.4879f, 0.6606f, -0.5706f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.860198f, 0.595336f, -0.595336f, -0.4879f, 0.6606f, -0.5706f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.521509f, 0.408953f, -0.521509f, -0.4879f, 0.6606f, -0.5706f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.583262f, 0.583262f, -0.453862f, 0.1387f, 0.9262f, 0.3506f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.757225f, 0.514726f, -0.341660f, 0.1387f, 0.9262f, 0.3506f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.860198f, 0.595336f, -0.595336f, 0.1387f, 0.9262f, 0.3506f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.462871f, 0.462871f, -0.243147f, -0.0857f, 0.9641f, 0.2515f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.670563f, 0.457139f, -0.150432f, -0.0857f, 0.9641f, 0.2515f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.757225f, 0.514726f, -0.341660f, -0.0857f, 0.9641f, 0.2515f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.713197f, 0.713197f, -0.181360f, 0.8939f, -0.1974f, -0.4023f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.749169f, 0.506496f, -0.000000f, 0.8939f, -0.1974f, -0.4023f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.670563f, 0.457139f, -0.150432f, 0.8939f, -0.1974f, -0.4023f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.749169f, 0.506496f, -0.000000f, 0.7960f, 0.4933f, 0.3507f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.613645f, 0.613645f, 0.156850f, 0.7960f, 0.4933f, 0.3507f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.699549f, 0.475357f, 0.156405f, 0.7960f, 0.4933f, 0.3507f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.699549f, 0.475357f, 0.156405f, 0.8551f, -0.4199f, 0.3042f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.755718f, 0.755718f, 0.385504f, 0.8551f, -0.4199f, 0.3042f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.632287f, 0.434974f, 0.289751f, 0.8551f, -0.4199f, 0.3042f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.632287f, 0.434974f, 0.289751f, 0.2452f, 0.9337f, -0.2607f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.498710f, 0.498710f, 0.392372f, 0.2452f, 0.9337f, -0.2607f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.676614f, 0.475089f, 0.475089f, 0.2452f, 0.9337f, -0.2607f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.676614f, 0.475089f, 0.475089f, 0.1403f, -0.5319f, 0.8351f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.538827f, 0.538827f, 0.538827f, 0.1403f, -0.5319f, 0.8351f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.432840f, 0.344470f, 0.432840f, 0.1403f, -0.5319f, 0.8351f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.736188f, 0.332920f, 0.501298f, -0.2044f, 0.2464f, 0.9474f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.432840f, 0.344470f, 0.432840f, -0.2044f, 0.2464f, 0.9474f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.465944f, 0.244641f, 0.465945f, -0.2044f, 0.2464f, 0.9474f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.690075f, 0.154453f, 0.469403f, 0.3731f, 0.9277f, 0.0124f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.465944f, 0.244641f, 0.465945f, 0.3731f, 0.9277f, 0.0124f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.653571f, 0.166680f, 0.653571f, 0.3731f, 0.9277f, 0.0124f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.807342f, 0.000000f, 0.542848f, 0.7274f, 0.6859f, -0.0223f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.653571f, 0.166680f, 0.653571f, 0.7274f, 0.6859f, -0.0223f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.815698f, -0.000000f, 0.815698f, 0.7274f, 0.6859f, -0.0223f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.807342f, 0.000000f, 0.542848f, 0.6741f, -0.6021f, -0.4278f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.781988f, -0.198297f, 0.781988f, 0.6741f, -0.6021f, -0.4278f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.604331f, -0.136784f, 0.415511f, 0.6741f, -0.6021f, -0.4278f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.604331f, -0.136784f, 0.415511f, 0.9545f, -0.0894f, -0.2844f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.657647f, -0.337831f, 0.657647f, 0.9545f, -0.0894f, -0.2844f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.589297f, -0.271890f, 0.407531f, 0.9545f, -0.0894f, -0.2844f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.589297f, -0.271890f, 0.407531f, 0.1276f, -0.9262f, 0.3549f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.384191f, -0.309091f, 0.384191f, 0.1276f, -0.9262f, 0.3549f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.445603f, -0.323777f, 0.323777f, 0.1276f, -0.9262f, 0.3549f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.445603f, -0.323777f, 0.323777f, 0.2302f, 0.1034f, 0.9676f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.349401f, -0.349401f, 0.349401f, 0.2302f, 0.1034f, 0.9676f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.425849f, -0.425849f, 0.339386f, 0.2302f, 0.1034f, 0.9676f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.799266f, -0.541563f, 0.359127f, -0.1737f, -0.4076f, 0.8965f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.425849f, -0.425849f, 0.339386f, -0.1737f, -0.4076f, 0.8965f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.578778f, -0.578778f, 0.299491f, -0.1737f, -0.4076f, 0.8965f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.739019f, -0.500165f, 0.164539f, 0.6889f, -0.2166f, 0.6918f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.578778f, -0.578778f, 0.299491f, 0.6889f, -0.2166f, 0.6918f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.676028f, -0.676028f, 0.172209f, 0.6889f, -0.2166f, 0.6918f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.542205f, -0.377167f, 0.000000f, 0.9322f, 0.3196f, -0.1697f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.676028f, -0.676028f, 0.172209f, 0.9322f, 0.3196f, -0.1697f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.628324f, -0.628324f, 0.000000f, 0.9322f, 0.3196f, -0.1697f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.623072f, -0.623072f, -0.159171f, 0.9457f, 0.3243f, -0.0205f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.542205f, -0.377167f, 0.000000f, 0.9457f, 0.3243f, -0.0205f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.628324f, -0.628324f, 0.000000f, 0.9457f, 0.3243f, -0.0205f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.499946f, -0.499946f, -0.261170f, 0.1287f, -0.5532f, -0.8231f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.814341f, -0.547506f, -0.180060f, 0.1287f, -0.5532f, -0.8231f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.623072f, -0.623072f, -0.159171f, 0.1287f, -0.5532f, -0.8231f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.591253f, -0.591253f, -0.459673f, 0.0828f, -0.8904f, 0.4476f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.760581f, -0.516869f, -0.343055f, 0.0828f, -0.8904f, 0.4476f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.499946f, -0.499946f, -0.261170f, 0.0828f, -0.8904f, 0.4476f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.492963f, -0.492963f, -0.703903f, 0.5549f, -0.7196f, -0.4173f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.331187f, -0.331187f, -0.767752f, 0.5549f, -0.7196f, -0.4173f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.263289f, -0.394317f, -0.568596f, 0.5549f, -0.7196f, -0.4173f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.411329f, -0.274362f, -0.595247f, -0.9091f, -0.0185f, -0.4162f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.330872f, -0.163312f, -0.775913f, -0.9091f, -0.0185f, -0.4162f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.331187f, -0.331187f, -0.767752f, -0.9091f, -0.0185f, -0.4162f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.409616f, -0.134851f, -0.594951f, -0.8760f, 0.2390f, -0.4188f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.311449f, -0.000000f, -0.723324f, -0.8760f, 0.2390f, -0.4188f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.330872f, -0.163312f, -0.775913f, -0.8760f, 0.2390f, -0.4188f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.588619f, 0.193539f, -0.879752f, -0.5019f, -0.8496f, -0.1618f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.311449f, -0.000000f, -0.723324f, -0.5019f, -0.8496f, -0.1618f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.372340f, -0.000000f, -0.534481f, -0.5019f, -0.8496f, -0.1618f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.441012f, 0.293681f, -0.641746f, 0.2851f, 0.8074f, -0.5165f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.333924f, 0.164833f, -0.784050f, 0.2851f, 0.8074f, -0.5165f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.588619f, 0.193539f, -0.879752f, 0.2851f, 0.8074f, -0.5165f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.445498f, 0.445498f, -0.631437f, -0.3626f, 0.0525f, -0.9305f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.302980f, 0.302980f, -0.695006f, -0.3626f, 0.0525f, -0.9305f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.441012f, 0.293681f, -0.641746f, -0.3626f, 0.0525f, -0.9305f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.263289f, -0.394317f, -0.568596f, -0.6302f, 0.5981f, -0.4951f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.125979f, -0.255973f, -0.576224f, -0.6302f, 0.5981f, -0.4951f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.198604f, -0.604068f, -0.904332f, -0.6302f, 0.5981f, -0.4951f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.331187f, -0.331187f, -0.767752f, 0.5800f, -0.7453f, -0.3288f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.173953f, -0.173953f, -0.846765f, 0.5800f, -0.7453f, -0.3288f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.125979f, -0.255973f, -0.576224f, 0.5800f, -0.7453f, -0.3288f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.330872f, -0.163312f, -0.775913f, -0.4124f, -0.2511f, -0.8757f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.181933f, -0.000000f, -0.892884f, -0.4124f, -0.2511f, -0.8757f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.173953f, -0.173953f, -0.846765f, -0.4124f, -0.2511f, -0.8757f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.333924f, 0.164833f, -0.784050f, -0.7542f, -0.3151f, -0.5761f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.181933f, -0.000000f, -0.892884f, -0.7542f, -0.3151f, -0.5761f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.311449f, -0.000000f, -0.723324f, -0.7542f, -0.3151f, -0.5761f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.302980f, 0.302980f, -0.695006f, -0.4853f, 0.5479f, -0.6814f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.180090f, 0.180090f, -0.881345f, -0.4853f, 0.5479f, -0.6814f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.333924f, 0.164833f, -0.784050f, -0.4853f, 0.5479f, -0.6814f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.302979f, 0.455296f, -0.664123f, -0.6483f, 0.1513f, -0.7462f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.168564f, 0.341408f, -0.804003f, -0.6483f, 0.1513f, -0.7462f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.302980f, 0.302980f, -0.695006f, -0.6483f, 0.1513f, -0.7462f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.198604f, -0.604068f, -0.904332f, 0.1522f, -0.7958f, -0.5861f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.441348f, -1.073682f, 0.1522f, -0.7958f, -0.5861f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.586301f, -0.876880f, 0.1522f, -0.7958f, -0.5861f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.125979f, -0.255973f, -0.576224f, -0.9721f, -0.0402f, -0.2312f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.220143f, -1.112172f, -0.9721f, -0.0402f, -0.2312f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.441348f, -1.073682f, -0.9721f, -0.0402f, -0.2312f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.173953f, -0.173953f, -0.846765f, -0.5503f, 0.6835f, -0.4796f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.000000f, -0.798438f, -0.5503f, 0.6835f, -0.4796f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.220143f, -1.112172f, -0.5503f, 0.6835f, -0.4796f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.180090f, 0.180090f, -0.881345f, 0.4601f, 0.0521f, -0.8863f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.000000f, -0.798438f, 0.4601f, 0.0521f, -0.8863f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.181933f, -0.000000f, -0.892884f, 0.4601f, 0.0521f, -0.8863f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.168564f, 0.341408f, -0.804003f, 0.3023f, 0.3944f, -0.8678f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.169804f, -0.823278f, 0.3023f, 0.3944f, -0.8678f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.180090f, 0.180090f, -0.881345f, 0.3023f, 0.3944f, -0.8678f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.123491f, 0.374969f, -0.539827f, -0.4677f, 0.8833f, -0.0324f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.422621f, -1.023172f, -0.4677f, 0.8833f, -0.0324f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.168564f, 0.341408f, -0.804003f, -0.4677f, 0.8833f, -0.0324f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.441348f, -1.073682f, 0.9546f, -0.2398f, -0.1767f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.120480f, -0.365784f, -0.525213f, 0.9546f, -0.2398f, -0.1767f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.586301f, -0.876880f, 0.9546f, -0.2398f, -0.1767f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.220143f, -1.112172f, 0.8496f, -0.0904f, -0.5195f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170119f, -0.344528f, -0.812321f, 0.8496f, -0.0904f, -0.5195f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.441348f, -1.073682f, 0.8496f, -0.0904f, -0.5195f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.000000f, -0.798438f, 0.5307f, 0.6938f, -0.4868f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.175831f, -0.175831f, -0.857344f, 0.5307f, 0.6938f, -0.4868f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.220143f, -1.112172f, 0.5307f, 0.6938f, -0.4868f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.000000f, -0.798438f, 0.4758f, -0.7545f, -0.4520f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185700f, 0.185700f, -0.912960f, 0.4758f, -0.7545f, -0.4520f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.139827f, 0.000000f, -0.651244f, 0.4758f, -0.7545f, -0.4520f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.169804f, -0.823278f, -0.3412f, 0.7439f, -0.5746f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.154583f, 0.313359f, -0.729220f, -0.3412f, 0.7439f, -0.5746f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185700f, 0.185700f, -0.912960f, -0.3412f, 0.7439f, -0.5746f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.422621f, -1.023172f, 0.8533f, -0.1423f, -0.5016f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.169152f, 0.514237f, -0.761408f, 0.8533f, -0.1423f, -0.5016f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.154583f, 0.313359f, -0.729220f, 0.8533f, -0.1423f, -0.5016f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.170119f, -0.344528f, -0.812321f, 0.0212f, -0.9973f, -0.0702f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.243242f, -0.363517f, -0.520346f, 0.0212f, -0.9973f, -0.0702f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.120480f, -0.365784f, -0.525213f, 0.0212f, -0.9973f, -0.0702f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.175831f, -0.175831f, -0.857344f, 0.0244f, -0.2586f, -0.9657f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.346531f, -0.346531f, -0.807324f, 0.0244f, -0.2586f, -0.9657f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170119f, -0.344528f, -0.812321f, 0.0244f, -0.2586f, -0.9657f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.139827f, 0.000000f, -0.651244f, 0.0175f, 0.7622f, -0.6472f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.360539f, -0.178100f, -0.855010f, 0.0175f, 0.7622f, -0.6472f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.175831f, -0.175831f, -0.857344f, 0.0175f, 0.7622f, -0.6472f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.139827f, 0.000000f, -0.651244f, -0.4143f, -0.6304f, -0.6565f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.429019f, 0.212234f, -1.037585f, -0.4143f, -0.6304f, -0.6565f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.328992f, 0.000000f, -0.770641f, -0.4143f, -0.6304f, -0.6565f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.185700f, 0.185700f, -0.912960f, -0.3824f, 0.7057f, -0.5964f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.352443f, 0.352443f, -0.822573f, -0.3824f, 0.7057f, -0.5964f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.429019f, 0.212234f, -1.037585f, -0.3824f, 0.7057f, -0.5964f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.154583f, 0.313359f, -0.729220f, -0.4481f, 0.1739f, -0.8769f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.354225f, 0.534031f, -0.787467f, -0.4481f, 0.1739f, -0.8769f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.352443f, 0.352443f, -0.822573f, -0.4481f, 0.1739f, -0.8769f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.346531f, -0.346531f, -0.807324f, -0.5918f, -0.7636f, -0.2582f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.495694f, -0.495695f, -0.708073f, -0.5918f, -0.7636f, -0.2582f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.243242f, -0.363517f, -0.520346f, -0.5918f, -0.7636f, -0.2582f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.360539f, -0.178100f, -0.855010f, 0.9849f, -0.1178f, -0.1267f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.388761f, -0.259673f, -0.559892f, 0.9849f, -0.1178f, -0.1267f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.346531f, -0.346531f, -0.807324f, 0.9849f, -0.1178f, -0.1267f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.328992f, 0.000000f, -0.770641f, 0.6182f, 0.4233f, -0.6624f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.490235f, -0.161283f, -0.723219f, 0.6182f, 0.4233f, -0.6624f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.360539f, -0.178100f, -0.855010f, 0.6182f, 0.4233f, -0.6624f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.328992f, 0.000000f, -0.770641f, -0.1558f, 0.6645f, -0.7309f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.451548f, 0.148599f, -0.661667f, -0.1558f, 0.6645f, -0.7309f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.549247f, -0.000000f, -0.817583f, -0.1558f, 0.6645f, -0.7309f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.429019f, 0.212234f, -1.037585f, 0.9964f, 0.0698f, -0.0479f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.442193f, 0.294450f, -0.643596f, 0.9964f, 0.0698f, -0.0479f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.451548f, 0.148599f, -0.661667f, 0.9964f, 0.0698f, -0.0479f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.352443f, 0.352443f, -0.822573f, 0.8614f, -0.1572f, -0.4829f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.464074f, 0.464074f, -0.659797f, 0.8614f, -0.1572f, -0.4829f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.442193f, 0.294450f, -0.643596f, 0.8614f, -0.1572f, -0.4829f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.450496f, -0.450496f, -0.450496f, -0.9602f, -0.2003f, 0.1945f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.492963f, -0.492963f, -0.703903f, -0.9602f, -0.2003f, 0.1945f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.449732f, -0.577583f, -0.577583f, -0.9602f, -0.2003f, 0.1945f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.608913f, -0.472516f, -0.608913f, -0.4807f, 0.5276f, -0.7004f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.411329f, -0.274362f, -0.595247f, -0.4807f, 0.5276f, -0.7004f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.492963f, -0.492963f, -0.703903f, -0.4807f, 0.5276f, -0.7004f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.467353f, -0.245326f, -0.467352f, -0.9149f, 0.0121f, -0.4035f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.409616f, -0.134851f, -0.594951f, -0.9149f, 0.0121f, -0.4035f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.411329f, -0.274362f, -0.595247f, -0.9149f, 0.0121f, -0.4035f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.580775f, -0.148757f, -0.580775f, -0.1092f, 0.4317f, -0.8954f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.372340f, -0.000000f, -0.534481f, -0.1092f, 0.4317f, -0.8954f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.409616f, -0.134851f, -0.594951f, -0.1092f, 0.4317f, -0.8954f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.681304f, 0.173508f, -0.681304f, -0.4509f, -0.8870f, -0.0994f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.372340f, -0.000000f, -0.534481f, -0.4509f, -0.8870f, -0.0994f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.401631f, 0.000000f, -0.401631f, -0.4509f, -0.8870f, -0.0994f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.674306f, 0.345929f, -0.674306f, -0.9068f, 0.0538f, -0.4181f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.588619f, 0.193539f, -0.879752f, -0.9068f, 0.0538f, -0.4181f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.681304f, 0.173508f, -0.681304f, -0.9068f, 0.0538f, -0.4181f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.437378f, 0.347770f, -0.437378f, 0.2449f, 0.9362f, -0.2521f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.441012f, 0.293681f, -0.641746f, 0.2449f, 0.9362f, -0.2521f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.674306f, 0.345929f, -0.674306f, 0.2449f, 0.9362f, -0.2521f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.271962f, 0.271962f, -0.271962f, -0.0422f, 0.8916f, 0.4508f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.445498f, 0.445498f, -0.631437f, -0.0422f, 0.8916f, 0.4508f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.437378f, 0.347770f, -0.437378f, -0.0422f, 0.8916f, 0.4508f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.429708f, 0.550049f, -0.550049f, -0.2157f, 0.6199f, -0.7545f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.302979f, 0.455296f, -0.664123f, -0.2157f, 0.6199f, -0.7545f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.445498f, 0.445498f, -0.631437f, -0.2157f, 0.6199f, -0.7545f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.193485f, 0.360709f, -0.360709f, 0.2898f, 0.9384f, 0.1880f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.123491f, 0.374969f, -0.539827f, 0.2898f, 0.9384f, 0.1880f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.302979f, 0.455296f, -0.664123f, 0.2898f, 0.9384f, 0.1880f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.161804f, 0.633768f, -0.633768f, -0.8446f, -0.2887f, -0.4509f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.617128f, -0.926213f, -0.8446f, -0.2887f, -0.4509f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.123491f, 0.374969f, -0.539827f, -0.8446f, -0.2887f, -0.4509f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.617128f, -0.926213f, 0.5886f, 0.8018f, -0.1036f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.143451f, 0.559224f, -0.559224f, 0.5886f, 0.8018f, -0.1036f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.169152f, 0.514237f, -0.761408f, 0.5886f, 0.8018f, -0.1036f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.169152f, 0.514237f, -0.761408f, -0.1145f, 0.9916f, -0.0599f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.280869f, 0.540470f, -0.540470f, -0.1145f, 0.9916f, -0.0599f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.354225f, 0.534031f, -0.787467f, -0.1145f, 0.9916f, -0.0599f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.354225f, 0.534031f, -0.787467f, 0.2093f, 0.9222f, 0.3252f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.325094f, 0.406197f, -0.406197f, 0.2093f, 0.9222f, 0.3252f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.464074f, 0.464074f, -0.659797f, 0.2093f, 0.9222f, 0.3252f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.464074f, 0.464074f, -0.659797f, 0.3383f, 0.9140f, 0.2238f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.421482f, 0.421482f, -0.421482f, 0.3383f, 0.9140f, 0.2238f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.521509f, 0.408953f, -0.521509f, 0.3383f, 0.9140f, 0.2238f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.442193f, 0.294450f, -0.643596f, -0.1934f, 0.7752f, -0.6014f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.521509f, 0.408953f, -0.521509f, -0.1934f, 0.7752f, -0.6014f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.657125f, 0.337577f, -0.657125f, -0.1934f, 0.7752f, -0.6014f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.451548f, 0.148599f, -0.661667f, -0.2317f, 0.2745f, -0.9332f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.657125f, 0.337577f, -0.657125f, -0.2317f, 0.2745f, -0.9332f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.717813f, 0.182497f, -0.717813f, -0.2317f, 0.2745f, -0.9332f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.549247f, -0.000000f, -0.817583f, 0.6237f, -0.1575f, -0.7656f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.717813f, 0.182497f, -0.717813f, 0.6237f, -0.1575f, -0.7656f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.697125f, -0.000000f, -0.697125f, 0.6237f, -0.1575f, -0.7656f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.549247f, -0.000000f, -0.817583f, 0.2370f, -0.5538f, -0.7982f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.665434f, -0.169601f, -0.665434f, 0.2370f, -0.5538f, -0.7982f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.490235f, -0.161283f, -0.723219f, 0.2370f, -0.5538f, -0.7982f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.490235f, -0.161283f, -0.723219f, -0.3999f, -0.6539f, -0.6423f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.646260f, -0.332295f, -0.646260f, -0.3999f, -0.6539f, -0.6423f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.388761f, -0.259673f, -0.559892f, -0.3999f, -0.6539f, -0.6423f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.388761f, -0.259673f, -0.559892f, 0.6539f, 0.5920f, -0.4711f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.549801f, -0.429528f, -0.549801f, 0.6539f, 0.5920f, -0.4711f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.495694f, -0.495695f, -0.708073f, 0.6539f, 0.5920f, -0.4711f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.495694f, -0.495695f, -0.708073f, -0.1294f, -0.9881f, 0.0835f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.472812f, -0.472812f, -0.472812f, -0.1294f, -0.9881f, 0.0835f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.362015f, -0.456966f, -0.456966f, -0.1294f, -0.9881f, 0.0835f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.243242f, -0.363517f, -0.520346f, 0.6722f, 0.4618f, -0.5787f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.362015f, -0.456966f, -0.456966f, 0.6722f, 0.4618f, -0.5787f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.329947f, -0.641428f, -0.641428f, 0.6722f, 0.4618f, -0.5787f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.120480f, -0.365784f, -0.525213f, 0.1207f, 0.4621f, -0.8786f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.329947f, -0.641428f, -0.641428f, 0.1207f, 0.4621f, -0.8786f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.174764f, -0.686407f, -0.686407f, 0.1207f, 0.4621f, -0.8786f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.586301f, -0.876880f, -0.4150f, -0.9049f, -0.0948f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.174764f, -0.686407f, -0.686407f, -0.4150f, -0.9049f, -0.0948f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.613845f, -0.613845f, -0.4150f, -0.9049f, -0.0948f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.148160f, -0.578352f, -0.578352f, -0.2545f, -0.9618f, -0.1007f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.586301f, -0.876880f, -0.2545f, -0.9618f, -0.1007f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.613845f, -0.613845f, -0.2545f, -0.9618f, -0.1007f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.245217f, -0.467128f, -0.467128f, -0.6590f, -0.7350f, 0.1600f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.198604f, -0.604068f, -0.904332f, -0.6590f, -0.7350f, 0.1600f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.148160f, -0.578352f, -0.578352f, -0.6590f, -0.7350f, 0.1600f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.449732f, -0.577583f, -0.577583f, 0.6065f, -0.5909f, -0.5320f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.263289f, -0.394317f, -0.568596f, 0.6065f, -0.5909f, -0.5320f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.245217f, -0.467128f, -0.467128f, 0.6065f, -0.5909f, -0.5320f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.431201f, 0.609609f, -0.431201f, 0.4635f, 0.6552f, 0.5966f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.336864f, 0.745681f, -0.507358f, 0.4635f, 0.6552f, 0.5966f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.256078f, 0.574043f, -0.256078f, 0.4635f, 0.6552f, 0.5966f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.336864f, 0.745681f, -0.507358f, 0.2303f, 0.7621f, -0.6052f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.173045f, 0.780299f, -0.526111f, 0.2303f, 0.7621f, -0.6052f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185726f, 0.895799f, -0.375838f, 0.2303f, 0.7621f, -0.6052f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.173045f, 0.780299f, -0.526111f, -0.2069f, 0.9131f, 0.3513f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.728611f, -0.493650f, -0.2069f, 0.9131f, 0.3513f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.647736f, -0.283424f, -0.2069f, 0.9131f, 0.3513f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.179752f, 0.812846f, -0.546567f, 0.7159f, 0.6963f, -0.0521f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.171405f, 0.819202f, -0.347109f, 0.7159f, 0.6963f, -0.0521f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.647736f, -0.283424f, 0.7159f, 0.6963f, -0.0521f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.301697f, 0.661039f, -0.453327f, -0.1017f, 0.6110f, -0.7851f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.342433f, 0.796756f, -0.342433f, -0.1017f, 0.6110f, -0.7851f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.171405f, 0.819202f, -0.347109f, -0.1017f, 0.6110f, -0.7851f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.458239f, 0.650889f, -0.458239f, -0.7906f, 0.1580f, 0.5916f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.315001f, 0.444342f, -0.211664f, -0.7906f, 0.1580f, 0.5916f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.342433f, 0.796756f, -0.342433f, -0.7906f, 0.1580f, 0.5916f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.362647f, 0.518982f, -0.242676f, 0.2571f, 0.2720f, -0.9273f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.256078f, 0.574043f, -0.256078f, 0.2571f, 0.2720f, -0.9273f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.339102f, 0.797856f, -0.167415f, 0.2571f, 0.2720f, -0.9273f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.256078f, 0.574043f, -0.256078f, 0.7198f, 0.3748f, 0.5842f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185726f, 0.895799f, -0.375838f, 0.7198f, 0.3748f, 0.5842f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.133430f, 0.618410f, -0.133430f, 0.7198f, 0.3748f, 0.5842f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.185726f, 0.895799f, -0.375838f, -0.7618f, 0.3731f, -0.5295f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.647736f, -0.283424f, -0.7618f, 0.3731f, -0.5295f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.811828f, -0.167809f, -0.7618f, 0.3731f, -0.5295f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.171405f, 0.819202f, -0.347109f, -0.1662f, 0.9659f, 0.1986f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.162598f, 0.782778f, -0.162598f, -0.1662f, 0.9659f, 0.1986f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.811828f, -0.167809f, -0.1662f, 0.9659f, 0.1986f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.342433f, 0.796756f, -0.342433f, 0.1516f, 0.9856f, -0.0750f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.343691f, 0.810090f, -0.169702f, 0.1516f, 0.9856f, -0.0750f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.162598f, 0.782778f, -0.162598f, 0.1516f, 0.9856f, -0.0750f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.315001f, 0.444342f, -0.211664f, -0.1123f, 0.1046f, -0.9882f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.491815f, 0.725733f, -0.161801f, -0.1123f, 0.1046f, -0.9882f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.343691f, 0.810090f, -0.169702f, -0.1123f, 0.1046f, -0.9882f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.353197f, 0.505187f, -0.116353f, 0.8937f, 0.1185f, 0.4328f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.339102f, 0.797856f, -0.167415f, 0.8937f, 0.1185f, 0.4328f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.279382f, 0.636835f, -0.000000f, 0.8937f, 0.1185f, 0.4328f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.339102f, 0.797856f, -0.167415f, -0.5755f, 0.5440f, -0.6106f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.133430f, 0.618410f, -0.133430f, -0.5755f, 0.5440f, -0.6106f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.166254f, 0.802906f, -0.000000f, -0.5755f, 0.5440f, -0.6106f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.133430f, 0.618410f, -0.133430f, 0.6831f, 0.3581f, -0.6364f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.811828f, -0.167809f, 0.6831f, 0.3581f, -0.6364f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 1.110028f, -0.000000f, 0.6831f, 0.3581f, -0.6364f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.162598f, 0.782778f, -0.162598f, -0.3488f, 0.5503f, -0.7587f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.197909f, 0.984574f, -0.000000f, -0.3488f, 0.5503f, -0.7587f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 1.110028f, -0.000000f, -0.3488f, 0.5503f, -0.7587f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.343691f, 0.810090f, -0.169702f, -0.8575f, 0.3881f, 0.3376f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.313565f, 0.729030f, -0.000000f, -0.8575f, 0.3881f, 0.3376f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.197909f, 0.984574f, -0.000000f, -0.8575f, 0.3881f, 0.3376f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.491815f, 0.725733f, -0.161801f, 0.3258f, 0.8672f, -0.3766f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.549233f, 0.817561f, 0.000000f, 0.3258f, 0.8672f, -0.3766f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.313565f, 0.729030f, -0.000000f, 0.3258f, 0.8672f, -0.3766f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.279382f, 0.636835f, -0.000000f, 0.4482f, 0.4459f, -0.7748f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.364005f, 0.864249f, 0.179827f, 0.4482f, 0.4459f, -0.7748f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.481310f, 0.709020f, 0.158357f, 0.4482f, 0.4459f, -0.7748f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.166254f, 0.802906f, -0.000000f, -0.1689f, 0.9746f, -0.1467f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170876f, 0.829425f, 0.170876f, -0.1689f, 0.9746f, -0.1467f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.364005f, 0.864249f, 0.179827f, -0.1689f, 0.9746f, -0.1467f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 1.110028f, -0.000000f, -0.4074f, 0.2811f, 0.8689f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.668472f, 0.142829f, -0.4074f, 0.2811f, 0.8689f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170876f, 0.829425f, 0.170876f, -0.4074f, 0.2811f, 0.8689f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 1.110028f, -0.000000f, -0.2755f, 0.4345f, 0.8575f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.197909f, 0.984574f, -0.000000f, -0.2755f, 0.4345f, 0.8575f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.150869f, 0.716685f, 0.150869f, -0.2755f, 0.4345f, 0.8575f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.197909f, 0.984574f, -0.000000f, -0.9053f, 0.4097f, -0.1121f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.313565f, 0.729030f, -0.000000f, -0.9053f, 0.4097f, -0.1121f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.322342f, 0.753172f, 0.159061f, -0.9053f, 0.4097f, -0.1121f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.313565f, 0.729030f, -0.000000f, 0.3083f, 0.8206f, -0.4812f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.549233f, 0.817561f, 0.000000f, 0.3083f, 0.8206f, -0.4812f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.653224f, 0.982542f, 0.214721f, 0.3083f, 0.8206f, -0.4812f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.364005f, 0.864249f, 0.179827f, -0.5117f, 0.3374f, 0.7902f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.240776f, 0.534579f, 0.240776f, -0.5117f, 0.3374f, 0.7902f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.487157f, 0.714036f, 0.323716f, -0.5117f, 0.3374f, 0.7902f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.170876f, 0.829425f, 0.170876f, 0.9306f, 0.2773f, 0.2390f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.157657f, 0.745663f, 0.319526f, 0.9306f, 0.2773f, 0.2390f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.240776f, 0.534579f, 0.240776f, 0.9306f, 0.2773f, 0.2390f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.668472f, 0.142829f, -0.2617f, 0.9481f, -0.1807f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.698882f, 0.302387f, -0.2617f, 0.9481f, -0.1807f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.157657f, 0.745663f, 0.319526f, -0.2617f, 0.9481f, -0.1807f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.668472f, 0.142829f, 0.1906f, 0.4509f, 0.8720f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.150869f, 0.716685f, 0.150869f, 0.1906f, 0.4509f, 0.8720f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.117652f, 0.531685f, 0.239267f, 0.1906f, 0.4509f, 0.8720f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.150869f, 0.716685f, 0.150869f, 0.1513f, 0.5228f, 0.8389f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.322342f, 0.753172f, 0.159061f, 0.1513f, 0.5228f, 0.8389f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.257172f, 0.576865f, 0.257172f, 0.1513f, 0.5228f, 0.8389f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.322342f, 0.753172f, 0.159061f, 0.5255f, 0.6156f, 0.5873f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.653224f, 0.982542f, 0.214721f, 0.5255f, 0.6156f, 0.5873f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.504213f, 0.740755f, 0.334817f, 0.5255f, 0.6156f, 0.5873f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.240776f, 0.534579f, 0.240776f, 0.8371f, 0.2944f, -0.4611f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313369f, 0.689132f, 0.471260f, 0.8371f, 0.2944f, -0.4611f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.317692f, 0.436313f, 0.317692f, 0.8371f, 0.2944f, -0.4611f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.157657f, 0.745663f, 0.319526f, -0.6195f, 0.2710f, 0.7367f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.121374f, 0.529553f, 0.368511f, -0.6195f, 0.2710f, 0.7367f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313369f, 0.689132f, 0.471260f, -0.6195f, 0.2710f, 0.7367f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.698882f, 0.302387f, 0.8316f, 0.5114f, -0.2168f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.798499f, 0.537322f, 0.8316f, 0.5114f, -0.2168f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.121374f, 0.529553f, 0.368511f, 0.8316f, 0.5114f, -0.2168f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.698882f, 0.302387f, -0.6500f, 0.6221f, -0.4365f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.117652f, 0.531685f, 0.239267f, -0.6500f, 0.6221f, -0.4365f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.145037f, 0.644381f, 0.440683f, -0.6500f, 0.6221f, -0.4365f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.117652f, 0.531685f, 0.239267f, 0.2824f, 0.5650f, 0.7752f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.257172f, 0.576865f, 0.257172f, 0.2824f, 0.5650f, 0.7752f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.219242f, 0.462580f, 0.326643f, 0.2824f, 0.5650f, 0.7752f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.257172f, 0.576865f, 0.257172f, 0.5888f, 0.6817f, 0.4344f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.504213f, 0.740755f, 0.334817f, 0.5888f, 0.6817f, 0.4344f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.436330f, 0.617440f, 0.436330f, 0.5888f, 0.6817f, 0.4344f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.421482f, 0.421482f, -0.421482f, -0.1498f, -0.0433f, -0.9878f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.325094f, 0.406197f, -0.406197f, -0.1498f, -0.0433f, -0.9878f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.431201f, 0.609609f, -0.431201f, -0.1498f, -0.0433f, -0.9878f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.325094f, 0.406197f, -0.406197f, 0.8755f, -0.1656f, -0.4540f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.280869f, 0.540470f, -0.540470f, 0.8755f, -0.1656f, -0.4540f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.336864f, 0.745681f, -0.507358f, 0.8755f, -0.1656f, -0.4540f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.280869f, 0.540470f, -0.540470f, 0.1511f, 0.1266f, -0.9804f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.143451f, 0.559224f, -0.559224f, 0.1511f, 0.1266f, -0.9804f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.173045f, 0.780299f, -0.526111f, 0.1511f, 0.1266f, -0.9804f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.143451f, 0.559224f, -0.559224f, 0.7578f, 0.6520f, -0.0265f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.719448f, -0.719448f, 0.7578f, 0.6520f, -0.0265f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.728611f, -0.493650f, 0.7578f, 0.6520f, -0.0265f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.161804f, 0.633768f, -0.633768f, 0.4343f, 0.4292f, -0.7920f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.179752f, 0.812846f, -0.546567f, 0.4343f, 0.4292f, -0.7920f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.728611f, -0.493650f, 0.4343f, 0.4292f, -0.7920f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.193485f, 0.360709f, -0.360709f, -0.2368f, -0.3632f, -0.9011f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.301697f, 0.661039f, -0.453327f, -0.2368f, -0.3632f, -0.9011f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.179752f, 0.812846f, -0.546567f, -0.2368f, -0.3632f, -0.9011f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.429708f, 0.550049f, -0.550049f, -0.0202f, 0.6700f, -0.7421f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.458239f, 0.650889f, -0.458239f, -0.0202f, 0.6700f, -0.7421f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.301697f, 0.661039f, -0.453327f, -0.0202f, 0.6700f, -0.7421f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.271962f, 0.271962f, -0.271962f, 0.0339f, -0.4275f, -0.9034f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.477370f, 0.477370f, -0.376854f, 0.0339f, -0.4275f, -0.9034f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.458239f, 0.650889f, -0.458239f, 0.0339f, -0.4275f, -0.9034f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.477370f, 0.477370f, -0.376854f, 0.5428f, 0.7469f, -0.3842f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.601862f, 0.601862f, -0.310713f, 0.5428f, 0.7469f, -0.3842f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.315001f, 0.444342f, -0.211664f, 0.5428f, 0.7469f, -0.3842f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.601862f, 0.601862f, -0.310713f, 0.1594f, 0.6979f, -0.6983f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.755952f, 0.755952f, -0.191887f, 0.1594f, 0.6979f, -0.6983f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.491815f, 0.725733f, -0.161801f, 0.1594f, 0.6979f, -0.6983f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.755952f, 0.755952f, -0.191887f, -0.4682f, 0.8530f, 0.2305f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.722475f, 0.722475f, 0.000000f, -0.4682f, 0.8530f, 0.2305f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.549233f, 0.817561f, 0.000000f, -0.4682f, 0.8530f, 0.2305f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.549233f, 0.817561f, 0.000000f, -0.3373f, 0.6145f, 0.7132f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.722475f, 0.722475f, 0.000000f, -0.3373f, 0.6145f, 0.7132f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.606308f, 0.606308f, 0.155044f, -0.3373f, 0.6145f, 0.7132f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.653224f, 0.982542f, 0.214721f, -0.7194f, -0.1954f, 0.6665f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.606308f, 0.606308f, 0.155044f, -0.7194f, -0.1954f, 0.6665f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.482247f, 0.482247f, 0.252566f, -0.7194f, -0.1954f, 0.6665f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.504213f, 0.740755f, 0.334817f, -0.9866f, -0.1193f, 0.1116f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.482247f, 0.482247f, 0.252566f, -0.9866f, -0.1193f, 0.1116f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467203f, 0.467203f, 0.369460f, -0.9866f, -0.1193f, 0.1116f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.436330f, 0.617440f, 0.436330f, -0.9810f, 0.1929f, 0.0195f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467203f, 0.467203f, 0.369460f, -0.9810f, 0.1929f, 0.0195f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.465606f, 0.465606f, 0.465606f, -0.9810f, 0.1929f, 0.0195f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.219242f, 0.462580f, 0.326643f, 0.6247f, 0.7651f, 0.1561f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.436330f, 0.617440f, 0.436330f, 0.6247f, 0.7651f, 0.1561f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.475932f, 0.613611f, 0.613611f, 0.6247f, 0.7651f, 0.1561f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.145037f, 0.644381f, 0.440683f, -0.6645f, 0.5725f, -0.4803f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.219242f, 0.462580f, 0.326643f, -0.6645f, 0.5725f, -0.4803f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.300945f, 0.581767f, 0.581767f, -0.6645f, 0.5725f, -0.4803f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.798499f, 0.537322f, -0.5198f, -0.0457f, 0.8531f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.145037f, 0.644381f, 0.440683f, -0.5198f, -0.0457f, 0.8531f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.116025f, 0.447827f, 0.447827f, -0.5198f, -0.0457f, 0.8531f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.798499f, 0.537322f, -0.3567f, 0.6189f, 0.6998f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.659898f, 0.659898f, -0.3567f, 0.6189f, 0.6998f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.180243f, 0.708659f, 0.708659f, -0.3567f, 0.6189f, 0.6998f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.121374f, 0.529553f, 0.368511f, 0.4973f, 0.7292f, -0.4700f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.180243f, 0.708659f, 0.708659f, 0.4973f, 0.7292f, -0.4700f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.274601f, 0.527575f, 0.527575f, 0.4973f, 0.7292f, -0.4700f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.313369f, 0.689132f, 0.471260f, 0.9664f, -0.1625f, 0.1990f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.274601f, 0.527575f, 0.527575f, 0.9664f, -0.1625f, 0.1990f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.281453f, 0.346188f, 0.346188f, 0.9664f, -0.1625f, 0.1990f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.317692f, 0.436313f, 0.317692f, -0.7262f, 0.4550f, 0.5153f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.281453f, 0.346188f, 0.346188f, -0.7262f, 0.4550f, 0.5153f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.538827f, 0.538827f, 0.538827f, -0.7262f, 0.4550f, 0.5153f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.487157f, 0.714036f, 0.323716f, -0.4418f, 0.2509f, 0.8613f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.317692f, 0.436313f, 0.317692f, -0.4418f, 0.2509f, 0.8613f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.498710f, 0.498710f, 0.392372f, -0.4418f, 0.2509f, 0.8613f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.481310f, 0.709020f, 0.158357f, -0.1478f, 0.9887f, -0.0248f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.487157f, 0.714036f, 0.323716f, -0.1478f, 0.9887f, -0.0248f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.755718f, 0.755718f, 0.385504f, -0.1478f, 0.9887f, -0.0248f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.483113f, 0.711750f, 0.000000f, 0.5847f, 0.8110f, 0.0206f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.481310f, 0.709020f, 0.158357f, 0.5847f, 0.8110f, 0.0206f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.613645f, 0.613645f, 0.156850f, 0.5847f, 0.8110f, 0.0206f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.713197f, 0.713197f, -0.181360f, -0.4783f, 0.6397f, -0.6017f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.353197f, 0.505187f, -0.116353f, -0.4783f, 0.6397f, -0.6017f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.483113f, 0.711750f, 0.000000f, -0.4783f, 0.6397f, -0.6017f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.462871f, 0.462871f, -0.243147f, 0.4848f, 0.8648f, 0.1307f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.362647f, 0.518982f, -0.242676f, 0.4848f, 0.8648f, 0.1307f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.353197f, 0.505187f, -0.116353f, 0.4848f, 0.8648f, 0.1307f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.583262f, 0.583262f, -0.453862f, 0.2190f, 0.8459f, 0.4863f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.431201f, 0.609609f, -0.431201f, 0.2190f, 0.8459f, 0.4863f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.362647f, 0.518982f, -0.242676f, 0.2190f, 0.8459f, 0.4863f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.382515f, -0.382515f, 0.535280f, 0.9051f, -0.3048f, 0.2964f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.404388f, -0.269844f, 0.584372f, 0.9051f, -0.3048f, 0.2964f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.327745f, -0.327745f, 0.758876f, 0.9051f, -0.3048f, 0.2964f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.404388f, -0.269844f, 0.584372f, 0.8846f, 0.4658f, -0.0218f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313143f, -0.103221f, 0.441460f, 0.8846f, 0.4658f, -0.0218f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.363809f, -0.179730f, 0.863728f, 0.8846f, 0.4658f, -0.0218f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.313143f, -0.103221f, 0.441460f, 0.2401f, -0.9228f, 0.3013f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.445738f, -0.000000f, 0.651939f, 0.2401f, -0.9228f, 0.3013f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.321650f, -0.000000f, 0.750837f, 0.2401f, -0.9228f, 0.3013f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.504643f, 0.166007f, 0.746144f, 0.4968f, -0.5281f, 0.6887f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.361990f, 0.178823f, 0.858877f, 0.4968f, -0.5281f, 0.6887f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.321650f, -0.000000f, 0.750837f, 0.4968f, -0.5281f, 0.6887f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.537106f, 0.356226f, 0.792284f, -0.5949f, 0.7208f, 0.3557f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.239416f, 0.239416f, 0.531072f, -0.5949f, 0.7208f, 0.3557f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.361990f, 0.178823f, 0.858877f, -0.5949f, 0.7208f, 0.3557f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.487306f, 0.487306f, 0.695266f, -0.1864f, -0.4061f, 0.8946f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.288454f, 0.432981f, 0.629165f, -0.1864f, -0.4061f, 0.8946f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.239416f, 0.239416f, 0.531072f, -0.1864f, -0.4061f, 0.8946f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.256950f, -0.384578f, 0.553339f, -0.1749f, -0.9319f, 0.3179f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.327745f, -0.327745f, 0.758876f, -0.1749f, -0.9319f, 0.3179f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.152390f, -0.308959f, 0.717490f, -0.1749f, -0.9319f, 0.3179f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.327745f, -0.327745f, 0.758876f, -0.5279f, -0.4015f, 0.7484f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.363809f, -0.179730f, 0.863728f, -0.5279f, -0.4015f, 0.7484f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.153123f, -0.153123f, 0.729385f, -0.5279f, -0.4015f, 0.7484f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.363809f, -0.179730f, 0.863728f, 0.6589f, 0.5048f, 0.5577f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.321650f, -0.000000f, 0.750837f, 0.6589f, 0.5048f, 0.5577f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185262f, -0.000000f, 0.911990f, 0.6589f, 0.5048f, 0.5577f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.361990f, 0.178823f, 0.858877f, -0.1360f, 0.4032f, 0.9050f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.171598f, 0.171598f, 0.833493f, -0.1360f, 0.4032f, 0.9050f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185262f, -0.000000f, 0.911990f, -0.1360f, 0.4032f, 0.9050f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.239416f, 0.239416f, 0.531072f, 0.9740f, 0.0279f, 0.2247f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170735f, 0.345763f, 0.815613f, 0.9740f, 0.0279f, 0.2247f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.171598f, 0.171598f, 0.833493f, 0.9740f, 0.0279f, 0.2247f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.288454f, 0.432981f, 0.629165f, -0.4011f, 0.9005f, 0.1680f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.123834f, 0.376013f, 0.541488f, -0.4011f, 0.9005f, 0.1680f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170735f, 0.345763f, 0.815613f, -0.4011f, 0.9005f, 0.1680f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.158186f, -0.480788f, 0.708190f, 0.2976f, -0.0416f, 0.9538f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.152390f, -0.308959f, 0.717490f, 0.2976f, -0.0416f, 0.9538f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.326629f, 0.764267f, 0.2976f, -0.0416f, 0.9538f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.152390f, -0.308959f, 0.717490f, 0.0385f, -0.0762f, 0.9963f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.153123f, -0.153123f, 0.729385f, 0.0385f, -0.0762f, 0.9963f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.154457f, 0.735207f, 0.0385f, -0.0762f, 0.9963f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.153123f, -0.153123f, 0.729385f, 0.2071f, -0.7673f, 0.6070f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185262f, -0.000000f, 0.911990f, 0.2071f, -0.7673f, 0.6070f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.000000f, 0.975207f, 0.2071f, -0.7673f, 0.6070f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.171598f, 0.171598f, 0.833493f, -0.5789f, 0.7787f, 0.2419f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.125747f, 0.570440f, -0.5789f, 0.7787f, 0.2419f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.000000f, 0.975207f, -0.5789f, 0.7787f, 0.2419f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.170735f, 0.345763f, 0.815613f, 0.2600f, -0.8016f, 0.5383f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.413812f, 0.999413f, 0.2600f, -0.8016f, 0.5383f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.125747f, 0.570440f, 0.2600f, -0.8016f, 0.5383f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.123834f, 0.376013f, 0.541488f, -0.1218f, 0.9859f, -0.1143f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.357065f, 0.510037f, -0.1218f, 0.9859f, -0.1143f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.413812f, 0.999413f, -0.1218f, 0.9859f, -0.1143f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.326629f, 0.764267f, 0.0574f, -0.9493f, 0.3090f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.163810f, -0.331870f, 0.778574f, 0.0574f, -0.9493f, 0.3090f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.130535f, -0.396451f, 0.574005f, 0.0574f, -0.9493f, 0.3090f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.154457f, 0.735207f, 0.6854f, -0.5045f, 0.5251f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.192247f, -0.192247f, 0.949852f, 0.6854f, -0.5045f, 0.5251f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.163810f, -0.331870f, 0.778574f, 0.6854f, -0.5045f, 0.5251f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.000000f, 0.975207f, 0.0185f, -0.1488f, 0.9887f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.196918f, 0.000000f, 0.978883f, 0.0185f, -0.1488f, 0.9887f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.192247f, -0.192247f, 0.949852f, 0.0185f, -0.1488f, 0.9887f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.000000f, 0.975207f, 0.6843f, 0.6964f, 0.2163f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.125747f, 0.570440f, 0.6843f, 0.6964f, 0.2163f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.194806f, 0.194806f, 0.964270f, 0.6843f, 0.6964f, 0.2163f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.125747f, 0.570440f, -0.8726f, -0.4055f, 0.2723f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.413812f, 0.999413f, -0.8726f, -0.4055f, 0.2723f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.087252f, 0.178279f, 0.369084f, -0.8726f, -0.4055f, 0.2723f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.413812f, 0.999413f, 0.2801f, 0.9536f, -0.1106f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.357065f, 0.510037f, 0.2801f, 0.9536f, -0.1106f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.133444f, 0.405324f, 0.588123f, 0.2801f, 0.9536f, -0.1106f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.163810f, -0.331870f, 0.778574f, 0.3587f, -0.9303f, 0.0770f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.411064f, -0.411064f, 0.973758f, 0.3587f, -0.9303f, 0.0770f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.258315f, -0.386675f, 0.556625f, 0.3587f, -0.9303f, 0.0770f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.192247f, -0.192247f, 0.949852f, -0.0803f, 0.1872f, 0.9790f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.390072f, -0.192820f, 0.933746f, -0.0803f, 0.1872f, 0.9790f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.411064f, -0.411064f, 0.973758f, -0.0803f, 0.1872f, 0.9790f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.196918f, 0.000000f, 0.978883f, -0.6301f, 0.4902f, 0.6022f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.347708f, 0.000000f, 0.821120f, -0.6301f, 0.4902f, 0.6022f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.390072f, -0.192820f, 0.933746f, -0.6301f, 0.4902f, 0.6022f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.196918f, 0.000000f, 0.978883f, -0.8689f, 0.0464f, 0.4927f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.194806f, 0.194806f, 0.964270f, -0.8689f, 0.0464f, 0.4927f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.320007f, 0.157896f, 0.746945f, -0.8689f, 0.0464f, 0.4927f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.194806f, 0.194806f, 0.964270f, 0.3186f, 0.9474f, 0.0313f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.087252f, 0.178279f, 0.369084f, 0.3186f, 0.9474f, 0.0313f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.218973f, 0.218973f, 0.478349f, 0.3186f, 0.9474f, 0.0313f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.087252f, 0.178279f, 0.369084f, 0.0805f, -0.6835f, 0.7255f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.133444f, 0.405324f, 0.588123f, 0.0805f, -0.6835f, 0.7255f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.291068f, 0.436997f, 0.635456f, 0.0805f, -0.6835f, 0.7255f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.411064f, -0.411064f, 0.973758f, -0.2734f, -0.8926f, 0.3584f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.593284f, -0.392791f, 0.880291f, -0.2734f, -0.8926f, 0.3584f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.493679f, -0.493679f, 0.704996f, -0.2734f, -0.8926f, 0.3584f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.390072f, -0.192820f, 0.933746f, 0.2075f, -0.4439f, 0.8717f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.654998f, -0.215302f, 0.985363f, 0.2075f, -0.4439f, 0.8717f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.593284f, -0.392791f, 0.880291f, 0.2075f, -0.4439f, 0.8717f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.347708f, 0.000000f, 0.821120f, 0.3745f, 0.1620f, 0.9130f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.621680f, -0.000000f, 0.933497f, 0.3745f, 0.1620f, 0.9130f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.654998f, -0.215302f, 0.985363f, 0.3745f, 0.1620f, 0.9130f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.347708f, 0.000000f, 0.821120f, 0.5603f, 0.2696f, 0.7832f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.320007f, 0.157896f, 0.746945f, 0.5603f, 0.2696f, 0.7832f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.636257f, 0.209158f, 0.955546f, 0.5603f, 0.2696f, 0.7832f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.320007f, 0.157896f, 0.746945f, 0.4471f, 0.8211f, 0.3549f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.218973f, 0.218973f, 0.478349f, 0.4471f, 0.8211f, 0.3549f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.409826f, 0.273383f, 0.592892f, 0.4471f, 0.8211f, 0.3549f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.218973f, 0.218973f, 0.478349f, 0.2660f, -0.5041f, 0.8216f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.291068f, 0.436997f, 0.635456f, 0.2660f, -0.5041f, 0.8216f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.545989f, 0.545989f, 0.784859f, 0.2660f, -0.5041f, 0.8216f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.349401f, -0.349401f, 0.349401f, 0.8274f, -0.5087f, -0.2380f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.384191f, -0.309091f, 0.384191f, 0.8274f, -0.5087f, -0.2380f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.382515f, -0.382515f, 0.535280f, 0.8274f, -0.5087f, -0.2380f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.384191f, -0.309091f, 0.384191f, -0.3103f, -0.9265f, 0.2130f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.657647f, -0.337831f, 0.657647f, -0.3103f, -0.9265f, 0.2130f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.404388f, -0.269844f, 0.584372f, -0.3103f, -0.9265f, 0.2130f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.657647f, -0.337831f, 0.657647f, -0.6012f, -0.1617f, 0.7826f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.781988f, -0.198297f, 0.781988f, -0.6012f, -0.1617f, 0.7826f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313143f, -0.103221f, 0.441460f, -0.6012f, -0.1617f, 0.7826f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.781988f, -0.198297f, 0.781988f, -0.4032f, -0.0863f, 0.9110f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.815698f, -0.000000f, 0.815698f, -0.4032f, -0.0863f, 0.9110f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.445738f, -0.000000f, 0.651939f, -0.4032f, -0.0863f, 0.9110f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.653571f, 0.166680f, 0.653571f, 0.4405f, -0.5562f, 0.7047f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.504643f, 0.166007f, 0.746144f, 0.4405f, -0.5562f, 0.7047f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.445738f, -0.000000f, 0.651939f, 0.4405f, -0.5562f, 0.7047f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.465944f, 0.244641f, 0.465945f, 0.9774f, -0.1255f, -0.1702f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.537106f, 0.356226f, 0.792284f, 0.9774f, -0.1255f, -0.1702f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.504643f, 0.166007f, 0.746144f, 0.9774f, -0.1255f, -0.1702f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.432840f, 0.344470f, 0.432840f, 0.9478f, 0.1529f, -0.2799f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.487306f, 0.487306f, 0.695266f, 0.9478f, 0.1529f, -0.2799f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.537106f, 0.356226f, 0.792284f, 0.9478f, 0.1529f, -0.2799f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.538827f, 0.538827f, 0.538827f, -0.6230f, 0.7805f, 0.0519f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.281453f, 0.346188f, 0.346188f, -0.6230f, 0.7805f, 0.0519f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.487306f, 0.487306f, 0.695266f, -0.6230f, 0.7805f, 0.0519f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.281453f, 0.346188f, 0.346188f, 0.9946f, 0.0897f, -0.0521f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.274601f, 0.527575f, 0.527575f, 0.9946f, 0.0897f, -0.0521f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.288454f, 0.432981f, 0.629165f, 0.9946f, 0.0897f, -0.0521f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.274601f, 0.527575f, 0.527575f, 0.5213f, -0.4521f, 0.7238f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.180243f, 0.708659f, 0.708659f, 0.5213f, -0.4521f, 0.7238f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.123834f, 0.376013f, 0.541488f, 0.5213f, -0.4521f, 0.7238f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.180243f, 0.708659f, 0.708659f, -0.1216f, -0.4402f, 0.8896f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.659898f, 0.659898f, -0.1216f, -0.4402f, 0.8896f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.357065f, 0.510037f, -0.1216f, -0.4402f, 0.8896f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.357065f, 0.510037f, -0.6375f, -0.3417f, 0.6905f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.659898f, 0.659898f, -0.6375f, -0.3417f, 0.6905f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.116025f, 0.447827f, 0.447827f, -0.6375f, -0.3417f, 0.6905f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.133444f, 0.405324f, 0.588123f, 0.6895f, 0.6649f, 0.2871f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.116025f, 0.447827f, 0.447827f, 0.6895f, 0.6649f, 0.2871f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.300945f, 0.581767f, 0.581767f, 0.6895f, 0.6649f, 0.2871f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.291068f, 0.436997f, 0.635456f, 0.2292f, 0.3522f, 0.9074f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.300945f, 0.581767f, 0.581767f, 0.2292f, 0.3522f, 0.9074f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.475932f, 0.613611f, 0.613611f, 0.2292f, 0.3522f, 0.9074f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.545989f, 0.545989f, 0.784859f, -0.9296f, 0.2261f, -0.2910f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.475932f, 0.613611f, 0.613611f, -0.9296f, 0.2261f, -0.2910f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.465606f, 0.465606f, 0.465606f, -0.9296f, 0.2261f, -0.2910f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.409826f, 0.273383f, 0.592892f, -0.6856f, -0.6158f, 0.3882f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.545989f, 0.545989f, 0.784859f, -0.6856f, -0.6158f, 0.3882f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.576827f, 0.449182f, 0.576827f, -0.6856f, -0.6158f, 0.3882f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.636257f, 0.209158f, 0.955546f, -0.5637f, 0.7987f, -0.2105f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.409826f, 0.273383f, 0.592892f, -0.5637f, 0.7987f, -0.2105f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.418188f, 0.221426f, 0.418188f, -0.5637f, 0.7987f, -0.2105f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.621680f, -0.000000f, 0.933497f, -0.8490f, -0.1136f, 0.5161f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.636257f, 0.209158f, 0.955546f, -0.8490f, -0.1136f, 0.5161f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.755523f, 0.191781f, 0.755523f, -0.8490f, -0.1136f, 0.5161f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.621680f, -0.000000f, 0.933497f, -0.9243f, -0.3507f, 0.1509f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.665454f, -0.000000f, 0.665454f, -0.9243f, -0.3507f, 0.1509f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.614249f, -0.156999f, 0.614249f, -0.9243f, -0.3507f, 0.1509f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.654998f, -0.215302f, 0.985363f, -0.6180f, -0.7634f, -0.1878f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.614249f, -0.156999f, 0.614249f, -0.6180f, -0.7634f, -0.1878f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.462077f, -0.242762f, 0.462077f, -0.6180f, -0.7634f, -0.1878f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.593284f, -0.392791f, 0.880291f, -0.9504f, -0.0311f, -0.3093f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.462077f, -0.242762f, 0.462077f, -0.9504f, -0.0311f, -0.3093f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.456319f, -0.361544f, 0.456319f, -0.9504f, -0.0311f, -0.3093f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.493679f, -0.493679f, 0.704996f, -0.8191f, 0.5484f, 0.1684f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.456319f, -0.361544f, 0.456319f, -0.8191f, 0.5484f, 0.1684f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.574719f, -0.574719f, 0.574719f, -0.8191f, 0.5484f, 0.1684f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.258315f, -0.386675f, 0.556625f, 0.3361f, -0.9316f, -0.1386f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.493679f, -0.493679f, 0.704996f, 0.3361f, -0.9316f, -0.1386f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.303095f, -0.375947f, 0.375947f, 0.3361f, -0.9316f, -0.1386f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.130535f, -0.396451f, 0.574005f, -0.1402f, -0.0775f, 0.9871f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.258315f, -0.386675f, 0.556625f, -0.1402f, -0.0775f, 0.9871f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.281239f, -0.541230f, 0.541230f, -0.1402f, -0.0775f, 0.9871f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.551223f, 0.820744f, -0.7816f, 0.2512f, 0.5710f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.130535f, -0.396451f, 0.574005f, -0.7816f, 0.2512f, 0.5710f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.162303f, -0.635792f, 0.635792f, -0.7816f, 0.2512f, 0.5710f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.134213f, -0.521701f, 0.521700f, 0.4785f, -0.8685f, 0.1290f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.158186f, -0.480788f, 0.708190f, 0.4785f, -0.8685f, 0.1290f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.551223f, 0.820744f, 0.4785f, -0.8685f, 0.1290f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.244147f, -0.464927f, 0.464927f, 0.8562f, -0.4379f, 0.2740f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.256950f, -0.384578f, 0.553339f, 0.8562f, -0.4379f, 0.2740f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.158186f, -0.480788f, 0.708190f, 0.8562f, -0.4379f, 0.2740f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.340510f, -0.427395f, 0.427396f, 0.0672f, -0.9302f, 0.3608f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.382515f, -0.382515f, 0.535280f, 0.0672f, -0.9302f, 0.3608f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.256950f, -0.384578f, 0.553339f, 0.0672f, -0.9302f, 0.3608f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.780100f, -0.542872f, 0.542872f, -0.6581f, 0.6397f, 0.3972f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.598564f, -0.275740f, 0.413447f, -0.6581f, 0.6397f, 0.3972f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.691180f, -0.301497f, 0.301497f, -0.6581f, 0.6397f, 0.3972f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.598564f, -0.275740f, 0.413447f, -0.5515f, 0.0094f, 0.8341f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.598545f, -0.135591f, 0.411875f, -0.5515f, 0.0094f, 0.8341f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.741145f, -0.156812f, 0.317831f, -0.5515f, 0.0094f, 0.8341f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.598545f, -0.135591f, 0.411875f, -0.6200f, -0.6755f, -0.3992f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.846255f, 0.000000f, 0.567164f, -0.6200f, -0.6755f, -0.3992f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.668534f, -0.000000f, 0.291135f, -0.6200f, -0.6755f, -0.3992f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -1.010385f, 0.220458f, 0.670724f, -0.7558f, -0.5437f, -0.3649f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.818719f, 0.171315f, 0.346927f, -0.7558f, -0.5437f, -0.3649f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.668534f, -0.000000f, 0.291135f, -0.7558f, -0.5437f, -0.3649f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.654709f, 0.299067f, 0.449287f, -0.3525f, -0.2662f, 0.8971f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.892758f, 0.379657f, 0.379657f, -0.3525f, -0.2662f, 0.8971f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.818719f, 0.171315f, 0.346927f, -0.3525f, -0.2662f, 0.8971f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.467288f, 0.337980f, 0.337981f, 0.1160f, 0.2142f, 0.9699f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.738815f, 0.502975f, 0.334011f, 0.1160f, 0.2142f, 0.9699f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.892758f, 0.379657f, 0.379657f, 0.1160f, 0.2142f, 0.9699f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.884736f, -0.596122f, 0.394638f, -0.8472f, 0.5140f, -0.1347f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.691180f, -0.301497f, 0.301497f, -0.8472f, 0.5140f, -0.1347f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.656247f, -0.285988f, 0.140940f, -0.8472f, 0.5140f, -0.1347f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.691180f, -0.301497f, 0.301497f, -0.9264f, -0.2933f, -0.2360f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.741145f, -0.156812f, 0.317831f, -0.9264f, -0.2933f, -0.2360f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.700677f, -0.148029f, 0.148029f, -0.9264f, -0.2933f, -0.2360f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.741145f, -0.156812f, 0.317831f, -0.4271f, 0.3404f, 0.8377f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.668534f, -0.000000f, 0.291135f, -0.4271f, 0.3404f, 0.8377f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.885302f, -0.000000f, 0.180611f, -0.4271f, 0.3404f, 0.8377f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.818719f, 0.171315f, 0.346927f, -0.7379f, -0.3001f, 0.6045f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.953661f, 0.192923f, 0.192923f, -0.7379f, -0.3001f, 0.6045f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.885302f, -0.000000f, 0.180611f, -0.7379f, -0.3001f, 0.6045f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.892758f, 0.379657f, 0.379657f, -0.2301f, 0.7246f, -0.6496f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.570152f, 0.253695f, 0.124843f, -0.2301f, 0.7246f, -0.6496f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.953661f, 0.192923f, 0.192923f, -0.2301f, 0.7246f, -0.6496f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.738815f, 0.502975f, 0.334011f, -0.6856f, -0.6806f, 0.2583f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.873176f, 0.584486f, 0.192184f, -0.6856f, -0.6806f, 0.2583f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.570152f, 0.253695f, 0.124843f, -0.6856f, -0.6806f, 0.2583f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.727620f, -0.493001f, 0.162190f, -0.4612f, 0.2465f, 0.8524f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.656247f, -0.285988f, 0.140940f, -0.4612f, 0.2465f, 0.8524f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.981479f, -0.407163f, -0.000000f, -0.4612f, 0.2465f, 0.8524f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.656247f, -0.285988f, 0.140940f, -0.8597f, -0.2982f, 0.4148f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.700677f, -0.148029f, 0.148029f, -0.8597f, -0.2982f, 0.4148f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.767887f, -0.160152f, -0.000000f, -0.8597f, -0.2982f, 0.4148f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.700677f, -0.148029f, 0.148029f, -0.5475f, -0.7599f, 0.3504f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.885302f, -0.000000f, 0.180611f, -0.5475f, -0.7599f, 0.3504f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.000884f, -0.000000f, 0.000000f, -0.5475f, -0.7599f, 0.3504f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.953661f, 0.192923f, 0.192923f, -0.5576f, 0.6512f, -0.5147f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.806074f, 0.166806f, -0.000000f, -0.5576f, 0.6512f, -0.5147f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.000884f, -0.000000f, 0.000000f, -0.5576f, 0.6512f, -0.5147f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.570152f, 0.253695f, 0.124843f, -0.3480f, -0.3207f, 0.8809f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.051441f, 0.433102f, -0.000000f, -0.3480f, -0.3207f, 0.8809f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.806074f, 0.166806f, -0.000000f, -0.3480f, -0.3207f, 0.8809f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.873176f, 0.584486f, 0.192184f, 0.0864f, 0.7421f, -0.6647f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.535651f, 0.373071f, -0.000000f, 0.0864f, 0.7421f, -0.6647f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.051441f, 0.433102f, -0.000000f, 0.0864f, 0.7421f, -0.6647f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.981479f, -0.407163f, -0.000000f, -0.3082f, -0.1021f, -0.9458f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.616712f, -0.271159f, -0.133548f, -0.3082f, -0.1021f, -0.9458f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.579688f, -0.400023f, -0.131706f, -0.3082f, -0.1021f, -0.9458f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.767887f, -0.160152f, -0.000000f, -0.6015f, -0.7987f, -0.0170f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.764446f, -0.159345f, -0.159345f, -0.6015f, -0.7987f, -0.0170f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.616712f, -0.271159f, -0.133548f, -0.6015f, -0.7987f, -0.0170f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -1.000884f, -0.000000f, 0.000000f, -0.6685f, -0.6708f, -0.3211f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.911905f, 0.000000f, -0.185247f, -0.6685f, -0.6708f, -0.3211f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.764446f, -0.159345f, -0.159345f, -0.6685f, -0.6708f, -0.3211f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -1.000884f, -0.000000f, 0.000000f, -0.5535f, 0.6464f, -0.5251f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.806074f, 0.166806f, -0.000000f, -0.5535f, 0.6464f, -0.5251f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.691145f, 0.146337f, -0.146337f, -0.5535f, 0.6464f, -0.5251f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.806074f, 0.166806f, -0.000000f, -0.3502f, -0.3227f, -0.8793f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.051441f, 0.433102f, -0.000000f, -0.3502f, -0.3227f, -0.8793f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.572381f, 0.254531f, -0.125260f, -0.3502f, -0.3227f, -0.8793f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -1.051441f, 0.433102f, -0.000000f, 0.1068f, 0.9177f, 0.3825f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.535651f, 0.373071f, -0.000000f, 0.1068f, 0.9177f, 0.3825f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.657275f, 0.448787f, -0.147694f, 0.1068f, 0.9177f, 0.3825f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.616712f, -0.271159f, -0.133548f, -0.6184f, -0.3569f, 0.7002f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.816309f, -0.350015f, -0.350014f, -0.6184f, -0.3569f, 0.7002f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.706616f, -0.482421f, -0.320633f, -0.6184f, -0.3569f, 0.7002f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.764446f, -0.159345f, -0.159345f, -0.8243f, -0.2722f, 0.4964f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.884195f, -0.183556f, -0.371486f, -0.8243f, -0.2722f, 0.4964f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.816309f, -0.350015f, -0.350014f, -0.8243f, -0.2722f, 0.4964f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.911905f, 0.000000f, -0.185247f, -0.2786f, 0.6630f, -0.6949f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.657742f, -0.000000f, -0.287134f, -0.2786f, 0.6630f, -0.6949f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.884195f, -0.183556f, -0.371486f, -0.2786f, 0.6630f, -0.6949f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.911905f, 0.000000f, -0.185247f, -0.5120f, 0.6123f, 0.6024f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.691145f, 0.146337f, -0.146337f, -0.5120f, 0.6123f, 0.6024f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.919143f, 0.190090f, -0.384594f, -0.5120f, 0.6123f, 0.6024f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.691145f, 0.146337f, -0.146337f, -0.5624f, 0.4872f, 0.6681f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.572381f, 0.254531f, -0.125260f, -0.5624f, 0.4872f, 0.6681f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.748195f, 0.323604f, -0.323604f, -0.5624f, 0.4872f, 0.6681f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.572381f, 0.254531f, -0.125260f, -0.8626f, -0.4110f, -0.2949f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.657275f, 0.448787f, -0.147694f, -0.8626f, -0.4110f, -0.2949f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.596287f, 0.411994f, -0.274794f, -0.8626f, -0.4110f, -0.2949f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.816309f, -0.350015f, -0.350014f, -0.2181f, -0.6092f, -0.7625f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.644876f, -0.294982f, -0.443010f, -0.2181f, -0.6092f, -0.7625f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.550241f, -0.392314f, -0.392315f, -0.2181f, -0.6092f, -0.7625f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.884195f, -0.183556f, -0.371486f, -0.3923f, -0.9132f, 0.1100f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.912052f, -0.200195f, -0.608920f, -0.3923f, -0.9132f, 0.1100f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.644876f, -0.294982f, -0.443010f, -0.3923f, -0.9132f, 0.1100f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.657742f, -0.000000f, -0.287134f, -0.8289f, 0.2433f, 0.5037f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.816676f, 0.000000f, -0.548680f, -0.8289f, 0.2433f, 0.5037f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.912052f, -0.200195f, -0.608920f, -0.8289f, 0.2433f, 0.5037f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.657742f, -0.000000f, -0.287134f, -0.6071f, -0.7897f, 0.0882f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.919143f, 0.190090f, -0.384594f, -0.6071f, -0.7897f, 0.0882f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.994485f, 0.217182f, -0.660731f, -0.6071f, -0.7897f, 0.0882f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.919143f, 0.190090f, -0.384594f, -0.2738f, 0.6671f, -0.6929f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.748195f, 0.323604f, -0.323604f, -0.2738f, 0.6671f, -0.6929f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.609280f, 0.280192f, -0.420287f, -0.2738f, 0.6671f, -0.6929f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.748195f, 0.323604f, -0.323604f, -0.4279f, 0.8702f, -0.2441f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.596287f, 0.411994f, -0.274794f, -0.4279f, 0.8702f, -0.2441f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.557279f, 0.396925f, -0.396925f, -0.4279f, 0.8702f, -0.2441f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.574719f, -0.574719f, 0.574719f, -0.0546f, 0.5078f, 0.8597f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.456319f, -0.361544f, 0.456319f, -0.0546f, 0.5078f, 0.8597f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.780100f, -0.542872f, 0.542872f, -0.0546f, 0.5078f, 0.8597f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.456319f, -0.361544f, 0.456319f, -0.3218f, -0.0614f, 0.9448f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.462077f, -0.242762f, 0.462077f, -0.3218f, -0.0614f, 0.9448f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.598564f, -0.275740f, 0.413447f, -0.3218f, -0.0614f, 0.9448f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.462077f, -0.242762f, 0.462077f, -0.5823f, -0.8025f, -0.1301f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.614249f, -0.156999f, 0.614249f, -0.5823f, -0.8025f, -0.1301f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.598545f, -0.135591f, 0.411875f, -0.5823f, -0.8025f, -0.1301f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.614249f, -0.156999f, 0.614249f, -0.4368f, -0.4045f, 0.8035f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.665454f, -0.000000f, 0.665454f, -0.4368f, -0.4045f, 0.8035f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.846255f, 0.000000f, 0.567164f, -0.4368f, -0.4045f, 0.8035f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.755523f, 0.191781f, 0.755523f, -0.3145f, -0.5852f, 0.7474f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.010385f, 0.220458f, 0.670724f, -0.3145f, -0.5852f, 0.7474f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.846255f, 0.000000f, 0.567164f, -0.3145f, -0.5852f, 0.7474f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.418188f, 0.221426f, 0.418188f, 0.3020f, 0.6354f, 0.7107f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.654709f, 0.299067f, 0.449287f, 0.3020f, 0.6354f, 0.7107f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -1.010385f, 0.220458f, 0.670724f, 0.3020f, 0.6354f, 0.7107f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.576827f, 0.449182f, 0.576827f, -0.4662f, 0.7007f, -0.5400f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467288f, 0.337980f, 0.337981f, -0.4662f, 0.7007f, -0.5400f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.654709f, 0.299067f, 0.449287f, -0.4662f, 0.7007f, -0.5400f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.465606f, 0.465606f, 0.465606f, -0.9999f, -0.0034f, 0.0165f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467203f, 0.467203f, 0.369460f, -0.9999f, -0.0034f, 0.0165f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467288f, 0.337980f, 0.337981f, -0.9999f, -0.0034f, 0.0165f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.467203f, 0.467203f, 0.369460f, 0.1153f, 0.9870f, 0.1122f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.482247f, 0.482247f, 0.252566f, 0.1153f, 0.9870f, 0.1122f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.738815f, 0.502975f, 0.334011f, 0.1153f, 0.9870f, 0.1122f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.482247f, 0.482247f, 0.252566f, 0.0525f, 0.6491f, 0.7589f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.606308f, 0.606308f, 0.155044f, 0.0525f, 0.6491f, 0.7589f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.873176f, 0.584486f, 0.192184f, 0.0525f, 0.6491f, 0.7589f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.606308f, 0.606308f, 0.155044f, -0.8429f, -0.4507f, 0.2939f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.722475f, 0.722475f, 0.000000f, -0.8429f, -0.4507f, 0.2939f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.535651f, 0.373071f, -0.000000f, -0.8429f, -0.4507f, 0.2939f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.535651f, 0.373071f, -0.000000f, -0.8796f, -0.4703f, 0.0714f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.722475f, 0.722475f, 0.000000f, -0.8796f, -0.4703f, 0.0714f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.755952f, 0.755952f, -0.191887f, -0.8796f, -0.4703f, 0.0714f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.657275f, 0.448787f, -0.147694f, -0.7595f, -0.3251f, -0.5634f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.755952f, 0.755952f, -0.191887f, -0.7595f, -0.3251f, -0.5634f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.601862f, 0.601862f, -0.310713f, -0.7595f, -0.3251f, -0.5634f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.596287f, 0.411994f, -0.274794f, -0.5876f, -0.1670f, -0.7917f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.601862f, 0.601862f, -0.310713f, -0.5876f, -0.1670f, -0.7917f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.477370f, 0.477370f, -0.376854f, -0.5876f, -0.1670f, -0.7917f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.557279f, 0.396925f, -0.396925f, 0.3537f, -0.1199f, -0.9276f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.477370f, 0.477370f, -0.376854f, 0.3537f, -0.1199f, -0.9276f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.271962f, 0.271962f, -0.271962f, 0.3537f, -0.1199f, -0.9276f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.609280f, 0.280192f, -0.420287f, -0.2027f, 0.2782f, -0.9389f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.557279f, 0.396925f, -0.396925f, -0.2027f, 0.2782f, -0.9389f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.437378f, 0.347770f, -0.437378f, -0.2027f, 0.2782f, -0.9389f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.994485f, 0.217182f, -0.660731f, -0.3423f, 0.8847f, 0.3166f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.609280f, 0.280192f, -0.420287f, -0.3423f, 0.8847f, 0.3166f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.674306f, 0.345929f, -0.674306f, -0.3423f, 0.8847f, 0.3166f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.816676f, 0.000000f, -0.548680f, -0.1296f, -0.5364f, -0.8340f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.994485f, 0.217182f, -0.660731f, -0.1296f, -0.5364f, -0.8340f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.681304f, 0.173508f, -0.681304f, -0.1296f, -0.5364f, -0.8340f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.816676f, 0.000000f, -0.548680f, 0.2694f, 0.5912f, -0.7602f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.401631f, 0.000000f, -0.401631f, 0.2694f, 0.5912f, -0.7602f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.580775f, -0.148757f, -0.580775f, 0.2694f, 0.5912f, -0.7602f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.912052f, -0.200195f, -0.608920f, 0.1650f, -0.6634f, -0.7298f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.580775f, -0.148757f, -0.580775f, 0.1650f, -0.6634f, -0.7298f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467353f, -0.245326f, -0.467352f, 0.1650f, -0.6634f, -0.7298f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.644876f, -0.294982f, -0.443010f, -0.2752f, 0.6261f, -0.7296f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467353f, -0.245326f, -0.467352f, -0.2752f, 0.6261f, -0.7296f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.608913f, -0.472516f, -0.608913f, -0.2752f, 0.6261f, -0.7296f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.550241f, -0.392314f, -0.392315f, -0.2773f, -0.8741f, 0.3988f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.608913f, -0.472516f, -0.608913f, -0.2773f, -0.8741f, 0.3988f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.450496f, -0.450496f, -0.450496f, -0.2773f, -0.8741f, 0.3988f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.706616f, -0.482421f, -0.320633f, 0.3493f, -0.8743f, -0.3370f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.550241f, -0.392314f, -0.392315f, 0.3493f, -0.8743f, -0.3370f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.354508f, -0.354508f, -0.287505f, 0.3493f, -0.8743f, -0.3370f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.579688f, -0.400023f, -0.131706f, -0.7977f, -0.1243f, 0.5901f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.706616f, -0.482421f, -0.320633f, -0.7977f, -0.1243f, 0.5901f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.700848f, -0.700848f, -0.358831f, -0.7977f, -0.1243f, 0.5901f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.990879f, -0.657537f, -0.000000f, -0.5101f, 0.4351f, -0.7419f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.579688f, -0.400023f, -0.131706f, -0.5101f, 0.4351f, -0.7419f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.834707f, -0.834707f, -0.211277f, -0.5101f, 0.4351f, -0.7419f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.455396f, -0.455396f, 0.117888f, 0.1993f, -0.8309f, 0.5194f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.727620f, -0.493001f, 0.162190f, 0.1993f, -0.8309f, 0.5194f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.990879f, -0.657537f, -0.000000f, 0.1993f, -0.8309f, 0.5194f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.542685f, -0.542685f, 0.281946f, 0.0119f, -0.9170f, -0.3988f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.884736f, -0.596122f, 0.394638f, 0.0119f, -0.9170f, -0.3988f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.727620f, -0.493001f, 0.162190f, 0.0119f, -0.9170f, -0.3988f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.569328f, -0.569328f, 0.443728f, 0.0309f, -0.9474f, 0.3185f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.780100f, -0.542872f, 0.542872f, 0.0309f, -0.9474f, 0.3185f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.884736f, -0.596122f, 0.394638f, 0.0309f, -0.9474f, 0.3185f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.547855f, -0.787708f, -0.547855f, -0.3264f, -0.7480f, 0.5778f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.398933f, -0.895074f, -0.602721f, -0.3264f, -0.7480f, 0.5778f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.307622f, -0.706978f, -0.307622f, -0.3264f, -0.7480f, 0.5778f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.398933f, -0.895074f, -0.602721f, 0.5489f, -0.7920f, -0.2672f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.169974f, -0.765397f, -0.516744f, 0.5489f, -0.7920f, -0.2672f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.172235f, -0.823641f, -0.348774f, 0.5489f, -0.7920f, -0.2672f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.169974f, -0.765397f, -0.516744f, 0.1180f, -0.9353f, -0.3336f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.747885f, -0.505694f, 0.1180f, -0.9353f, -0.3336f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.806215f, -0.342182f, 0.1180f, -0.9353f, -0.3336f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.102127f, -0.436152f, -0.309807f, 0.6227f, -0.1040f, -0.7755f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.130487f, -0.600341f, -0.265019f, 0.6227f, -0.1040f, -0.7755f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.806215f, -0.342182f, 0.6227f, -0.1040f, -0.7755f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.347593f, -0.771503f, -0.523841f, -0.6360f, -0.7714f, -0.0233f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.328976f, -0.762050f, -0.328976f, -0.6360f, -0.7714f, -0.0233f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.130487f, -0.600341f, -0.265019f, -0.6360f, -0.7714f, -0.0233f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.410149f, -0.577469f, -0.410149f, 0.9229f, -0.3806f, 0.0574f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.400765f, -0.578697f, -0.267486f, 0.9229f, -0.3806f, 0.0574f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.328976f, -0.762050f, -0.328976f, 0.9229f, -0.3806f, 0.0574f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.668695f, -0.998427f, -0.441874f, 0.6555f, -0.7376f, -0.1618f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.307622f, -0.706978f, -0.307622f, 0.6555f, -0.7376f, -0.1618f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.322062f, -0.752426f, -0.158921f, 0.6555f, -0.7376f, -0.1618f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.307622f, -0.706978f, -0.307622f, -0.5975f, -0.4623f, -0.6552f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.172235f, -0.823641f, -0.348774f, -0.5975f, -0.4623f, -0.6552f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.200605f, -0.996950f, -0.200605f, -0.5975f, -0.4623f, -0.6552f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.172235f, -0.823641f, -0.348774f, 0.0805f, -0.4610f, -0.8837f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.806215f, -0.342182f, 0.0805f, -0.4610f, -0.8837f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -1.058182f, -0.210736f, 0.0805f, -0.4610f, -0.8837f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.130487f, -0.600341f, -0.265019f, 0.7457f, -0.2840f, -0.6027f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.157842f, -0.755973f, -0.157842f, 0.7457f, -0.2840f, -0.6027f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -1.058182f, -0.210736f, 0.7457f, -0.2840f, -0.6027f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.328976f, -0.762050f, -0.328976f, 0.1637f, -0.9664f, 0.1980f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313266f, -0.728973f, -0.154536f, 0.1637f, -0.9664f, 0.1980f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.157842f, -0.755973f, -0.157842f, 0.1637f, -0.9664f, 0.1980f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.400765f, -0.578697f, -0.267486f, 0.7970f, -0.0102f, 0.6039f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.267683f, -0.369131f, -0.088317f, 0.7970f, -0.0102f, 0.6039f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.313266f, -0.728973f, -0.154536f, 0.7970f, -0.0102f, 0.6039f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.432186f, -0.630861f, -0.142251f, -0.5297f, -0.3755f, -0.7606f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.322062f, -0.752426f, -0.158921f, -0.5297f, -0.3755f, -0.7606f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.400537f, -0.963609f, 0.000000f, -0.5297f, -0.3755f, -0.7606f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.322062f, -0.752426f, -0.158921f, -0.5729f, -0.4060f, 0.7120f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.200605f, -0.996950f, -0.200605f, -0.5729f, -0.4060f, 0.7120f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.150966f, -0.715172f, 0.000000f, -0.5729f, -0.4060f, 0.7120f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.200605f, -0.996950f, -0.200605f, -0.1955f, -0.7459f, 0.6367f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -1.058182f, -0.210736f, -0.1955f, -0.7459f, 0.6367f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.878321f, -0.000000f, -0.1955f, -0.7459f, 0.6367f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.157842f, -0.755973f, -0.157842f, -0.3349f, -0.5526f, -0.7632f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.200575f, -0.999869f, -0.000000f, -0.3349f, -0.5526f, -0.7632f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.878321f, -0.000000f, -0.3349f, -0.5526f, -0.7632f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.313266f, -0.728973f, -0.154536f, 0.8981f, -0.4290f, -0.0971f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.320722f, -0.748334f, -0.000000f, 0.8981f, -0.4290f, -0.0971f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.200575f, -0.999869f, -0.000000f, 0.8981f, -0.4290f, -0.0971f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.267683f, -0.369131f, -0.088317f, -0.0376f, -0.2317f, -0.9721f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.526876f, -0.781782f, 0.000000f, -0.0376f, -0.2317f, -0.9721f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.320722f, -0.748334f, -0.000000f, -0.0376f, -0.2317f, -0.9721f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.400537f, -0.963609f, 0.000000f, -0.1221f, -0.3611f, 0.9245f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.282534f, -0.647040f, 0.139218f, -0.1221f, -0.3611f, 0.9245f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.416949f, -0.606618f, 0.137255f, -0.1221f, -0.3611f, 0.9245f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.150966f, -0.715172f, 0.000000f, -0.7507f, -0.4365f, -0.4959f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.178216f, -0.870786f, 0.178216f, -0.7507f, -0.4365f, -0.4959f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.282534f, -0.647040f, 0.139218f, -0.7507f, -0.4365f, -0.4959f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.878321f, -0.000000f, 0.1382f, -0.9740f, 0.1794f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.846304f, 0.173816f, 0.1382f, -0.9740f, 0.1794f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.178216f, -0.870786f, 0.178216f, 0.1382f, -0.9740f, 0.1794f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.878321f, -0.000000f, -0.2680f, -0.4422f, 0.8559f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.200575f, -0.999869f, -0.000000f, -0.2680f, -0.4422f, 0.8559f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.145275f, -0.685160f, 0.145275f, -0.2680f, -0.4422f, 0.8559f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.200575f, -0.999869f, -0.000000f, 0.7874f, -0.3761f, -0.4884f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.320722f, -0.748334f, -0.000000f, 0.7874f, -0.3761f, -0.4884f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.370563f, -0.881734f, 0.183096f, 0.7874f, -0.3761f, -0.4884f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.320722f, -0.748334f, -0.000000f, -0.1555f, -0.9582f, -0.2402f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.526876f, -0.781782f, 0.000000f, -0.1555f, -0.9582f, -0.2402f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.559328f, -0.833150f, 0.183936f, -0.1555f, -0.9582f, -0.2402f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.282534f, -0.647040f, 0.139218f, -0.1076f, -0.7310f, -0.6738f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.358383f, -0.837893f, 0.358383f, -0.1076f, -0.7310f, -0.6738f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.552658f, -0.816648f, 0.366349f, -0.1076f, -0.7310f, -0.6738f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.178216f, -0.870786f, 0.178216f, 0.0021f, -0.9834f, 0.1816f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.174969f, -0.838261f, 0.354257f, 0.0021f, -0.9834f, 0.1816f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.358383f, -0.837893f, 0.358383f, 0.0021f, -0.9834f, 0.1816f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.846304f, 0.173816f, -0.2031f, -0.9670f, -0.1538f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.877283f, 0.368531f, -0.2031f, -0.9670f, -0.1538f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.174969f, -0.838261f, 0.354257f, -0.2031f, -0.9670f, -0.1538f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.846304f, 0.173816f, 0.7432f, -0.6691f, 0.0052f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.145275f, -0.685160f, 0.145275f, 0.7432f, -0.6691f, 0.0052f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.145980f, -0.683205f, 0.296099f, 0.7432f, -0.6691f, 0.0052f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.145275f, -0.685160f, 0.145275f, -0.6632f, -0.7226f, 0.1947f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.370563f, -0.881734f, 0.183096f, -0.6632f, -0.7226f, 0.1947f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.350838f, -0.818433f, 0.350838f, -0.6632f, -0.7226f, 0.1947f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.370563f, -0.881734f, 0.183096f, 0.0826f, -0.3372f, 0.9378f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.559328f, -0.833150f, 0.183936f, 0.0826f, -0.3372f, 0.9378f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.417581f, -0.605040f, 0.278431f, 0.0826f, -0.3372f, 0.9378f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.358383f, -0.837893f, 0.358383f, -0.1948f, -0.9640f, 0.1812f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.360523f, -0.802626f, 0.543708f, -0.1948f, -0.9640f, 0.1812f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.535477f, -0.768810f, 0.535477f, -0.1948f, -0.9640f, 0.1812f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.174969f, -0.838261f, 0.354257f, -0.3713f, -0.9083f, -0.1928f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.193682f, -0.880448f, 0.589056f, -0.3713f, -0.9083f, -0.1928f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.360523f, -0.802626f, 0.543708f, -0.3713f, -0.9083f, -0.1928f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.877283f, 0.368531f, -0.2455f, -0.9419f, -0.2292f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.939839f, 0.625643f, -0.2455f, -0.9419f, -0.2292f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.193682f, -0.880448f, 0.589056f, -0.2455f, -0.9419f, -0.2292f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.877283f, 0.368531f, 0.6275f, -0.6384f, -0.4458f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.145980f, -0.683205f, 0.296099f, 0.6275f, -0.6384f, -0.4458f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.183011f, -0.828663f, 0.556508f, 0.6275f, -0.6384f, -0.4458f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.145980f, -0.683205f, 0.296099f, -0.5801f, -0.7565f, 0.3021f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.350838f, -0.818433f, 0.350838f, -0.5801f, -0.7565f, 0.3021f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.336655f, -0.745179f, 0.507037f, -0.5801f, -0.7565f, 0.3021f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.350838f, -0.818433f, 0.350838f, 0.7048f, 0.0201f, 0.7091f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.417581f, -0.605040f, 0.278431f, 0.7048f, 0.0201f, 0.7091f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.346010f, -0.479546f, 0.346010f, 0.7048f, 0.0201f, 0.7091f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.450496f, -0.450496f, -0.450496f, -0.8644f, 0.3530f, -0.3582f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.449732f, -0.577583f, -0.577583f, -0.8644f, 0.3530f, -0.3582f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.547855f, -0.787708f, -0.547855f, -0.8644f, 0.3530f, -0.3582f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.449732f, -0.577583f, -0.577583f, 0.4123f, 0.1373f, -0.9007f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.245217f, -0.467128f, -0.467128f, 0.4123f, 0.1373f, -0.9007f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.398933f, -0.895074f, -0.602721f, 0.4123f, 0.1373f, -0.9007f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.245217f, -0.467128f, -0.467128f, -0.7980f, -0.1025f, -0.5938f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.148160f, -0.578352f, -0.578352f, -0.7980f, -0.1025f, -0.5938f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.169974f, -0.765397f, -0.516744f, -0.7980f, -0.1025f, -0.5938f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.148160f, -0.578352f, -0.578352f, -0.3192f, -0.5951f, -0.7375f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.613845f, -0.613845f, -0.3192f, -0.5951f, -0.7375f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.747885f, -0.505694f, -0.3192f, -0.5951f, -0.7375f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.174764f, -0.686407f, -0.686407f, 0.6916f, -0.5329f, 0.4875f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.102127f, -0.436152f, -0.309807f, 0.6916f, -0.5329f, 0.4875f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.747885f, -0.505694f, 0.6916f, -0.5329f, 0.4875f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.329947f, -0.641428f, -0.641428f, -0.8497f, -0.4123f, -0.3286f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.347593f, -0.771503f, -0.523841f, -0.8497f, -0.4123f, -0.3286f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.102127f, -0.436152f, -0.309807f, -0.8497f, -0.4123f, -0.3286f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.362015f, -0.456966f, -0.456966f, 0.8003f, 0.0894f, -0.5929f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.410149f, -0.577469f, -0.410149f, 0.8003f, 0.0894f, -0.5929f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.347593f, -0.771503f, -0.523841f, 0.8003f, 0.0894f, -0.5929f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.472812f, -0.472812f, -0.472812f, -0.2712f, -0.3697f, -0.8887f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.591253f, -0.591253f, -0.459673f, -0.2712f, -0.3697f, -0.8887f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.410149f, -0.577469f, -0.410149f, -0.2712f, -0.3697f, -0.8887f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.591253f, -0.591253f, -0.459673f, 0.5015f, -0.6750f, 0.5412f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.499946f, -0.499946f, -0.261170f, 0.5015f, -0.6750f, 0.5412f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.400765f, -0.578697f, -0.267486f, 0.5015f, -0.6750f, 0.5412f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.499946f, -0.499946f, -0.261170f, -0.5958f, -0.7738f, -0.2149f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.623072f, -0.623072f, -0.159171f, -0.5958f, -0.7738f, -0.2149f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.267683f, -0.369131f, -0.088317f, -0.5958f, -0.7738f, -0.2149f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.623072f, -0.623072f, -0.159171f, 0.8333f, -0.5509f, -0.0457f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.628324f, -0.628324f, 0.000000f, 0.8333f, -0.5509f, -0.0457f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.526876f, -0.781782f, 0.000000f, 0.8333f, -0.5509f, -0.0457f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.526876f, -0.781782f, 0.000000f, 0.7788f, -0.5148f, -0.3584f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.628324f, -0.628324f, 0.000000f, 0.7788f, -0.5148f, -0.3584f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.676028f, -0.676028f, 0.172209f, 0.7788f, -0.5148f, -0.3584f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.559328f, -0.833150f, 0.183936f, 0.5766f, -0.3741f, 0.7264f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.676028f, -0.676028f, 0.172209f, 0.5766f, -0.3741f, 0.7264f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.578778f, -0.578778f, 0.299491f, 0.5766f, -0.3741f, 0.7264f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.417581f, -0.605040f, 0.278431f, -0.0717f, -0.3183f, 0.9453f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.578778f, -0.578778f, 0.299491f, -0.0717f, -0.3183f, 0.9453f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.425849f, -0.425849f, 0.339386f, -0.0717f, -0.3183f, 0.9453f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.346010f, -0.479546f, 0.346010f, 0.1017f, -0.0286f, 0.9944f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.425849f, -0.425849f, 0.339386f, 0.1017f, -0.0286f, 0.9944f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.349401f, -0.349401f, 0.349401f, 0.1017f, -0.0286f, 0.9944f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.336655f, -0.745179f, 0.507037f, 0.9979f, 0.0041f, 0.0648f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.346010f, -0.479546f, 0.346010f, 0.9979f, 0.0041f, 0.0648f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.340510f, -0.427395f, 0.427396f, 0.9979f, 0.0041f, 0.0648f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.183011f, -0.828663f, 0.556508f, 0.1953f, 0.2085f, 0.9583f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.336655f, -0.745179f, 0.507037f, 0.1953f, 0.2085f, 0.9583f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.244147f, -0.464927f, 0.464927f, 0.1953f, 0.2085f, 0.9583f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.939839f, 0.625643f, 0.2680f, 0.1505f, 0.9516f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.183011f, -0.828663f, 0.556508f, 0.2680f, 0.1505f, 0.9516f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.134213f, -0.521701f, 0.521700f, 0.2680f, 0.1505f, 0.9516f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.939839f, 0.625643f, -0.0117f, -0.0396f, 0.9991f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.637618f, 0.637618f, -0.0117f, -0.0396f, 0.9991f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.162303f, -0.635792f, 0.635792f, -0.0117f, -0.0396f, 0.9991f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.193682f, -0.880448f, 0.589056f, -0.6505f, -0.0612f, 0.7570f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.162303f, -0.635792f, 0.635792f, -0.6505f, -0.0612f, 0.7570f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.281239f, -0.541230f, 0.541230f, -0.6505f, -0.0612f, 0.7570f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.360523f, -0.802626f, 0.543708f, -0.8812f, 0.2709f, 0.3875f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.281239f, -0.541230f, 0.541230f, -0.8812f, 0.2709f, 0.3875f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.303095f, -0.375947f, 0.375947f, -0.8812f, 0.2709f, 0.3875f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.535477f, -0.768810f, 0.535477f, 0.6079f, -0.0375f, 0.7932f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.303095f, -0.375947f, 0.375947f, 0.6079f, -0.0375f, 0.7932f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.574719f, -0.574719f, 0.574719f, 0.6079f, -0.0375f, 0.7932f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.552658f, -0.816648f, 0.366349f, -0.9856f, -0.1072f, 0.1305f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.535477f, -0.768810f, 0.535477f, -0.9856f, -0.1072f, 0.1305f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.569328f, -0.569328f, 0.443728f, -0.9856f, -0.1072f, 0.1305f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.416949f, -0.606618f, 0.137255f, -0.7789f, -0.1586f, -0.6068f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.552658f, -0.816648f, 0.366349f, -0.7789f, -0.1586f, -0.6068f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.542685f, -0.542685f, 0.281946f, -0.7789f, -0.1586f, -0.6068f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.465203f, -0.683089f, 0.000000f, -0.8944f, -0.1747f, 0.4118f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.416949f, -0.606618f, 0.137255f, -0.8944f, -0.1747f, 0.4118f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.455396f, -0.455396f, 0.117888f, -0.8944f, -0.1747f, 0.4118f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.834707f, -0.834707f, -0.211277f, 0.4700f, -0.8583f, -0.2060f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.432186f, -0.630861f, -0.142251f, 0.4700f, -0.8583f, -0.2060f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.465203f, -0.683089f, 0.000000f, 0.4700f, -0.8583f, -0.2060f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.700848f, -0.700848f, -0.358831f, -0.5578f, -0.2785f, 0.7819f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.668695f, -0.998427f, -0.441874f, -0.5578f, -0.2785f, 0.7819f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.432186f, -0.630861f, -0.142251f, -0.5578f, -0.2785f, 0.7819f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.354508f, -0.354508f, -0.287505f, -0.8842f, 0.4558f, -0.1018f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.547855f, -0.787708f, -0.547855f, -0.8842f, 0.4558f, -0.1018f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.668695f, -0.998427f, -0.441874f, -0.8842f, 0.4558f, -0.1018f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.467835f, -0.338339f, -0.338339f, -0.1139f, -0.9890f, -0.0949f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.793392f, -0.356687f, -0.537814f, -0.1139f, -0.9890f, -0.0949f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.904531f, -0.384222f, -0.384222f, -0.1139f, -0.9890f, -0.0949f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.793392f, -0.356687f, -0.537814f, 0.8127f, 0.0132f, 0.5825f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.783441f, -0.173692f, -0.528085f, 0.8127f, 0.0132f, 0.5825f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.591866f, -0.128903f, -0.261840f, 0.8127f, 0.0132f, 0.5825f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.783441f, -0.173692f, -0.528085f, 0.6334f, -0.0286f, 0.7733f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.815583f, -0.000000f, -0.547997f, 0.6334f, -0.0286f, 0.7733f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.364271f, -0.000000f, -0.178327f, 0.6334f, -0.0286f, 0.7733f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.688762f, 0.154182f, -0.468578f, 0.2070f, -0.9408f, -0.2684f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.984669f, 0.202341f, -0.409171f, 0.2070f, -0.9408f, -0.2684f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.364271f, -0.000000f, -0.178327f, 0.2070f, -0.9408f, -0.2684f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.850258f, 0.380313f, -0.574113f, 0.8655f, 0.2527f, -0.4326f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.934747f, 0.395938f, -0.395938f, 0.8655f, 0.2527f, -0.4326f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.984669f, 0.202341f, -0.409171f, 0.8655f, 0.2527f, -0.4326f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.860198f, 0.595336f, -0.595336f, 0.5874f, 0.6714f, 0.4518f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.757225f, 0.514726f, -0.341660f, 0.5874f, 0.6714f, 0.4518f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.934747f, 0.395938f, -0.395938f, 0.5874f, 0.6714f, 0.4518f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.760581f, -0.516869f, -0.343055f, 0.6198f, -0.4800f, 0.6208f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.904531f, -0.384222f, -0.384222f, 0.6198f, -0.4800f, 0.6208f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.729381f, -0.313419f, -0.154613f, 0.6198f, -0.4800f, 0.6208f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.904531f, -0.384222f, -0.384222f, 0.4006f, 0.7458f, -0.5323f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.591866f, -0.128903f, -0.261840f, 0.4006f, 0.7458f, -0.5323f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.786823f, -0.163316f, -0.163316f, 0.4006f, 0.7458f, -0.5323f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.591866f, -0.128903f, -0.261840f, 0.0433f, 0.5959f, -0.8019f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.364271f, -0.000000f, -0.178327f, 0.0433f, 0.5959f, -0.8019f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.752095f, 0.000000f, -0.157400f, 0.0433f, 0.5959f, -0.8019f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.984669f, 0.202341f, -0.409171f, 0.7841f, -0.1526f, 0.6016f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.788746f, 0.163657f, -0.163657f, 0.7841f, -0.1526f, 0.6016f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.752095f, 0.000000f, -0.157400f, 0.7841f, -0.1526f, 0.6016f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.934747f, 0.395938f, -0.395938f, 0.3278f, 0.5570f, 0.7631f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.549565f, 0.245974f, -0.120995f, 0.3278f, 0.5570f, 0.7631f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.788746f, 0.163657f, -0.163657f, 0.3278f, 0.5570f, 0.7631f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.757225f, 0.514726f, -0.341660f, 0.8547f, -0.4548f, 0.2504f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.670563f, 0.457139f, -0.150432f, 0.8547f, -0.4548f, 0.2504f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.549565f, 0.245974f, -0.120995f, 0.8547f, -0.4548f, 0.2504f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.814341f, -0.547506f, -0.180060f, 0.7295f, 0.1934f, 0.6561f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.729381f, -0.313419f, -0.154613f, 0.7295f, 0.1934f, 0.6561f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.575239f, -0.256545f, 0.000000f, 0.7295f, 0.1934f, 0.6561f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.729381f, -0.313419f, -0.154613f, 0.6670f, -0.2949f, -0.6842f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.786823f, -0.163316f, -0.163316f, 0.6670f, -0.2949f, -0.6842f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.942295f, -0.190542f, -0.000000f, 0.6670f, -0.2949f, -0.6842f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.786823f, -0.163316f, -0.163316f, 0.5741f, 0.0926f, 0.8135f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.752095f, 0.000000f, -0.157400f, 0.5741f, 0.0926f, 0.8135f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.529077f, -0.000000f, 0.000000f, 0.5741f, 0.0926f, 0.8135f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.788746f, 0.163657f, -0.163657f, 0.3992f, -0.8824f, -0.2490f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.955271f, 0.192803f, -0.000000f, 0.3992f, -0.8824f, -0.2490f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.529077f, -0.000000f, 0.000000f, 0.3992f, -0.8824f, -0.2490f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.549565f, 0.245974f, -0.120995f, 0.2960f, 0.0955f, -0.9504f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.896296f, 0.375580f, 0.000000f, 0.2960f, 0.0955f, -0.9504f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.955271f, 0.192803f, -0.000000f, 0.2960f, 0.0955f, -0.9504f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.670563f, 0.457139f, -0.150432f, 0.5719f, 0.6427f, -0.5097f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.749169f, 0.506496f, -0.000000f, 0.5719f, 0.6427f, -0.5097f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.896296f, 0.375580f, 0.000000f, 0.5719f, 0.6427f, -0.5097f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.575239f, -0.256545f, 0.000000f, 0.4241f, -0.2935f, -0.8567f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.864106f, -0.363951f, 0.179801f, 0.4241f, -0.2935f, -0.8567f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.739019f, -0.500165f, 0.164539f, 0.4241f, -0.2935f, -0.8567f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.942295f, -0.190542f, -0.000000f, 0.4923f, 0.5097f, 0.7056f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.687068f, -0.145614f, 0.145614f, 0.4923f, 0.5097f, 0.7056f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.864106f, -0.363951f, 0.179801f, 0.4923f, 0.5097f, 0.7056f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.529077f, -0.000000f, 0.000000f, 0.4060f, -0.3873f, -0.8278f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.903925f, 0.000000f, 0.183856f, 0.4060f, -0.3873f, -0.8278f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.687068f, -0.145614f, 0.145614f, 0.4060f, -0.3873f, -0.8278f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.529077f, -0.000000f, 0.000000f, 0.3177f, -0.7023f, 0.6370f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.955271f, 0.192803f, -0.000000f, 0.3177f, -0.7023f, 0.6370f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.554161f, 0.122028f, 0.122028f, 0.3177f, -0.7023f, 0.6370f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.955271f, 0.192803f, -0.000000f, 0.3770f, 0.1216f, 0.9182f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.896296f, 0.375580f, 0.000000f, 0.3770f, 0.1216f, 0.9182f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.609102f, 0.268305f, 0.132126f, 0.3770f, 0.1216f, 0.9182f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.896296f, 0.375580f, 0.000000f, 0.6255f, 0.7030f, 0.3384f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.749169f, 0.506496f, -0.000000f, 0.6255f, 0.7030f, 0.3384f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.699549f, 0.475357f, 0.156405f, 0.6255f, 0.7030f, 0.3384f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.864106f, -0.363951f, 0.179801f, 0.6957f, 0.3685f, 0.6166f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.719294f, -0.312397f, 0.312397f, 0.6957f, 0.3685f, 0.6166f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.799266f, -0.541563f, 0.359127f, 0.6957f, 0.3685f, 0.6166f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.687068f, -0.145614f, 0.145614f, 0.7198f, -0.4164f, -0.5555f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.829506f, -0.173332f, 0.350974f, 0.7198f, -0.4164f, -0.5555f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.719294f, -0.312397f, 0.312397f, 0.7198f, -0.4164f, -0.5555f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.903925f, 0.000000f, 0.183856f, 0.9242f, -0.3816f, 0.0157f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.900631f, 0.000000f, 0.377188f, 0.9242f, -0.3816f, 0.0157f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.829506f, -0.173332f, 0.350974f, 0.9242f, -0.3816f, 0.0157f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.903925f, 0.000000f, 0.183856f, 0.3636f, 0.8565f, -0.3664f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.554161f, 0.122028f, 0.122028f, 0.3636f, 0.8565f, -0.3664f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.673745f, 0.144211f, 0.292551f, 0.3636f, 0.8565f, -0.3664f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.554161f, 0.122028f, 0.122028f, 0.8161f, -0.3389f, 0.4681f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.609102f, 0.268305f, 0.132126f, 0.8161f, -0.3389f, 0.4681f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.535315f, 0.241061f, 0.241061f, 0.8161f, -0.3389f, 0.4681f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.609102f, 0.268305f, 0.132126f, 0.8579f, -0.4109f, 0.3083f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.699549f, 0.475357f, 0.156405f, 0.8579f, -0.4109f, 0.3083f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.632287f, 0.434974f, 0.289751f, 0.8579f, -0.4109f, 0.3083f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.719294f, -0.312397f, 0.312397f, 0.0559f, -0.8888f, 0.4548f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.589297f, -0.271890f, 0.407531f, 0.0559f, -0.8888f, 0.4548f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.445603f, -0.323777f, 0.323777f, 0.0559f, -0.8888f, 0.4548f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.829506f, -0.173332f, 0.350974f, 0.2616f, -0.0859f, 0.9614f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.604331f, -0.136784f, 0.415511f, 0.2616f, -0.0859f, 0.9614f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.589297f, -0.271890f, 0.407531f, 0.2616f, -0.0859f, 0.9614f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.900631f, 0.000000f, 0.377188f, 0.4323f, -0.8682f, 0.2434f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.807342f, 0.000000f, 0.542848f, 0.4323f, -0.8682f, 0.2434f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.604331f, -0.136784f, 0.415511f, 0.4323f, -0.8682f, 0.2434f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.900631f, 0.000000f, 0.377188f, 0.5599f, 0.8226f, -0.0993f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.673745f, 0.144211f, 0.292551f, 0.5599f, 0.8226f, -0.0993f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.690075f, 0.154453f, 0.469403f, 0.5599f, 0.8226f, -0.0993f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.673745f, 0.144211f, 0.292551f, 0.5881f, 0.5046f, -0.6321f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.535315f, 0.241061f, 0.241061f, 0.5881f, 0.5046f, -0.6321f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.736188f, 0.332920f, 0.501298f, 0.5881f, 0.5046f, -0.6321f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.535315f, 0.241061f, 0.241061f, 0.8996f, -0.4186f, -0.1246f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.632287f, 0.434974f, 0.289751f, 0.8996f, -0.4186f, -0.1246f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.676614f, 0.475089f, 0.475089f, 0.8996f, -0.4186f, -0.1246f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.472812f, -0.472812f, -0.472812f, 0.7439f, -0.4586f, 0.4861f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.549801f, -0.429528f, -0.549801f, 0.7439f, -0.4586f, 0.4861f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.467835f, -0.338339f, -0.338339f, 0.7439f, -0.4586f, 0.4861f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.549801f, -0.429528f, -0.549801f, 0.2654f, -0.7988f, -0.5398f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.646260f, -0.332295f, -0.646260f, 0.2654f, -0.7988f, -0.5398f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.793392f, -0.356687f, -0.537814f, 0.2654f, -0.7988f, -0.5398f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.646260f, -0.332295f, -0.646260f, 0.7458f, -0.1640f, -0.6457f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.665434f, -0.169601f, -0.665434f, 0.7458f, -0.1640f, -0.6457f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.783441f, -0.173692f, -0.528085f, 0.7458f, -0.1640f, -0.6457f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.665434f, -0.169601f, -0.665434f, 0.7574f, -0.2539f, -0.6016f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.697125f, -0.000000f, -0.697125f, 0.7574f, -0.2539f, -0.6016f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.815583f, -0.000000f, -0.547997f, 0.7574f, -0.2539f, -0.6016f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.717813f, 0.182497f, -0.717813f, 0.8011f, 0.5771f, 0.1589f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.688762f, 0.154182f, -0.468578f, 0.8011f, 0.5771f, 0.1589f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.815583f, -0.000000f, -0.547997f, 0.8011f, 0.5771f, 0.1589f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.657125f, 0.337577f, -0.657125f, 0.4225f, -0.6132f, -0.6674f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.850258f, 0.380313f, -0.574113f, 0.4225f, -0.6132f, -0.6674f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.688762f, 0.154182f, -0.468578f, 0.4225f, -0.6132f, -0.6674f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.521509f, 0.408953f, -0.521509f, -0.1650f, -0.0893f, -0.9822f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.860198f, 0.595336f, -0.595336f, -0.1650f, -0.0893f, -0.9822f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.850258f, 0.380313f, -0.574113f, -0.1650f, -0.0893f, -0.9822f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.421482f, 0.421482f, -0.421482f, -0.4467f, 0.2764f, -0.8509f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.583262f, 0.583262f, -0.453862f, -0.4467f, 0.2764f, -0.8509f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.860198f, 0.595336f, -0.595336f, -0.4467f, 0.2764f, -0.8509f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.583262f, 0.583262f, -0.453862f, 0.0161f, 0.8642f, 0.5029f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.462871f, 0.462871f, -0.243147f, 0.0161f, 0.8642f, 0.5029f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.757225f, 0.514726f, -0.341660f, 0.0161f, 0.8642f, 0.5029f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.462871f, 0.462871f, -0.243147f, 0.3973f, -0.1750f, -0.9008f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.713197f, 0.713197f, -0.181360f, 0.3973f, -0.1750f, -0.9008f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.670563f, 0.457139f, -0.150432f, 0.3973f, -0.1750f, -0.9008f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.713197f, 0.713197f, -0.181360f, 0.3940f, 0.6440f, 0.6558f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.598613f, 0.598613f, -0.000000f, 0.3940f, 0.6440f, 0.6558f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.749169f, 0.506496f, -0.000000f, 0.3940f, 0.6440f, 0.6558f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.749169f, 0.506496f, -0.000000f, 0.5174f, 0.8457f, -0.1306f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.598613f, 0.598613f, -0.000000f, 0.5174f, 0.8457f, -0.1306f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.613645f, 0.613645f, 0.156850f, 0.5174f, 0.8457f, -0.1306f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.699549f, 0.475357f, 0.156405f, 0.6443f, 0.4023f, -0.6503f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.613645f, 0.613645f, 0.156850f, 0.6443f, 0.4023f, -0.6503f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.755718f, 0.755718f, 0.385504f, 0.6443f, 0.4023f, -0.6503f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.632287f, 0.434974f, 0.289751f, 0.4272f, -0.4056f, 0.8080f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.755718f, 0.755718f, 0.385504f, 0.4272f, -0.4056f, 0.8080f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.498710f, 0.498710f, 0.392372f, 0.4272f, -0.4056f, 0.8080f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.676614f, 0.475089f, 0.475089f, 0.2704f, 0.9071f, -0.3225f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.498710f, 0.498710f, 0.392372f, 0.2704f, 0.9071f, -0.3225f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.538827f, 0.538827f, 0.538827f, 0.2704f, 0.9071f, -0.3225f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.736188f, 0.332920f, 0.501298f, -0.2161f, 0.0887f, 0.9723f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.676614f, 0.475089f, 0.475089f, -0.2161f, 0.0887f, 0.9723f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.432840f, 0.344470f, 0.432840f, -0.2161f, 0.0887f, 0.9723f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.690075f, 0.154453f, 0.469403f, -0.0779f, -0.1559f, 0.9847f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.736188f, 0.332920f, 0.501298f, -0.0779f, -0.1559f, 0.9847f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.465944f, 0.244641f, 0.465945f, -0.0779f, -0.1559f, 0.9847f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.807342f, 0.000000f, 0.542848f, 0.7658f, 0.6336f, 0.1097f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.690075f, 0.154453f, 0.469403f, 0.7658f, 0.6336f, 0.1097f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.653571f, 0.166680f, 0.653571f, 0.7658f, 0.6336f, 0.1097f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.807342f, 0.000000f, 0.542848f, 0.9862f, -0.1625f, -0.0302f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.815698f, -0.000000f, 0.815698f, 0.9862f, -0.1625f, -0.0302f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.781988f, -0.198297f, 0.781988f, 0.9862f, -0.1625f, -0.0302f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.604331f, -0.136784f, 0.415511f, 0.8265f, -0.3301f, -0.4561f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.781988f, -0.198297f, 0.781988f, 0.8265f, -0.3301f, -0.4561f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.657647f, -0.337831f, 0.657647f, 0.8265f, -0.3301f, -0.4561f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.589297f, -0.271890f, 0.407531f, 0.2033f, -0.9317f, -0.3012f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.657647f, -0.337831f, 0.657647f, 0.2033f, -0.9317f, -0.3012f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.384191f, -0.309091f, 0.384191f, 0.2033f, -0.9317f, -0.3012f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.445603f, -0.323777f, 0.323777f, 0.3480f, -0.7664f, 0.5400f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.384191f, -0.309091f, 0.384191f, 0.3480f, -0.7664f, 0.5400f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.349401f, -0.349401f, 0.349401f, 0.3480f, -0.7664f, 0.5400f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.799266f, -0.541563f, 0.359127f, -0.0051f, 0.1521f, 0.9883f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.445603f, -0.323777f, 0.323777f, -0.0051f, 0.1521f, 0.9883f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.425849f, -0.425849f, 0.339386f, -0.0051f, 0.1521f, 0.9883f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.739019f, -0.500165f, 0.164539f, 0.2309f, -0.9347f, -0.2703f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.799266f, -0.541563f, 0.359127f, 0.2309f, -0.9347f, -0.2703f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.578778f, -0.578778f, 0.299491f, 0.2309f, -0.9347f, -0.2703f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.542205f, -0.377167f, 0.000000f, 0.5368f, -0.2277f, -0.8124f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.739019f, -0.500165f, 0.164539f, 0.5368f, -0.2277f, -0.8124f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.676028f, -0.676028f, 0.172209f, 0.5368f, -0.2277f, -0.8124f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.623072f, -0.623072f, -0.159171f, 0.2733f, -0.4578f, 0.8460f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.814341f, -0.547506f, -0.180060f, 0.2733f, -0.4578f, 0.8460f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.542205f, -0.377167f, 0.000000f, 0.2733f, -0.4578f, 0.8460f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.499946f, -0.499946f, -0.261170f, -0.1104f, -0.9828f, -0.1483f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.760581f, -0.516869f, -0.343055f, -0.1104f, -0.9828f, -0.1483f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.814341f, -0.547506f, -0.180060f, -0.1104f, -0.9828f, -0.1483f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.591253f, -0.591253f, -0.459673f, 0.3106f, 0.5301f, -0.7890f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.467835f, -0.338339f, -0.338339f, 0.3106f, 0.5301f, -0.7890f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.760581f, -0.516869f, -0.343055f, 0.3106f, 0.5301f, -0.7890f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.492963f, -0.492963f, -0.703903f, -0.7043f, 0.5090f, -0.4949f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.411329f, -0.274362f, -0.595247f, -0.7043f, 0.5090f, -0.4949f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.331187f, -0.331187f, -0.767752f, -0.7043f, 0.5090f, -0.4949f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.411329f, -0.274362f, -0.595247f, -0.9162f, 0.0121f, -0.4006f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.409616f, -0.134851f, -0.594951f, -0.9162f, 0.0121f, -0.4006f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.330872f, -0.163312f, -0.775913f, -0.9162f, 0.0121f, -0.4006f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.409616f, -0.134851f, -0.594951f, -0.8835f, 0.3719f, -0.2849f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.372340f, -0.000000f, -0.534481f, -0.8835f, 0.3719f, -0.2849f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.311449f, -0.000000f, -0.723324f, -0.8835f, 0.3719f, -0.2849f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.588619f, 0.193539f, -0.879752f, 0.3074f, -0.2918f, -0.9057f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.333924f, 0.164833f, -0.784050f, 0.3074f, -0.2918f, -0.9057f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.311449f, -0.000000f, -0.723324f, 0.3074f, -0.2918f, -0.9057f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.441012f, 0.293681f, -0.641746f, -0.3303f, 0.5625f, -0.7579f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.302980f, 0.302980f, -0.695006f, -0.3303f, 0.5625f, -0.7579f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.333924f, 0.164833f, -0.784050f, -0.3303f, 0.5625f, -0.7579f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.445498f, 0.445498f, -0.631437f, -0.2319f, 0.1933f, -0.9533f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.302979f, 0.455296f, -0.664123f, -0.2319f, 0.1933f, -0.9533f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.302980f, 0.302980f, -0.695006f, -0.2319f, 0.1933f, -0.9533f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.263289f, -0.394317f, -0.568596f, 0.6319f, -0.6504f, -0.4216f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.331187f, -0.331187f, -0.767752f, 0.6319f, -0.6504f, -0.4216f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.125979f, -0.255973f, -0.576224f, 0.6319f, -0.6504f, -0.4216f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.331187f, -0.331187f, -0.767752f, -0.4136f, -0.0434f, -0.9094f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.330872f, -0.163312f, -0.775913f, -0.4136f, -0.0434f, -0.9094f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.173953f, -0.173953f, -0.846765f, -0.4136f, -0.0434f, -0.9094f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.330872f, -0.163312f, -0.775913f, -0.7632f, 0.2785f, -0.5830f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.311449f, -0.000000f, -0.723324f, -0.7632f, 0.2785f, -0.5830f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.181933f, -0.000000f, -0.892884f, -0.7632f, 0.2785f, -0.5830f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.333924f, 0.164833f, -0.784050f, -0.5378f, 0.0594f, -0.8410f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.180090f, 0.180090f, -0.881345f, -0.5378f, 0.0594f, -0.8410f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.181933f, -0.000000f, -0.892884f, -0.5378f, 0.0594f, -0.8410f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.302980f, 0.302980f, -0.695006f, -0.6470f, 0.3667f, -0.6685f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.168564f, 0.341408f, -0.804003f, -0.6470f, 0.3667f, -0.6685f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.180090f, 0.180090f, -0.881345f, -0.6470f, 0.3667f, -0.6685f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.302979f, 0.455296f, -0.664123f, 0.5093f, 0.8386f, -0.1934f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.123491f, 0.374969f, -0.539827f, 0.5093f, 0.8386f, -0.1934f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.168564f, 0.341408f, -0.804003f, 0.5093f, 0.8386f, -0.1934f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.198604f, -0.604068f, -0.904332f, -0.7590f, 0.5234f, -0.3873f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.125979f, -0.255973f, -0.576224f, -0.7590f, 0.5234f, -0.3873f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.441348f, -1.073682f, -0.7590f, 0.5234f, -0.3873f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.125979f, -0.255973f, -0.576224f, -0.4898f, -0.8547f, -0.1723f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.173953f, -0.173953f, -0.846765f, -0.4898f, -0.8547f, -0.1723f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, -0.220143f, -1.112172f, -0.4898f, -0.8547f, -0.1723f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.173953f, -0.173953f, -0.846765f, 0.4505f, -0.2094f, -0.8679f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.181933f, -0.000000f, -0.892884f, 0.4505f, -0.2094f, -0.8679f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.000000f, -0.798438f, 0.4505f, -0.2094f, -0.8679f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.180090f, 0.180090f, -0.881345f, 0.2968f, -0.1382f, -0.9449f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.169804f, -0.823278f, 0.2968f, -0.1382f, -0.9449f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.000000f, -0.798438f, 0.2968f, -0.1382f, -0.9449f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.168564f, 0.341408f, -0.804003f, -0.5849f, -0.5031f, -0.6363f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.422621f, -1.023172f, -0.5849f, -0.5031f, -0.6363f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.169804f, -0.823278f, -0.5849f, -0.5031f, -0.6363f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.123491f, 0.374969f, -0.539827f, -0.9649f, 0.1171f, -0.2350f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.617128f, -0.926213f, -0.9649f, 0.1171f, -0.2350f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.422621f, -1.023172f, -0.9649f, 0.1171f, -0.2350f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.441348f, -1.073682f, 0.4748f, -0.8799f, 0.0169f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170119f, -0.344528f, -0.812321f, 0.4748f, -0.8799f, 0.0169f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.120480f, -0.365784f, -0.525213f, 0.4748f, -0.8799f, 0.0169f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, -0.220143f, -1.112172f, 0.8246f, -0.1718f, -0.5391f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.175831f, -0.175831f, -0.857344f, 0.8246f, -0.1718f, -0.5391f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.170119f, -0.344528f, -0.812321f, 0.8246f, -0.1718f, -0.5391f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.000000f, -0.798438f, 0.5241f, 0.6909f, -0.4979f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.139827f, 0.000000f, -0.651244f, 0.5241f, 0.6909f, -0.4979f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.175831f, -0.175831f, -0.857344f, 0.5241f, 0.6909f, -0.4979f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.000000f, -0.798438f, -0.4220f, -0.1312f, -0.8971f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.169804f, -0.823278f, -0.4220f, -0.1312f, -0.8971f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185700f, 0.185700f, -0.912960f, -0.4220f, -0.1312f, -0.8971f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.169804f, -0.823278f, 0.7252f, -0.4270f, -0.5401f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.000000f, 0.422621f, -1.023172f, 0.7252f, -0.4270f, -0.5401f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.154583f, 0.313359f, -0.729220f, 0.7252f, -0.4270f, -0.5401f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.000000f, 0.422621f, -1.023172f, 0.7527f, 0.2937f, -0.5892f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.617128f, -0.926213f, 0.7527f, 0.2937f, -0.5892f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.169152f, 0.514237f, -0.761408f, 0.7527f, 0.2937f, -0.5892f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.170119f, -0.344528f, -0.812321f, -0.0096f, -0.9980f, -0.0625f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.346531f, -0.346531f, -0.807324f, -0.0096f, -0.9980f, -0.0625f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.243242f, -0.363517f, -0.520346f, -0.0096f, -0.9980f, -0.0625f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.175831f, -0.175831f, -0.857344f, 0.0088f, -0.2731f, -0.9620f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.360539f, -0.178100f, -0.855010f, 0.0088f, -0.2731f, -0.9620f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.346531f, -0.346531f, -0.807324f, 0.0088f, -0.2731f, -0.9620f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.139827f, 0.000000f, -0.651244f, -0.5104f, 0.2927f, -0.8086f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.328992f, 0.000000f, -0.770641f, -0.5104f, 0.2927f, -0.8086f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.360539f, -0.178100f, -0.855010f, -0.5104f, 0.2927f, -0.8086f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.139827f, 0.000000f, -0.651244f, -0.2173f, -0.7777f, -0.5899f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.185700f, 0.185700f, -0.912960f, -0.2173f, -0.7777f, -0.5899f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.429019f, 0.212234f, -1.037585f, -0.2173f, -0.7777f, -0.5899f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.185700f, 0.185700f, -0.912960f, -0.4086f, 0.7157f, -0.5664f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.154583f, 0.313359f, -0.729220f, -0.4086f, 0.7157f, -0.5664f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.352443f, 0.352443f, -0.822573f, -0.4086f, 0.7157f, -0.5664f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.154583f, 0.313359f, -0.729220f, -0.1223f, -0.1484f, -0.9813f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.169152f, 0.514237f, -0.761408f, -0.1223f, -0.1484f, -0.9813f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.354225f, 0.534031f, -0.787467f, -0.1223f, -0.1484f, -0.9813f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.346531f, -0.346531f, -0.807324f, 0.7680f, 0.5519f, -0.3248f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.388761f, -0.259673f, -0.559892f, 0.7680f, 0.5519f, -0.3248f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.495694f, -0.495695f, -0.708073f, 0.7680f, 0.5519f, -0.3248f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.360539f, -0.178100f, -0.855010f, 0.3974f, -0.8740f, -0.2796f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.490235f, -0.161283f, -0.723219f, 0.3974f, -0.8740f, -0.2796f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.388761f, -0.259673f, -0.559892f, 0.3974f, -0.8740f, -0.2796f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.328992f, 0.000000f, -0.770641f, -0.1867f, -0.4443f, -0.8762f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.549247f, -0.000000f, -0.817583f, -0.1867f, -0.4443f, -0.8762f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.490235f, -0.161283f, -0.723219f, -0.1867f, -0.4443f, -0.8762f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.328992f, 0.000000f, -0.770641f, 0.8127f, -0.5645f, -0.1443f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.429019f, 0.212234f, -1.037585f, 0.8127f, -0.5645f, -0.1443f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.451548f, 0.148599f, -0.661667f, 0.8127f, -0.5645f, -0.1443f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.429019f, 0.212234f, -1.037585f, 0.7415f, 0.6514f, -0.1607f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.352443f, 0.352443f, -0.822573f, 0.7415f, 0.6514f, -0.1607f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.442193f, 0.294450f, -0.643596f, 0.7415f, 0.6514f, -0.1607f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.352443f, 0.352443f, -0.822573f, 0.7826f, 0.1108f, -0.6126f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.354225f, 0.534031f, -0.787467f, 0.7826f, 0.1108f, -0.6126f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.464074f, 0.464074f, -0.659797f, 0.7826f, 0.1108f, -0.6126f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.450496f, -0.450496f, -0.450496f, -0.0338f, -0.9847f, 0.1707f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.608913f, -0.472516f, -0.608913f, -0.0338f, -0.9847f, 0.1707f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.492963f, -0.492963f, -0.703903f, -0.0338f, -0.9847f, 0.1707f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.608913f, -0.472516f, -0.608913f, -0.6269f, 0.6542f, -0.4231f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.467353f, -0.245326f, -0.467352f, -0.6269f, 0.6542f, -0.4231f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.411329f, -0.274362f, -0.595247f, -0.6269f, 0.6542f, -0.4231f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.467353f, -0.245326f, -0.467352f, 0.0075f, -0.7577f, -0.6526f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.580775f, -0.148757f, -0.580775f, 0.0075f, -0.7577f, -0.6526f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.409616f, -0.134851f, -0.594951f, 0.0075f, -0.7577f, -0.6526f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.580775f, -0.148757f, -0.580775f, -0.5582f, 0.8205f, -0.1231f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.401631f, 0.000000f, -0.401631f, -0.5582f, 0.8205f, -0.1231f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.372340f, -0.000000f, -0.534481f, -0.5582f, 0.8205f, -0.1231f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.681304f, 0.173508f, -0.681304f, -0.3737f, -0.8891f, -0.2643f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.588619f, 0.193539f, -0.879752f, -0.3737f, -0.8891f, -0.2643f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.372340f, -0.000000f, -0.534481f, -0.3737f, -0.8891f, -0.2643f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.674306f, 0.345929f, -0.674306f, 0.2551f, 0.8245f, -0.5051f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.441012f, 0.293681f, -0.641746f, 0.2551f, 0.8245f, -0.5051f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.588619f, 0.193539f, -0.879752f, 0.2551f, 0.8245f, -0.5051f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.437378f, 0.347770f, -0.437378f, -0.9992f, -0.0313f, 0.0260f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.445498f, 0.445498f, -0.631437f, -0.9992f, -0.0313f, 0.0260f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.441012f, 0.293681f, -0.641746f, -0.9992f, -0.0313f, 0.0260f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.271962f, 0.271962f, -0.271962f, -0.9168f, -0.1498f, 0.3703f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.429708f, 0.550049f, -0.550049f, -0.9168f, -0.1498f, 0.3703f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.445498f, 0.445498f, -0.631437f, -0.9168f, -0.1498f, 0.3703f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.429708f, 0.550049f, -0.550049f, 0.6130f, 0.7897f, 0.0250f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.193485f, 0.360709f, -0.360709f, 0.6130f, 0.7897f, 0.0250f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.302979f, 0.455296f, -0.664123f, 0.6130f, 0.7897f, 0.0250f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.193485f, 0.360709f, -0.360709f, -0.8905f, -0.2658f, -0.3692f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.161804f, 0.633768f, -0.633768f, -0.8905f, -0.2658f, -0.3692f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.123491f, 0.374969f, -0.539827f, -0.8905f, -0.2658f, -0.3692f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.161804f, 0.633768f, -0.633768f, -0.5786f, 0.7310f, -0.3617f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.719448f, -0.719448f, -0.5786f, 0.7310f, -0.3617f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.617128f, -0.926213f, -0.5786f, 0.7310f, -0.3617f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, 0.617128f, -0.926213f, 0.8314f, 0.4980f, -0.2464f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, 0.719448f, -0.719448f, 0.8314f, 0.4980f, -0.2464f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.143451f, 0.559224f, -0.559224f, 0.8314f, 0.4980f, -0.2464f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.169152f, 0.514237f, -0.761408f, 0.1587f, 0.9678f, -0.1952f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.143451f, 0.559224f, -0.559224f, 0.1587f, 0.9678f, -0.1952f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.280869f, 0.540470f, -0.540470f, 0.1587f, 0.9678f, -0.1952f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.354225f, 0.534031f, -0.787467f, 0.8300f, 0.5067f, 0.2333f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.280869f, 0.540470f, -0.540470f, 0.8300f, 0.5067f, 0.2333f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.325094f, 0.406197f, -0.406197f, 0.8300f, 0.5067f, 0.2333f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.464074f, 0.464074f, -0.659797f, -0.1313f, 0.9797f, 0.1516f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.325094f, 0.406197f, -0.406197f, -0.1313f, 0.9797f, 0.1516f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.421482f, 0.421482f, -0.421482f, -0.1313f, 0.9797f, 0.1516f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.442193f, 0.294450f, -0.643596f, 0.8887f, -0.1558f, -0.4312f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.464074f, 0.464074f, -0.659797f, 0.8887f, -0.1558f, -0.4312f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.521509f, 0.408953f, -0.521509f, 0.8887f, -0.1558f, -0.4312f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.451548f, 0.148599f, -0.661667f, -0.0858f, 0.1171f, -0.9894f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.442193f, 0.294450f, -0.643596f, -0.0858f, 0.1171f, -0.9894f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.657125f, 0.337577f, -0.657125f, -0.0858f, 0.1171f, -0.9894f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.549247f, -0.000000f, -0.817583f, -0.2364f, 0.6251f, -0.7439f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.451548f, 0.148599f, -0.661667f, -0.2364f, 0.6251f, -0.7439f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.717813f, 0.182497f, -0.717813f, -0.2364f, 0.6251f, -0.7439f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.549247f, -0.000000f, -0.817583f, 0.6108f, -0.2543f, -0.7498f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.697125f, -0.000000f, -0.697125f, 0.6108f, -0.2543f, -0.7498f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.665434f, -0.169601f, -0.665434f, 0.6108f, -0.2543f, -0.7498f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.490235f, -0.161283f, -0.723219f, 0.3036f, -0.1467f, -0.9415f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.665434f, -0.169601f, -0.665434f, 0.3036f, -0.1467f, -0.9415f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.646260f, -0.332295f, -0.646260f, 0.3036f, -0.1467f, -0.9415f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.388761f, -0.259673f, -0.559892f, -0.3930f, -0.4211f, -0.8175f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.646260f, -0.332295f, -0.646260f, -0.3930f, -0.4211f, -0.8175f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.549801f, -0.429528f, -0.549801f, -0.3930f, -0.4211f, -0.8175f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.495694f, -0.495695f, -0.708073f, 0.5845f, -0.8001f, 0.1347f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.549801f, -0.429528f, -0.549801f, 0.5845f, -0.8001f, 0.1347f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.472812f, -0.472812f, -0.472812f, 0.5845f, -0.8001f, 0.1347f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.243242f, -0.363517f, -0.520346f, -0.5525f, -0.8163f, -0.1682f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.495694f, -0.495695f, -0.708073f, -0.5525f, -0.8163f, -0.1682f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.362015f, -0.456966f, -0.456966f, -0.5525f, -0.8163f, -0.1682f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.120480f, -0.365784f, -0.525213f, 0.0287f, 0.4068f, -0.9131f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.243242f, -0.363517f, -0.520346f, 0.0287f, 0.4068f, -0.9131f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.329947f, -0.641428f, -0.641428f, 0.0287f, 0.4068f, -0.9131f, 0.7f, 0.7f, 0.7f, 1.0f },

        { 0.000000f, -0.586301f, -0.876880f, 0.7719f, 0.3850f, -0.5059f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.120480f, -0.365784f, -0.525213f, 0.7719f, 0.3850f, -0.5059f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.174764f, -0.686407f, -0.686407f, 0.7719f, 0.3850f, -0.5059f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.148160f, -0.578352f, -0.578352f, 0.0798f, -0.9946f, 0.0661f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.198604f, -0.604068f, -0.904332f, 0.0798f, -0.9946f, 0.0661f, 0.7f, 0.7f, 0.7f, 1.0f },
        { 0.000000f, -0.586301f, -0.876880f, 0.0798f, -0.9946f, 0.0661f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.245217f, -0.467128f, -0.467128f, -0.9637f, -0.2662f, -0.0194f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.263289f, -0.394317f, -0.568596f, -0.9637f, -0.2662f, -0.0194f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.198604f, -0.604068f, -0.904332f, -0.9637f, -0.2662f, -0.0194f, 0.7f, 0.7f, 0.7f, 1.0f },

        { -0.449732f, -0.577583f, -0.577583f, 0.5851f, -0.5668f, -0.5800f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.492963f, -0.492963f, -0.703903f, 0.5851f, -0.5668f, -0.5800f, 0.7f, 0.7f, 0.7f, 1.0f },
        { -0.263289f, -0.394317f, -0.568596f, 0.5851f, -0.5668f, -0.5800f, 0.7f, 0.7f, 0.7f, 1.0f },
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
