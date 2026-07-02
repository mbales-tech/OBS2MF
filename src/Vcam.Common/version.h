// OBS2MF - shared version info (single source of truth for SemVer).
#pragma once

#define OBS2MF_VER_MAJOR 0
#define OBS2MF_VER_MINOR 1
#define OBS2MF_VER_PATCH 0
#define OBS2MF_VER_BUILD 0

#define OBS2MF_STR2(x) L#x
#define OBS2MF_STR(x)  OBS2MF_STR2(x)

#define OBS2MF_VERSION_STRING \
    OBS2MF_STR(OBS2MF_VER_MAJOR) L"." OBS2MF_STR(OBS2MF_VER_MINOR) L"." OBS2MF_STR(OBS2MF_VER_PATCH)

// For RC FILEVERSION/PRODUCTVERSION (comma form).
#define OBS2MF_VERSION_COMMA OBS2MF_VER_MAJOR, OBS2MF_VER_MINOR, OBS2MF_VER_PATCH, OBS2MF_VER_BUILD

#define OBS2MF_PRODUCT_NAME          L"OBS2MF"
#define OBS2MF_CAMERA_FRIENDLY_NAME  L"OBS2MF Camera"
#define OBS2MF_COMPANY               L"OBS2MF"
#define OBS2MF_COPYRIGHT             L"Copyright (c) 2026. Portions derived from smourier/VCamSample (MIT)."
