#pragma once

#define SOUNDBRIDGE_VERSION_MAJOR  1
#define SOUNDBRIDGE_VERSION_MINOR  0
#define SOUNDBRIDGE_VERSION_PATCH  0

// ABI version: increment only when binary compatibility is broken
// 2（2026-11-21，地图 #235）：TrackInfo 追加 std::string id；Player 追加音量四方法。
#define SOUNDBRIDGE_ABI_VERSION    2

#define SOUNDBRIDGE_VERSION_STRING "1.0.0"

namespace soundbridge {

inline int versionMajor()
{
    return SOUNDBRIDGE_VERSION_MAJOR;
}

inline int versionMinor()
{
    return SOUNDBRIDGE_VERSION_MINOR;
}

inline int versionPatch()
{
    return SOUNDBRIDGE_VERSION_PATCH;
}

inline int abiVersion()
{
    return SOUNDBRIDGE_ABI_VERSION;
}

inline const char *versionString()
{
    return SOUNDBRIDGE_VERSION_STRING;
}

} // namespace soundbridge
