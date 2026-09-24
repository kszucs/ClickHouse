#pragma once

#include <IO/ReadBufferFromFileBase.h>

#include <opendal.hpp>

#include <optional>

namespace DB
{

/// Reads an object through positioned reads on an opendal::Reader (e.g. HTTP range requests for "hf"),
/// so a seek only moves the offset of the next read.
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
