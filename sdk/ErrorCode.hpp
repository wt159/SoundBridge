#pragma once

#include <cstdint>
#include <string>

namespace sdk {

enum class ErrorModule {
    Unknown,
    File,
    Extractor,
    Decode,
    Resample,
    Playlist,
    AudioDevice,
    Player
};

enum class ErrorSeverity {
    Info,
    Warning,
    Error,
    Fatal
};

enum class ErrorAction {
    None,
    SkipTrack,
    StopPlayback,
    CheckFile,
    ReportBug
};

struct ErrorCodeInfo {
    ErrorModule module;
    ErrorSeverity severity;
    bool recoverable;
    ErrorAction action;
    const char *message;
};

enum class ErrorCode : int32_t {
    Ok = 0,
    FileOpenFailed = 0x010001,
    ExtractorUnsupported = 0x020001,
    ExtractorInitFailed = 0x020002,
    DecodeInitFailed = 0x030001,
    DecodeFailed = 0x030002,
    ResampleInitFailed = 0x040001,
    ResampleFailed = 0x040002,
    PlaylistEmpty = 0x050001,
    PlayerNoCurrent = 0x060001,
    AudioDeviceOpenFailed = 0x070001,
    AudioDeviceStartFailed = 0x070002,
    Unknown = 0x7fffffff
};

const ErrorCodeInfo &GetErrorInfo(ErrorCode code);
const char *ToString(ErrorModule module);
const char *ToString(ErrorSeverity severity);
const char *ToString(ErrorAction action);
std::string FormatError(ErrorCode code, const std::string &detail);

} // namespace sdk
