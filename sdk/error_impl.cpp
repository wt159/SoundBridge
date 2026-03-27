#include "ErrorCode.hpp"
#include "soundbridge/error.h"

namespace soundbridge {

const ErrorInfo &GetErrorInfo(ErrorCode code)
{
    static_assert(sizeof(ErrorInfo) == sizeof(sdk::ErrorCodeInfo), "ABI mismatch");
    static_assert(static_cast<int>(ErrorModule::Unknown)
                      == static_cast<int>(sdk::ErrorModule::Unknown),
                  "Enum mismatch");
    static_assert(static_cast<int>(ErrorSeverity::Info)
                      == static_cast<int>(sdk::ErrorSeverity::Info),
                  "Enum mismatch");
    static_assert(static_cast<int>(ErrorAction::None) == static_cast<int>(sdk::ErrorAction::None),
                  "Enum mismatch");

    const sdk::ErrorCodeInfo &info = sdk::GetErrorInfo(static_cast<sdk::ErrorCode>(code));
    return reinterpret_cast<const ErrorInfo &>(info);
}

const char *ToString(ErrorModule module)
{
    return sdk::ToString(static_cast<sdk::ErrorModule>(module));
}

const char *ToString(ErrorSeverity severity)
{
    return sdk::ToString(static_cast<sdk::ErrorSeverity>(severity));
}

const char *ToString(ErrorAction action)
{
    return sdk::ToString(static_cast<sdk::ErrorAction>(action));
}

std::string FormatError(ErrorCode code, const std::string &detail)
{
    return sdk::FormatError(static_cast<sdk::ErrorCode>(code), detail);
}

} // namespace soundbridge
