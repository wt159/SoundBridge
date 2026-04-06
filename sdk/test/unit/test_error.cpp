#include "../fixtures/RealMediaFixture.hpp"
#include <ErrorCode.hpp>
#include <doctest/doctest.h>
#include <soundbridge/error.h>

TEST_SUITE("sdk::ErrorCode")
{
    TEST_CASE("GetErrorInfo returns correct info for Ok")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::Ok);
        CHECK(info.severity == sdk::ErrorSeverity::Info);
        CHECK(info.recoverable == true);
        CHECK(info.action == sdk::ErrorAction::None);
    }

    TEST_CASE("GetErrorInfo for FileOpenFailed")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::FileOpenFailed);
        CHECK(info.module == sdk::ErrorModule::File);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
        CHECK(info.recoverable == false);
        CHECK(info.action == sdk::ErrorAction::CheckFile);
    }

    TEST_CASE("GetErrorInfo for ExtractorUnsupported")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::ExtractorUnsupported);
        CHECK(info.module == sdk::ErrorModule::Extractor);
        CHECK(info.severity == sdk::ErrorSeverity::Warning);
        CHECK(info.recoverable == false);
        CHECK(info.action == sdk::ErrorAction::SkipTrack);
    }

    TEST_CASE("GetErrorInfo for ExtractorInitFailed")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::ExtractorInitFailed);
        CHECK(info.module == sdk::ErrorModule::Extractor);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
        CHECK(info.recoverable == false);
        CHECK(info.action == sdk::ErrorAction::SkipTrack);
    }

    TEST_CASE("GetErrorInfo for DecodeInitFailed")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::DecodeInitFailed);
        CHECK(info.module == sdk::ErrorModule::Decode);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
        CHECK(info.recoverable == false);
        CHECK(info.action == sdk::ErrorAction::SkipTrack);
    }

    TEST_CASE("GetErrorInfo for DecodeFailed")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::DecodeFailed);
        CHECK(info.module == sdk::ErrorModule::Decode);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
        CHECK(info.recoverable == false);
        CHECK(info.action == sdk::ErrorAction::SkipTrack);
    }

    TEST_CASE("GetErrorInfo for ResampleInitFailed")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::ResampleInitFailed);
        CHECK(info.module == sdk::ErrorModule::Resample);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
        CHECK(info.recoverable == true);
        CHECK(info.action == sdk::ErrorAction::SkipTrack);
    }

    TEST_CASE("GetErrorInfo for ResampleFailed")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::ResampleFailed);
        CHECK(info.module == sdk::ErrorModule::Resample);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
        CHECK(info.recoverable == true);
        CHECK(info.action == sdk::ErrorAction::SkipTrack);
    }

    TEST_CASE("GetErrorInfo for PlaylistEmpty")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::PlaylistEmpty);
        CHECK(info.module == sdk::ErrorModule::Playlist);
        CHECK(info.severity == sdk::ErrorSeverity::Warning);
        CHECK(info.recoverable == true);
        CHECK(info.action == sdk::ErrorAction::None);
    }

    TEST_CASE("GetErrorInfo for PlayerNoCurrent")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::PlayerNoCurrent);
        CHECK(info.module == sdk::ErrorModule::Player);
        CHECK(info.severity == sdk::ErrorSeverity::Warning);
        CHECK(info.recoverable == true);
        CHECK(info.action == sdk::ErrorAction::None);
    }

    TEST_CASE("GetErrorInfo for AudioDeviceOpenFailed")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::AudioDeviceOpenFailed);
        CHECK(info.module == sdk::ErrorModule::AudioDevice);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
        CHECK(info.recoverable == true);
        CHECK(info.action == sdk::ErrorAction::StopPlayback);
    }

    TEST_CASE("GetErrorInfo for AudioDeviceStartFailed")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::AudioDeviceStartFailed);
        CHECK(info.module == sdk::ErrorModule::AudioDevice);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
        CHECK(info.recoverable == true);
        CHECK(info.action == sdk::ErrorAction::StopPlayback);
    }

    TEST_CASE("GetErrorInfo for unknown code returns Unknown info")
    {
        auto info = sdk::GetErrorInfo(static_cast<sdk::ErrorCode>(999));
        CHECK(info.module == sdk::ErrorModule::Unknown);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
        CHECK(info.recoverable == false);
        CHECK(info.action == sdk::ErrorAction::ReportBug);
    }

    TEST_CASE("GetErrorInfo for Unknown code returns Unknown info")
    {
        auto info = sdk::GetErrorInfo(sdk::ErrorCode::Unknown);
        CHECK(info.module == sdk::ErrorModule::Unknown);
        CHECK(info.severity == sdk::ErrorSeverity::Error);
        CHECK(info.recoverable == false);
        CHECK(info.action == sdk::ErrorAction::ReportBug);
    }

    TEST_CASE("ToString for ErrorModule returns correct strings")
    {
        CHECK(std::string(sdk::ToString(sdk::ErrorModule::File)) == "File");
        CHECK(std::string(sdk::ToString(sdk::ErrorModule::Extractor)) == "Extractor");
        CHECK(std::string(sdk::ToString(sdk::ErrorModule::Decode)) == "Decode");
        CHECK(std::string(sdk::ToString(sdk::ErrorModule::Resample)) == "Resample");
        CHECK(std::string(sdk::ToString(sdk::ErrorModule::Playlist)) == "Playlist");
        CHECK(std::string(sdk::ToString(sdk::ErrorModule::AudioDevice)) == "AudioDevice");
        CHECK(std::string(sdk::ToString(sdk::ErrorModule::Player)) == "Player");
        CHECK(std::string(sdk::ToString(sdk::ErrorModule::Unknown)) == "Unknown");
    }

    TEST_CASE("ToString for ErrorModule unknown value returns Unknown")
    {
        CHECK(std::string(sdk::ToString(static_cast<sdk::ErrorModule>(999))) == "Unknown");
    }

    TEST_CASE("ToString for ErrorSeverity returns correct strings")
    {
        CHECK(std::string(sdk::ToString(sdk::ErrorSeverity::Info)) == "Info");
        CHECK(std::string(sdk::ToString(sdk::ErrorSeverity::Warning)) == "Warning");
        CHECK(std::string(sdk::ToString(sdk::ErrorSeverity::Error)) == "Error");
        CHECK(std::string(sdk::ToString(sdk::ErrorSeverity::Fatal)) == "Fatal");
    }

    TEST_CASE("ToString for ErrorSeverity unknown value returns Error")
    {
        CHECK(std::string(sdk::ToString(static_cast<sdk::ErrorSeverity>(999))) == "Error");
    }

    TEST_CASE("ToString for ErrorAction returns correct strings")
    {
        CHECK(std::string(sdk::ToString(sdk::ErrorAction::None)) == "None");
        CHECK(std::string(sdk::ToString(sdk::ErrorAction::SkipTrack)) == "SkipTrack");
        CHECK(std::string(sdk::ToString(sdk::ErrorAction::StopPlayback)) == "StopPlayback");
        CHECK(std::string(sdk::ToString(sdk::ErrorAction::CheckFile)) == "CheckFile");
        CHECK(std::string(sdk::ToString(sdk::ErrorAction::ReportBug)) == "ReportBug");
    }

    TEST_CASE("ToString for ErrorAction unknown value returns None")
    {
        CHECK(std::string(sdk::ToString(static_cast<sdk::ErrorAction>(999))) == "None");
    }

    TEST_CASE("FormatError with detail appends correctly")
    {
        std::string result = sdk::FormatError(sdk::ErrorCode::FileOpenFailed, "file.wav");
        CHECK(result.find("Open file failed") != std::string::npos);
        CHECK(result.find("file.wav") != std::string::npos);
    }

    TEST_CASE("FormatError without detail returns base message")
    {
        std::string result = sdk::FormatError(sdk::ErrorCode::Ok, "");
        CHECK(result == "Ok");
    }

    TEST_CASE("FormatError with empty detail returns base message only")
    {
        std::string result = sdk::FormatError(sdk::ErrorCode::DecodeFailed, "");
        CHECK(result == "Decode failed");
    }

    TEST_CASE("FormatError with detail adds colon separator")
    {
        std::string result = sdk::FormatError(sdk::ErrorCode::DecodeFailed, "codec error");
        CHECK(result == "Decode failed: codec error");
    }
}

TEST_SUITE("soundbridge::ErrorCode")
{
    TEST_CASE("soundbridge GetErrorInfo wraps sdk correctly for Ok")
    {
        auto info = soundbridge::GetErrorInfo(soundbridge::ErrorCode::Ok);
        CHECK(info.severity == soundbridge::ErrorSeverity::Info);
        CHECK(info.recoverable == true);
        CHECK(info.action == soundbridge::ErrorAction::None);
    }

    TEST_CASE("soundbridge GetErrorInfo wraps sdk correctly for FileOpenFailed")
    {
        auto info = soundbridge::GetErrorInfo(soundbridge::ErrorCode::FileOpenFailed);
        CHECK(info.module == soundbridge::ErrorModule::File);
        CHECK(info.severity == soundbridge::ErrorSeverity::Error);
        CHECK(info.recoverable == false);
        CHECK(info.action == soundbridge::ErrorAction::CheckFile);
    }

    TEST_CASE("soundbridge GetErrorInfo wraps sdk correctly for DecodeFailed")
    {
        auto info = soundbridge::GetErrorInfo(soundbridge::ErrorCode::DecodeFailed);
        CHECK(info.module == soundbridge::ErrorModule::Decode);
        CHECK(info.severity == soundbridge::ErrorSeverity::Error);
        CHECK(info.recoverable == false);
        CHECK(info.action == soundbridge::ErrorAction::SkipTrack);
    }

    TEST_CASE("soundbridge GetErrorInfo for unknown code")
    {
        auto info = soundbridge::GetErrorInfo(static_cast<soundbridge::ErrorCode>(999));
        CHECK(info.module == soundbridge::ErrorModule::Unknown);
        CHECK(info.severity == soundbridge::ErrorSeverity::Error);
        CHECK(info.recoverable == false);
        CHECK(info.action == soundbridge::ErrorAction::ReportBug);
    }

    TEST_CASE("soundbridge ToString wrappers for ErrorModule")
    {
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorModule::File)) == "File");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorModule::Extractor))
              == "Extractor");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorModule::Decode)) == "Decode");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorModule::Resample)) == "Resample");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorModule::Playlist)) == "Playlist");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorModule::AudioDevice))
              == "AudioDevice");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorModule::Player)) == "Player");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorModule::Unknown)) == "Unknown");
    }

    TEST_CASE("soundbridge ToString wrappers for ErrorSeverity")
    {
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorSeverity::Info)) == "Info");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorSeverity::Warning)) == "Warning");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorSeverity::Error)) == "Error");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorSeverity::Fatal)) == "Fatal");
    }

    TEST_CASE("soundbridge ToString wrappers for ErrorAction")
    {
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorAction::None)) == "None");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorAction::SkipTrack))
              == "SkipTrack");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorAction::StopPlayback))
              == "StopPlayback");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorAction::CheckFile))
              == "CheckFile");
        CHECK(std::string(soundbridge::ToString(soundbridge::ErrorAction::ReportBug))
              == "ReportBug");
    }

    TEST_CASE("soundbridge FormatError with detail")
    {
        auto result = soundbridge::FormatError(soundbridge::ErrorCode::DecodeFailed, "codec error");
        CHECK(result.find("Decode failed") != std::string::npos);
        CHECK(result.find("codec error") != std::string::npos);
    }

    TEST_CASE("soundbridge FormatError without detail")
    {
        auto result = soundbridge::FormatError(soundbridge::ErrorCode::Ok, "");
        CHECK(result == "Ok");
    }

    TEST_CASE("soundbridge FormatError with colon separator")
    {
        auto result = soundbridge::FormatError(soundbridge::ErrorCode::FileOpenFailed, "test.mp3");
        CHECK(result == "Open file failed: test.mp3");
    }
}
