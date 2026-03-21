#include "ErrorCode.hpp"

#include <sstream>

namespace sdk {
namespace {

ErrorCodeInfo MakeInfo(ErrorModule module, ErrorSeverity severity, bool recoverable, ErrorAction action,
                       const char *message)
{
    return ErrorCodeInfo{module, severity, recoverable, action, message};
}

const ErrorCodeInfo kUnknownInfo
    = MakeInfo(ErrorModule::Unknown, ErrorSeverity::Error, false, ErrorAction::ReportBug, "Unknown error");

} // namespace

const ErrorCodeInfo &GetErrorInfo(ErrorCode code)
{
    switch (code) {
    case ErrorCode::Ok:
        static const ErrorCodeInfo kOkInfo
            = MakeInfo(ErrorModule::Unknown, ErrorSeverity::Info, true, ErrorAction::None, "Ok");
        return kOkInfo;
    case ErrorCode::FileOpenFailed:
        static const ErrorCodeInfo kFileOpen
            = MakeInfo(ErrorModule::File, ErrorSeverity::Error, false, ErrorAction::CheckFile, "Open file failed");
        return kFileOpen;
    case ErrorCode::ExtractorUnsupported:
        static const ErrorCodeInfo kExtractorUnsupported
            = MakeInfo(ErrorModule::Extractor, ErrorSeverity::Warning, false, ErrorAction::SkipTrack, "Unsupported format");
        return kExtractorUnsupported;
    case ErrorCode::ExtractorInitFailed:
        static const ErrorCodeInfo kExtractorInit
            = MakeInfo(ErrorModule::Extractor, ErrorSeverity::Error, false, ErrorAction::SkipTrack, "Extractor init failed");
        return kExtractorInit;
    case ErrorCode::DecodeInitFailed:
        static const ErrorCodeInfo kDecodeInit
            = MakeInfo(ErrorModule::Decode, ErrorSeverity::Error, false, ErrorAction::SkipTrack, "Decode init failed");
        return kDecodeInit;
    case ErrorCode::DecodeFailed:
        static const ErrorCodeInfo kDecodeFailed
            = MakeInfo(ErrorModule::Decode, ErrorSeverity::Error, false, ErrorAction::SkipTrack, "Decode failed");
        return kDecodeFailed;
    case ErrorCode::ResampleInitFailed:
        static const ErrorCodeInfo kResampleInit
            = MakeInfo(ErrorModule::Resample, ErrorSeverity::Error, true, ErrorAction::SkipTrack, "Resample init failed");
        return kResampleInit;
    case ErrorCode::ResampleFailed:
        static const ErrorCodeInfo kResampleFailed
            = MakeInfo(ErrorModule::Resample, ErrorSeverity::Error, true, ErrorAction::SkipTrack, "Resample failed");
        return kResampleFailed;
    case ErrorCode::PlaylistEmpty:
        static const ErrorCodeInfo kPlaylistEmpty
            = MakeInfo(ErrorModule::Playlist, ErrorSeverity::Warning, true, ErrorAction::None, "Playlist is empty");
        return kPlaylistEmpty;
    case ErrorCode::PlayerNoCurrent:
        static const ErrorCodeInfo kPlayerNoCurrent
            = MakeInfo(ErrorModule::Player, ErrorSeverity::Warning, true, ErrorAction::None, "No current track");
        return kPlayerNoCurrent;
    case ErrorCode::AudioDeviceOpenFailed:
        static const ErrorCodeInfo kAudioOpen
            = MakeInfo(ErrorModule::AudioDevice, ErrorSeverity::Error, true, ErrorAction::StopPlayback, "Audio device open failed");
        return kAudioOpen;
    case ErrorCode::AudioDeviceStartFailed:
        static const ErrorCodeInfo kAudioStart
            = MakeInfo(ErrorModule::AudioDevice, ErrorSeverity::Error, true, ErrorAction::StopPlayback, "Audio device start failed");
        return kAudioStart;
    case ErrorCode::Unknown:
    default:
        return kUnknownInfo;
    }
}

const char *ToString(ErrorModule module)
{
    switch (module) {
    case ErrorModule::File:
        return "File";
    case ErrorModule::Extractor:
        return "Extractor";
    case ErrorModule::Decode:
        return "Decode";
    case ErrorModule::Resample:
        return "Resample";
    case ErrorModule::Playlist:
        return "Playlist";
    case ErrorModule::AudioDevice:
        return "AudioDevice";
    case ErrorModule::Player:
        return "Player";
    case ErrorModule::Unknown:
    default:
        return "Unknown";
    }
}

const char *ToString(ErrorSeverity severity)
{
    switch (severity) {
    case ErrorSeverity::Info:
        return "Info";
    case ErrorSeverity::Warning:
        return "Warning";
    case ErrorSeverity::Error:
        return "Error";
    case ErrorSeverity::Fatal:
        return "Fatal";
    default:
        return "Error";
    }
}

const char *ToString(ErrorAction action)
{
    switch (action) {
    case ErrorAction::None:
        return "None";
    case ErrorAction::SkipTrack:
        return "SkipTrack";
    case ErrorAction::StopPlayback:
        return "StopPlayback";
    case ErrorAction::CheckFile:
        return "CheckFile";
    case ErrorAction::ReportBug:
        return "ReportBug";
    default:
        return "None";
    }
}

std::string FormatError(ErrorCode code, const std::string &detail)
{
    const ErrorCodeInfo &info = GetErrorInfo(code);
    std::ostringstream oss;
    oss << info.message;
    if (!detail.empty()) {
        oss << ": " << detail;
    }
    return oss.str();
}

} // namespace sdk
