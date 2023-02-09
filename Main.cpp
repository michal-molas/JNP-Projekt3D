#include "D3DApp.h"
#include "WinApp.h"

_Use_decl_annotations_
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    RECT desktop;
    GetClientRect(GetDesktopWindow(), &desktop);
    D3DApp sample(desktop.right, desktop.bottom, L"3D App");
    return WinApp::Run(&sample, hInstance, nCmdShow);
}