#pragma once
#include <d3d12.h>

namespace SpatialPassDx12
{
bool Installed();
void Install(ID3D12Device* device);
void OnPresent();
void OnFGPresent();
void OnSessionEnter();
} // namespace SpatialPassDx12
