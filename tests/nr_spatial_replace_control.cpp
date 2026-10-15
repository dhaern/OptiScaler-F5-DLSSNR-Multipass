// Test-only control of nr_spatial_replace_dx12_gpu.cpp's backendchange scenario: compiled into a scratch build of the
// DLL (an MSBuild import adding this file, ForceImportBeforeCppTargets), never into OptiScaler.vcxproj or a shipped
// DLL.
#include "pch.h"
#include "Config.h"
#include "State.h"

// 2: the menu's backend switch to XeSS, every feature told to change (as the overlay does, between two presents)
extern "C" __declspec(dllexport) bool SpatialTestControl(int operation, int)
{
    if (operation != 2)
        return false;
    Config::Instance()->Dx12Upscaler = Upscaler::XeSS;
    for (auto& change : State::Instance().changeBackend)
        change.second = true;
    return true;
}
