#include "ThreadManager.h"

DWORD WINAPI CaptureThreadEntry(_In_ void* Param);

THREADMANAGER::THREADMANAGER() : m_ThreadCount(0),
                                 m_ThreadHandles(nullptr),
                                 m_ThreadData(nullptr)
{
    RtlZeroMemory(&m_PtrInfo, sizeof(m_PtrInfo));
}

THREADMANAGER::~THREADMANAGER()
{
    Clean();
}

//
// Clean up resources
//
void THREADMANAGER::Clean()
{
    if (m_PtrInfo.PtrShapeBuffer)
    {
        delete [] m_PtrInfo.PtrShapeBuffer;
        m_PtrInfo.PtrShapeBuffer = nullptr;
    }
    RtlZeroMemory(&m_PtrInfo, sizeof(m_PtrInfo));

    if (m_ThreadHandles)
    {
        for (UINT i = 0; i < m_ThreadCount; ++i)
        {
            if (m_ThreadHandles[i])
            {
                CloseHandle(m_ThreadHandles[i]);
            }
        }
        delete [] m_ThreadHandles;
        m_ThreadHandles = nullptr;
    }

    if (m_ThreadData)
    {
        for (UINT i = 0; i < m_ThreadCount; ++i)
        {
            CleanDx(&m_ThreadData[i].DxRes);
        }
        delete [] m_ThreadData;
        m_ThreadData = nullptr;
    }

    m_ThreadCount = 0;
}

//
// Clean up DX_RESOURCES
//
void THREADMANAGER::CleanDx(_Inout_ DX_RESOURCES* Data)
{
    if (Data->Device)
    {
        Data->Device->Release();
        Data->Device = nullptr;
    }

    if (Data->Context)
    {
        Data->Context->Release();
        Data->Context = nullptr;
    }

    if (Data->VertexShader)
    {
        Data->VertexShader->Release();
        Data->VertexShader = nullptr;
    }

    if (Data->PixelShader)
    {
        Data->PixelShader->Release();
        Data->PixelShader = nullptr;
    }

    if (Data->InputLayout)
    {
        Data->InputLayout->Release();
        Data->InputLayout = nullptr;
    }

    if (Data->Sampler)
    {
        Data->Sampler->Release();
        Data->Sampler = nullptr;
    }
}

//
// Start up threads for DDA
//
DUPL_RETURN THREADMANAGER::Initialize(INT SingleOutput, UINT OutputCount, HANDLE UnexpectedErrorEvent, HANDLE ExpectedErrorEvent, HANDLE NewFrameProcessedEvent,
                                      HANDLE PauseDuplicationEvent, HANDLE ResumeDuplicationEvent, HANDLE TerminateThreadsEvent,
                                      HANDLE SharedHandle, _In_ RECT* DesktopDim, IDXGIAdapter* DXGIAdapter)
{
    m_ThreadCount = OutputCount;
    m_ThreadHandles = new (std::nothrow) HANDLE[m_ThreadCount];
    m_ThreadData = new (std::nothrow) THREAD_DATA[m_ThreadCount];
    if (!m_ThreadHandles || !m_ThreadData)
    {
        return ProcessFailure(nullptr, L"Failed to allocate array for threads", L"Desktop+ Error", E_OUTOFMEMORY);
    }

    // Create appropriate # of threads for duplication
    DUPL_RETURN Ret = DUPL_RETURN_SUCCESS;
    for (UINT i = 0; i < m_ThreadCount; ++i)
    {
        m_ThreadData[i].UnexpectedErrorEvent = UnexpectedErrorEvent;
        m_ThreadData[i].ExpectedErrorEvent = ExpectedErrorEvent;
        m_ThreadData[i].NewFrameProcessedEvent = NewFrameProcessedEvent;
        m_ThreadData[i].PauseDuplicationEvent = PauseDuplicationEvent;
        m_ThreadData[i].ResumeDuplicationEvent = ResumeDuplicationEvent;
        m_ThreadData[i].TerminateThreadsEvent = TerminateThreadsEvent;
        m_ThreadData[i].Output = (SingleOutput < 0) ? i : SingleOutput;
        m_ThreadData[i].TexSharedHandle = SharedHandle;
        m_ThreadData[i].OffsetX = DesktopDim->left;
        m_ThreadData[i].OffsetY = DesktopDim->top;
        m_ThreadData[i].PtrInfo = &m_PtrInfo;
        m_ThreadData[i].DirtyRegionTotal = &m_DirtyRegionTotal;

        RtlZeroMemory(&m_ThreadData[i].DxRes, sizeof(DX_RESOURCES));
        Ret = InitializeDx(&m_ThreadData[i].DxRes, DXGIAdapter);
        if (Ret != DUPL_RETURN_SUCCESS)
        {
            if (DXGIAdapter != nullptr)
                DXGIAdapter->Release();

            return Ret;
        }

        DWORD ThreadId;
        m_ThreadHandles[i] = CreateThread(nullptr, 0, CaptureThreadEntry, &m_ThreadData[i], 0, &ThreadId);
        if (m_ThreadHandles[i] == nullptr)
        {
            if (DXGIAdapter != nullptr)
                DXGIAdapter->Release();

            return ProcessFailure(nullptr, L"Failed to create thread", L"Desktop+ Error", E_FAIL);
        }
    }

    if (DXGIAdapter != nullptr)
        DXGIAdapter->Release();

    return Ret;
}

//
// Get DX_RESOURCES
//
DUPL_RETURN THREADMANAGER::InitializeDx(_Out_ DX_RESOURCES* Data, IDXGIAdapter* DXGIAdapter)
{
    HRESULT hr = S_OK;

    // Driver types supported
    D3D_DRIVER_TYPE DriverTypes[] =
    {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };
    UINT NumDriverTypes = ARRAYSIZE(DriverTypes);

    // Feature levels supported
    D3D_FEATURE_LEVEL FeatureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_1
    };
    UINT NumFeatureLevels = ARRAYSIZE(FeatureLevels);

    D3D_FEATURE_LEVEL FeatureLevel;

    // Create device
    hr = D3D11CreateDevice(DXGIAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, FeatureLevels, NumFeatureLevels,
                            D3D11_SDK_VERSION, &Data->Device, &FeatureLevel, &Data->Context);

    if (FAILED(hr))
    {
        return ProcessFailure(nullptr, L"Failed to create device for thread", L"Desktop+ Error", hr);
    }

    // VERTEX shader
    UINT Size = ARRAYSIZE(g_VS);
    hr = Data->Device->CreateVertexShader(g_VS, Size, nullptr, &Data->VertexShader);
    if (FAILED(hr))
    {
        return ProcessFailure(Data->Device, L"Failed to create vertex shader for thread", L"Desktop+ Error", hr, SystemTransitionsExpectedErrors);
    }

    // Input layout
    D3D11_INPUT_ELEMENT_DESC Layout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    UINT NumElements = ARRAYSIZE(Layout);
    hr = Data->Device->CreateInputLayout(Layout, NumElements, g_VS, Size, &Data->InputLayout);
    if (FAILED(hr))
    {
        return ProcessFailure(Data->Device, L"Failed to create input layout for thread", L"Desktop+ Error", hr, SystemTransitionsExpectedErrors);
    }
    Data->Context->IASetInputLayout(Data->InputLayout);

    // Pixel shader
    Size = ARRAYSIZE(g_PS);
    hr = Data->Device->CreatePixelShader(g_PS, Size, nullptr, &Data->PixelShader);
    if (FAILED(hr))
    {
        return ProcessFailure(Data->Device, L"Failed to create pixel shader for thread", L"Desktop+ Error", hr, SystemTransitionsExpectedErrors);
    }

    // Set up sampler
    D3D11_SAMPLER_DESC SampDesc;
    RtlZeroMemory(&SampDesc, sizeof(SampDesc));
    SampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    SampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    SampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    SampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    SampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    SampDesc.MinLOD = 0;
    SampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = Data->Device->CreateSamplerState(&SampDesc, &Data->Sampler);
    if (FAILED(hr))
    {
        return ProcessFailure(Data->Device, L"Failed to create sampler state for thread", L"Desktop+ Error", hr, SystemTransitionsExpectedErrors);
    }

    return DUPL_RETURN_SUCCESS;
}

//
// Getter for the PTR_INFO structure
//
PTR_INFO* THREADMANAGER::GetPointerInfo()
{
    return &m_PtrInfo;
}

DPRect& THREADMANAGER::GetDirtyRegionTotal()
{
    return m_DirtyRegionTotal;
}

//
// Waits infinitely for all spawned threads to terminate
//
void THREADMANAGER::WaitForThreadTermination()
{
    if (m_ThreadCount != 0)
    {
        WaitForMultipleObjectsEx(m_ThreadCount, m_ThreadHandles, TRUE, INFINITE, FALSE);
    }
}
