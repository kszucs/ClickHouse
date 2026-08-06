#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALError.h>

#if USE_OPENDAL

#include <Common/Exception.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int ACCESS_DENIED;
    extern const int ARGUMENT_OUT_OF_BOUND;
    extern const int BAD_ARGUMENTS;
    extern const int BAD_FILE_TYPE;
    extern const int FILE_ALREADY_EXISTS;
    extern const int FILE_DOESNT_EXIST;
    extern const int NOT_IMPLEMENTED;
    extern const int OPENDAL_ERROR;
}

namespace
{
    int toErrorCode(opendal::ErrorKind kind)
    {
        switch (kind)
        {
            case opendal::ErrorKind::NotFound:
                return ErrorCodes::FILE_DOESNT_EXIST;
            case opendal::ErrorKind::PermissionDenied:
                return ErrorCodes::ACCESS_DENIED;
            case opendal::ErrorKind::ConfigInvalid:
                return ErrorCodes::BAD_ARGUMENTS;
            case opendal::ErrorKind::AlreadyExists:
                return ErrorCodes::FILE_ALREADY_EXISTS;
            case opendal::ErrorKind::IsADirectory:
            case opendal::ErrorKind::NotADirectory:
                return ErrorCodes::BAD_FILE_TYPE;
            case opendal::ErrorKind::Unsupported:
                return ErrorCodes::NOT_IMPLEMENTED;
            case opendal::ErrorKind::RangeNotSatisfied:
                return ErrorCodes::ARGUMENT_OUT_OF_BOUND;
            /// Everything else - Unexpected, RateLimited, ConditionNotMatch,
            /// IsSameFile, and any kind a newer OpenDAL adds - has no closer
            /// equivalent, so it lands on the backend's own code. This mirrors
            /// how S3_ERROR and AZURE_BLOB_STORAGE_ERROR are used.
            case opendal::ErrorKind::Unexpected:
            case opendal::ErrorKind::RateLimited:
            case opendal::ErrorKind::ConditionNotMatch:
            case opendal::ErrorKind::IsSameFile:
                return ErrorCodes::OPENDAL_ERROR;
        }
        return ErrorCodes::OPENDAL_ERROR;
    }
}

void throwOpenDALError(
    const opendal::Error & error,
    std::string_view operation,
    std::string_view path,
    std::string_view description)
{
    /// The kind name and the temporary flag are kept in the message on purpose:
    /// several kinds collapse onto OPENDAL_ERROR, so without them a log line
    /// could not distinguish a rate limit from an unexpected service failure.
    throw Exception(
        toErrorCode(error.Kind()),
        "OpenDAL {} failed for {} in {}: {}{} ({})",
        operation,
        path.empty() ? "<no path>" : path,
        description,
        opendal::ToStringView(error.Kind()),
        error.IsTemporary() ? " (temporary)" : "",
        error.Message());
}

}

#endif
