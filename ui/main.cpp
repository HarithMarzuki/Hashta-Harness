#include <windows.h>

#include "app.h"

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int rc = AppRun(instance);
    if (SUCCEEDED(com)) CoUninitialize();
    return rc;
}
