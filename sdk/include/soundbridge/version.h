#pragma once

#define SOUNDBRIDGE_VERSION_MAJOR  1
#define SOUNDBRIDGE_VERSION_MINOR  0
#define SOUNDBRIDGE_VERSION_PATCH  0

// ABI version: increment only when binary compatibility is broken
#define SOUNDBRIDGE_ABI_VERSION    1

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
