// OBS2MF - shared, FROZEN GUIDs. Never change these across versions:
// the camera identity (registration, saved app selections) depends on stability.
#pragma once

#include <guiddef.h>

// CLSID of the OBS2MF Media Foundation virtual-camera media source.
// {611A87DB-833F-4411-8A69-DD2EBE576597}
// constexpr => internal linkage per TU; compared by value, so a header-only
// definition shared by the DLL and the broker is safe and stays identical.
constexpr GUID CLSID_OBS2MFCamera =
    { 0x611a87db, 0x833f, 0x4411, { 0x8a, 0x69, 0xdd, 0x2e, 0xbe, 0x57, 0x65, 0x97 } };
