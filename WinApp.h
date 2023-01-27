#pragma once

#include "D3DApp.h"

class WinApp
{
public:
    static int Run(D3DApp* dApp, HINSTANCE hInstance, int nCmdShow);
    static HWND GetHwnd() { return hwnd; }

protected:
    static LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    static HWND hwnd;
};

