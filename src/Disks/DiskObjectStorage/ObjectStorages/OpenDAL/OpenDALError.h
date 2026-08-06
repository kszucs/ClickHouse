#pragma once

#include "config.h"

#if USE_OPENDAL

#include <base/types.h>

#include <opendal.hpp>

#include <string_view>
#include <utility>

namespace DB
{

/// Translates an OpenDAL failure into a DB::Exception with a matching error code.
///
/// The binding hands us `opendal::Error`, which carries the operation's own
/// `ErrorKind` (NotFound, PermissionDenied, RateLimited, ...) rather than just a
/// message. That is what lets callers here behave differently per category -
/// schema inference needs to tell "no such file" apart from "no permission", and
/// retry logic needs to tell a transient failure apart from a permanent one -
/// instead of matching on error text.
///
/// `operation` names the OpenDAL call that failed ("stat", "read", "list", ...);
/// `path` and `description` identify what it was operating on.
[[noreturn]] void throwOpenDALError(
    const opendal::Error & error,
    std::string_view operation,
    std::string_view path,
    std::string_view description);

/// Invoke an OpenDAL call, translating any failure via throwOpenDALError().
///
/// Every call into the binding from OpenDALObjectStorage and its buffers goes
/// through here, so an untranslated opendal::Error (which would surface as a
/// bare STD_EXCEPTION with no usable code) cannot escape into the rest of the
/// server.
template <typename F>
decltype(auto) callOpenDAL(
    std::string_view operation,
    std::string_view path,
    std::string_view description,
    F && f)
{
    try
    {
        return std::forward<F>(f)();
    }
    catch (const opendal::Error & e)
    {
        throwOpenDALError(e, operation, path, description);
    }
}

}

#endif
