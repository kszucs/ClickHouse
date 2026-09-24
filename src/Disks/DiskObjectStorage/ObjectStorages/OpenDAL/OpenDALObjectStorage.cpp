#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALError.h>
#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALReadBuffer.h>
#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALWriteBuffer.h>

#include <Common/ObjectStorageKeyGenerator.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int FILE_DOESNT_EXIST;
}

namespace
{
    ObjectMetadata toObjectMetadata(const opendal::Metadata & meta)
    {
        ObjectMetadata metadata;
        metadata.size_bytes = meta.content_length;
        if (meta.etag)
            metadata.etag = *meta.etag;
        if (meta.last_modified)
            metadata.last_modified = std::chrono::duration_cast<std::chrono::microseconds>(meta.last_modified->time_since_epoch()).count();
        else
            metadata.is_last_modified_known = false;
        return metadata;
    }
}

OpenDALObjectStorage::OpenDALObjectStorage(
    String scheme_,
    const std::unordered_map<String, String> & config,
    String description_,
    String object_namespace_)
    : scheme(std::move(scheme_))
    , description(std::move(description_))
    , object_namespace(std::move(object_namespace_))
    , operator_(callOpenDAL(description, [&] { return opendal::Operator(scheme, config); }))
    , capability(callOpenDAL(description, [this] { return operator_.Info(); }))
{
}

bool OpenDALObjectStorage::exists(const StoredObject & object) const
{
    return callOpenDAL(description, [&] { return operator_.Exists(object.remote_path); });
}

void OpenDALObjectStorage::listObjects(const std::string & path, RelativePathsWithMetadata & children, size_t max_keys) const
{
    opendal::ListOptions options;
    options.recursive = true;
    auto lister = callOpenDAL(description, [&] { return operator_.GetLister(path, options); });
    while (!max_keys || children.size() < max_keys)
    {
        auto entry = callOpenDAL(description, [&] { return lister.Next(); });
        if (!entry)
            break;
        if (entry->path.ends_with('/'))
            continue;

        auto meta = callOpenDAL(description, [&] { return operator_.Stat(entry->path); });
        children.push_back(std::make_shared<RelativePathWithMetadata>(std::move(entry->path), toObjectMetadata(meta)));
    }
}

ObjectMetadata OpenDALObjectStorage::getObjectMetadata(const std::string & path, bool /* with_tags */) const
{
    /// No OpenDAL service exposes object tags through the binding.
    return toObjectMetadata(callOpenDAL(description, [&] { return operator_.Stat(path); }));
}

std::optional<ObjectMetadata> OpenDALObjectStorage::tryGetObjectMetadata(const std::string & path, bool with_tags) const
{
    try
    {
        return getObjectMetadata(path, with_tags);
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::FILE_DOESNT_EXIST)
            return std::nullopt;
        throw;
    }
}

std::unique_ptr<ReadBufferFromFileBase> OpenDALObjectStorage::readObject( /// NOLINT
    const StoredObject & object,
    const ReadSettings & read_settings,
    std::optional<size_t> /* read_hint */,
    bool /* use_external_buffer */,
    bool /* restrict_seek */) const
{
    size_t object_size = object.bytes_size;
    if (object_size == StoredObject::UnknownSize)
        object_size = getObjectMetadata(object.remote_path, /* with_tags */ false).size_bytes;

    /// With the size known up front, the binding does not issue a stat of its own.
    opendal::ReaderOptions options;
    options.content_length_hint = object_size;
    auto reader = callOpenDAL(description, [&] { return operator_.GetReader(object.remote_path, options); });
    return std::make_unique<OpenDALReadBuffer>(
        std::move(reader), object.remote_path, description, object_size, read_settings.remote_fs_settings.buffer_size);
}

std::unique_ptr<WriteBufferFromFileBase> OpenDALObjectStorage::writeObject( /// NOLINT
    const StoredObject & object, WriteMode mode, std::optional<ObjectAttributes>, size_t buf_size, const WriteSettings &)
{
    if (mode != WriteMode::Rewrite)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "OpenDALObjectStorage only supports WriteMode::Rewrite");
    if (!capability.write)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "{} is read-only (no write-capable credentials configured)", description);
    return std::make_unique<OpenDALWriteBuffer>(operator_, object.remote_path, description, buf_size);
}

void OpenDALObjectStorage::removeObjectIfExists(const StoredObject & object)
{
    removeObjectsIfExist({object});
}

void OpenDALObjectStorage::removeObjectsIfExist(const StoredObjects & objects, StoredObjects * successful_objects)
{
    /// Checked even when every object is absent: a backend that cannot delete should say so.
    if (!capability.delete_feature)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "{} does not support delete (no delete-capable credentials configured)", description);

    std::vector<std::string> paths;
    paths.reserve(objects.size());
    for (const auto & object : objects)
        paths.push_back(object.remote_path);

    /// OpenDAL's delete is idempotent, so absent objects need no existence probe.
    callOpenDAL(description, [&] { operator_.RemoveAll(paths); });

    if (successful_objects)
        successful_objects->insert(successful_objects->end(), objects.begin(), objects.end());
}

void OpenDALObjectStorage::copyObject( /// NOLINT
    const StoredObject & object_from, const StoredObject & object_to, const ReadSettings &, const WriteSettings &, std::optional<ObjectAttributes>)
{
    if (!capability.copy)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "{} does not support a native copy operation", description);
    callOpenDAL(description, [&] { operator_.Copy(object_from.remote_path, object_to.remote_path); });
}

void OpenDALObjectStorage::shutdown() {}
void OpenDALObjectStorage::startup() {}

ObjectStorageKeyGeneratorPtr OpenDALObjectStorage::createKeyGenerator() const
{
    /// Keys are the paths as given: `opendal()` must read exactly the path the user named.
    return createObjectStorageKeyGeneratorByPrefix("");
}

}
