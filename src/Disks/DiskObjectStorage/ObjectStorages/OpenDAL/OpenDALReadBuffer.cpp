#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALReadBuffer.h>

#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALError.h>
#include <Common/Exception.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_SEEK_THROUGH_FILE;
    extern const int SEEK_POSITION_OUT_OF_BOUND;
}

OpenDALReadBuffer::OpenDALReadBuffer(
    opendal::Reader reader_, String file_name_, String description_, size_t file_size_, size_t buf_size)
    : ReadBufferFromFileBase(buf_size, nullptr, 0, file_size_)
    , reader(std::move(reader_))
    , file_name(std::move(file_name_))
    , description(std::move(description_))
{
}

bool OpenDALReadBuffer::nextImpl()
{
    size_t end = read_until_position ? std::min(*read_until_position, *file_size) : *file_size;
    if (file_offset_of_buffer_end >= end)
        return false;
    size_t max_bytes = std::min(internal_buffer.size(), end - file_offset_of_buffer_end);

    auto bytes_read = callOpenDAL(
        description,
        [&] { return reader.ReadAt(internal_buffer.begin(), static_cast<std::streamsize>(max_bytes), file_offset_of_buffer_end); });
    if (bytes_read <= 0)
        return false;

    file_offset_of_buffer_end += static_cast<size_t>(bytes_read);
    working_buffer = internal_buffer;
    working_buffer.resize(static_cast<size_t>(bytes_read));
    return true;
}

off_t OpenDALReadBuffer::seek(off_t offset, int whence)
{
    if (whence != SEEK_SET)
        throw Exception(ErrorCodes::CANNOT_SEEK_THROUGH_FILE, "Only SEEK_SET mode is allowed");
    if (offset < 0)
        throw Exception(ErrorCodes::SEEK_POSITION_OUT_OF_BOUND, "Seek position is out of bounds. Offset: {}", offset);

    auto new_pos = static_cast<size_t>(offset);
    if (!working_buffer.empty() && file_offset_of_buffer_end - working_buffer.size() <= new_pos && new_pos <= file_offset_of_buffer_end)
    {
        /// Still inside the already-fetched buffer.
        pos = working_buffer.end() - (file_offset_of_buffer_end - new_pos);
        return offset;
    }

    resetWorkingBuffer();
    file_offset_of_buffer_end = new_pos;
    return offset;
}

off_t OpenDALReadBuffer::getPosition()
{
    return static_cast<off_t>(file_offset_of_buffer_end) - (working_buffer.end() - pos);
}

}
