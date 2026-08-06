#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALReadBuffer.h>

#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALError.h>
#include <Common/Exception.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int CANNOT_SEEK_THROUGH_FILE;
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
    size_t max_bytes = internal_buffer.size();
    if (read_until_position)
    {
        if (file_offset_of_buffer_end >= *read_until_position)
            return false;
        max_bytes = std::min(max_bytes, *read_until_position - file_offset_of_buffer_end);
    }

    auto bytes_read = callOpenDAL(
        "read",
        file_name,
        description,
        [&] { return reader.Read(internal_buffer.begin(), static_cast<std::streamsize>(max_bytes)); });
    if (bytes_read <= 0)
        return false;

    file_offset_of_buffer_end += static_cast<size_t>(bytes_read);
    working_buffer = internal_buffer;
    working_buffer.resize(static_cast<size_t>(bytes_read));
    return true;
}

off_t OpenDALReadBuffer::seek(off_t offset, int whence)
{
    size_t new_pos;
    if (whence == SEEK_SET)
    {
        if (offset < 0)
            throw Exception(ErrorCodes::CANNOT_SEEK_THROUGH_FILE, "Seek position out of bounds: {}", offset);
        new_pos = static_cast<size_t>(offset);
    }
    else if (whence == SEEK_CUR)
    {
        new_pos = file_offset_of_buffer_end - (working_buffer.end() - pos) + offset;
    }
    else
    {
        throw Exception(ErrorCodes::CANNOT_SEEK_THROUGH_FILE, "Only SEEK_SET and SEEK_CUR seek modes are allowed");
    }

    /// Position is unchanged.
    if (new_pos + (working_buffer.end() - pos) == file_offset_of_buffer_end)
        return static_cast<off_t>(new_pos);

    if (!working_buffer.empty()
        && file_offset_of_buffer_end - working_buffer.size() <= new_pos
        && new_pos <= file_offset_of_buffer_end)
    {
        /// Still inside the already-fetched buffer - no need to re-seek the underlying reader.
        pos = working_buffer.end() - file_offset_of_buffer_end + new_pos;
        return static_cast<off_t>(new_pos);
    }

    auto actual = callOpenDAL(
        "seek",
        file_name,
        description,
        [&] { return reader.Seek(static_cast<std::streamoff>(new_pos), std::ios_base::beg); });
    resetWorkingBuffer();
    file_offset_of_buffer_end = static_cast<size_t>(actual);
    return static_cast<off_t>(actual);
}

off_t OpenDALReadBuffer::getPosition()
{
    return static_cast<off_t>(file_offset_of_buffer_end) - (working_buffer.end() - pos);
}

}
