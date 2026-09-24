#pragma once

#include <IO/WriteBufferFromFileBase.h>

#include <opendal.hpp>

namespace DB
{

/// Streams data with one opendal::Writer::Write() per flushed buffer and a single Close() on finalize.
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
