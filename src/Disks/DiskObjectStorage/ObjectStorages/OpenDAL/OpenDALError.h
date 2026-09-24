#pragma once

#include <opendal.hpp>

#include <string_view>
#include <utility>

namespace DB
{

/// Translates an OpenDAL failure into a DB::Exception with the error code matching its `ErrorKind`.
/// `description` identifies the storage; OpenDAL's message already names the operation and the path.
[[noreturn]] void throwOpenDALError(const opendal::Error & error, std::string_view description);

/// Invokes an OpenDAL call, translating any failure via throwOpenDALError().
template <typename F>
decltype(auto) callOpenDAL(std::string_view description, F && f)
{
    try
    {
        return std::forward<F>(f)();
    }
    catch (const opendal::Error & e)
    {
        throwOpenDALError(e, description);
    }
}

}
