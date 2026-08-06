#pragma once

#include <IO/WriteBufferFromFileBase.h>

#include <opendal.hpp>

namespace DB
{

/// Streams data via repeated opendal::Writer::Write() calls, one per flushed buffer, with
/// a single Close() on finalize - real streaming, not whole-object buffering (unlike
/// OpenDALReadBuffer's own "no streaming, single blocking call" simplification on the read
/// side, which the vendored C++ binding didn't support until this was added).
class OpenDALWriteBuffer : public WriteBufferFromFileBase
{
public:
    OpenDALWriteBuffer(opendal::Operator & operator_, String path_, String description_, size_t buf_size);

    void sync() override { next(); }
    std::string getFileName() const override { return path; }

private:
    void nextImpl() override;
    void finalizeImpl() override;

    String path;
    /// Only used to identify the backend in error messages.
    String description;
    opendal::Writer writer;
};

}
