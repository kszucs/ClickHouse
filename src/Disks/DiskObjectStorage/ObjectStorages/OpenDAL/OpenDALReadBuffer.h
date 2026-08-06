#pragma once

#include <IO/ReadBufferFromFileBase.h>

#include <opendal.hpp>

#include <optional>

namespace DB
{

/// Streams data lazily from an opendal::Reader, translating seeks into opendal's own
/// range-read machinery (which for e.g. the "hf" service means real HTTP Range requests,
/// or ranged Xet chunk downloads) rather than buffering whole objects into memory.
/// Modeled on ReadBufferFromFileDescriptor's nextImpl/seek pattern.
class OpenDALReadBuffer : public ReadBufferFromFileBase
{
public:
    OpenDALReadBuffer(opendal::Reader reader_, String file_name_, String description_, size_t file_size_, size_t buf_size);

    bool nextImpl() override;
    off_t seek(off_t offset, int whence) override;
    off_t getPosition() override;

    std::string getFileName() const override { return file_name; }
    size_t getFileOffsetOfBufferEnd() const override { return file_offset_of_buffer_end; }
    void setReadUntilPosition(size_t position) override { read_until_position = position; }
    bool supportsRightBoundedReads() const override { return true; }

private:
    opendal::Reader reader;
    String file_name;
    /// Only used to identify the backend in error messages.
    String description;
    size_t file_offset_of_buffer_end = 0;
    std::optional<size_t> read_until_position;
};

}
