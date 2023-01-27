#include "D3DApp.h"
#include "WinApp.h"

_Use_decl_annotations_
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    D3DApp sample(1280, 720, L"D3D12 Hello Window");
    return WinApp::Run(&sample, hInstance, nCmdShow);
}