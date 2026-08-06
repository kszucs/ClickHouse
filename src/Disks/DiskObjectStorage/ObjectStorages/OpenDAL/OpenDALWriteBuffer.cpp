#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALWriteBuffer.h>

#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALError.h>

namespace DB
{

OpenDALWriteBuffer::OpenDALWriteBuffer(
    opendal::Operator & operator_, String path_, String description_, size_t buf_size)
    : WriteBufferFromFileBase(buf_size, nullptr, 0)
    , path(std::move(path_))
    , description(std::move(description_))
    , writer(callOpenDAL("writer", path, description, [&] { return operator_.GetWriter(path); }))
{
}

void OpenDALWriteBuffer::nextImpl()
{
    if (!offset())
        return;
    callOpenDAL(
        "write",
        path,
        description,
        [&] { writer.Write(std::string_view(working_buffer.begin(), offset())); });
}

void OpenDALWriteBuffer::finalizeImpl()
{
    next();
    callOpenDAL("close", path, description, [&] { writer.Close(); });
}

}
