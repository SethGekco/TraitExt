#include "TraitExt.h"

#include <Phobos.h>
#include <Syringe.h>
#include <Utilities/Patch.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

HANDLE TraitExtDLL::hInstance = nullptr;

char TraitExtDLL::readBuffer[TraitExtDLL::readLength];
wchar_t TraitExtDLL::wideBuffer[TraitExtDLL::readLength];

void TraitExtDLL::ExeRun()
{
    Patch::ApplyStatic();
}

bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        TraitExtDLL::hInstance = hInstance;
        Phobos::hInstance = hInstance; // needed by Patch::ApplyStatic
    }
    return true;
}

SYRINGE_HANDSHAKE(pInfo)
{
    pInfo->Message = const_cast<char*>("TraitExt");
    return S_OK;
}

DEFINE_HOOK(0x7CD810, ExeRun, 0x9)
{
    TraitExtDLL::ExeRun();
    return 0;
}

DEFINE_HOOK(0x52F639, CmdLineParse, 0x5)
{
    Debug::LogDeferredFinalize();
    return 0;
}
