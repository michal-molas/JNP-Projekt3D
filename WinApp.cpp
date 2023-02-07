#include "stdafx.h"
#include "WinApp.h"

HWND WinApp::hwnd = nullptr;

int WinApp::Run(D3DApp* dApp, HINSTANCE hInstance, int nCmdShow)
{
    // Parse the command line parameters
    //int argc;
    //LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    //dApp->ParseCommandLineArgs(argv, argc);
    //LocalFree(argv);

    // Initialize the window class.
    WNDCLASSEX windowClass = { 0 };
    windowClass.cbSize = sizeof(WNDCLASSEX);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = hInstance;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.lpszClassName = L"DXSampleClass";
    RegisterClassEx(&windowClass);

    RECT windowRect = { 0, 0, static_cast<LONG>(dApp->GetWidth()), static_cast<LONG>(dApp->GetHeight()) };
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    // Create the window and store a handle to it.
    hwnd = CreateWindow(
        windowClass.lpszClassName,
        dApp->GetTitle(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr,        // We have no parent window.
        nullptr,        // We aren't using menus.
        hInstance,
        dApp);

    // Initialize the sample. OnInit is defined in each child-implementation of DXSample.
    dApp->init();

    ShowWindow(hwnd, nCmdShow);

    // Main sample loop.
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        // Process any messages in the queue.
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    dApp->destroy();

    // Return this part of the WM_QUIT message to Windows.
    return static_cast<char>(msg.wParam);
}

// Main message handler for the sample.
LRESULT CALLBACK WinApp::WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    D3DApp* dApp = reinterpret_cast<D3DApp*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (message)
    {
    case WM_CREATE:
    {
        // Save the DXSample* passed in to CreateWindow.
        LPCREATESTRUCT pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lParam);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));
    }
    return 0;

    case WM_KEYDOWN:
        if (dApp)
        {
            //dApp->onKeyDown(wParam);
            //dApp->update();
            //dApp->OnKeyDown(static_cast<UINT8>(wParam));
        }
        return 0;

    case WM_KEYUP:
        if (dApp)
        {
            //dApp->OnKeyUp(static_cast<UINT8>(wParam));
        }
        return 0;

    case WM_PAINT:
        if (dApp)
        {
            dApp->checkKeys();
            dApp->update();
            dApp->render();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    // Handle any messages the switch statement didn't.
    return DefWindowProc(hWnd, message, wParam, lParam);
}
