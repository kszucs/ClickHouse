#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALError.h>
#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALReadBuffer.h>
#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALWriteBuffer.h>

#include <Common/ObjectStorageKeyGenerator.h>
#include <Common/logger_useful.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int FILE_DOESNT_EXIST;
}

namespace
{
    /// Shared by getObjectMetadata() and the per-entry Stat() during listing, so both
    /// report the same fields for the same object.
    ObjectMetadata toObjectMetadata(const opendal::Metadata & meta)
    {
        ObjectMetadata metadata;
        metadata.size_bytes = meta.content_length;
        if (meta.etag)
            metadata.etag = *meta.etag;
        return metadata;
    }

    /// The operator and its capabilities are built in the member initialiser list,
    /// which runs before the constructor body could catch anything - so the
    /// translation has to happen inside the initialiser itself.
    opendal::Operator makeOperator(
        const String & scheme, const std::unordered_map<String, String> & config, const String & description)
    {
        return callOpenDAL("new_operator", /* path */ "", description, [&] { return opendal::Operator(scheme, config); });
    }

    /// Backends disagree on whether a directory listing includes the directory
    /// itself, and on how they spell it: listing "" on "fs" yields an entry "/",
    /// while "memory" omits the root altogether. Comparing the paths verbatim
    /// fails to recognise "/" as the root that was just listed, so listRecursive()
    /// descends into it and walks the whole tree a second time - on "fs" that
    /// returned every file twice.
    bool isSameDirectory(std::string_view entry_path, std::string_view path)
    {
        auto trim = [](std::string_view p)
        {
            while (!p.empty() && p.front() == '/')
                p.remove_prefix(1);
            while (!p.empty() && p.back() == '/')
                p.remove_suffix(1);
            return p;
        };
        return trim(entry_path) == trim(path);
    }
}

OpenDALObjectStorage::OpenDALObjectStorage(
    String scheme_,
    std::unordered_map<String, String> config_,
    String description_,
    String object_namespace_)
    : scheme(std::move(scheme_))
    , config(std::move(config_))
    , description(std::move(description_))
    , object_namespace(std::move(object_namespace_))
    , log(getLogger("OpenDALObjectStorage"))
    , operator_(makeOperator(scheme, config, description))
    , capability(callOpenDAL("info", /* path */ "", description, [this] { return operator_.Info(); }))
{
}

bool OpenDALObjectStorage::exists(const StoredObject & object) const
{
    return callOpenDAL("exists", object.remote_path, description, [&] { return operator_.Exists(object.remote_path); });
}

void OpenDALObjectStorage::listRecursive(const std::string & path, RelativePathsWithMetadata & children, size_t max_keys) const
{
    for (auto & entry : callOpenDAL("list", path, description, [&] { return operator_.List(path); }))
    {
        if (max_keys && children.size() >= max_keys)
            return;

        if (entry.path.ends_with('/'))
        {
            if (!isSameDirectory(entry.path, path))
                listRecursive(entry.path, children, max_keys);
            continue;
        }

        auto meta = callOpenDAL("stat", entry.path, description, [&] { return operator_.Stat(entry.path); });
        children.push_back(std::make_shared<RelativePathWithMetadata>(entry.path, toObjectMetadata(meta)));
    }
}

void OpenDALObjectStorage::listObjects(const std::string & path, RelativePathsWithMetadata & children, size_t max_keys) const
{
    listRecursive(path, children, max_keys);
}

ObjectMetadata OpenDALObjectStorage::getObjectMetadata(const std::string & path, bool /* with_tags */) const
{
    /// A missing object arrives here as ErrorKind::NotFound and so still maps to
    /// FILE_DOESNT_EXIST, but a permission or rate-limit failure now keeps its own
    /// code instead of being reported as a missing file.
    ///
    /// Tags are ignored: no OpenDAL service exposes object tagging through the
    /// binding, so there is nothing to fetch even when the caller asks for them.
    return toObjectMetadata(callOpenDAL("stat", path, description, [&] { return operator_.Stat(path); }));
}

std::optional<ObjectMetadata> OpenDALObjectStorage::tryGetObjectMetadata(const std::string & path, bool with_tags) const
{
    /// Only NotFound is swallowed. Anything else - notably PermissionDenied - keeps
    /// propagating, so "I am not allowed to see this" is never silently reported as
    /// "this does not exist". That distinction is exactly what the typed ErrorKind
    /// coming across the FFI boundary buys us.
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
    std::optional<size_t> read_hint,
    bool /* use_external_buffer */,
    bool /* restrict_seek */) const
{
    size_t object_size = read_hint.value_or(object.bytes_size);
    if (object_size == 0)
    {
        /// Some callers (e.g. ReadBufferIterator::recreateLastReadBuffer(), used during schema
        /// inference) construct a StoredObject with no size at all. Fall back to a real Stat()
        /// rather than silently treating the object as empty.
        object_size = getObjectMetadata(object.remote_path, /* with_tags */ false).size_bytes;
    }

    const auto patched = patchSettings(read_settings);
    size_t buf_size = patched.remote_fs_buffer_size ? patched.remote_fs_buffer_size : DBMS_DEFAULT_BUFFER_SIZE;

    auto reader = callOpenDAL(
        "reader", object.remote_path, description, [&] { return operator_.GetReader(object.remote_path); });
    return std::make_unique<OpenDALReadBuffer>(
        std::move(reader), object.remote_path, description, object_size, buf_size);
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
    /// The capability check stays ahead of the existence check on purpose: a backend
    /// that cannot delete at all should say so, rather than silently succeeding for
    /// objects that happen to be absent and failing only for those that are not.
    if (!capability.delete_feature)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "{} does not support delete (no delete-capable credentials configured)", description);

    /// OpenDAL's delete is already idempotent - removing an absent object is not an
    /// error for the services tested - so this needs no existence probe of its own.
    callOpenDAL("remove", object.remote_path, description, [&] { operator_.Remove(object.remote_path); });
}

void OpenDALObjectStorage::removeObjectsIfExist(const StoredObjects & objects)
{
    for (const auto & object : objects)
        removeObjectIfExists(object);
}

void OpenDALObjectStorage::copyObject( /// NOLINT
    const StoredObject & object_from, const StoredObject & object_to, const ReadSettings &, const WriteSettings &, std::optional<ObjectAttributes>)
{
    if (!capability.copy)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "{} does not support a native copy operation", description);
    callOpenDAL(
        "copy",
        object_from.remote_path,
        description,
        [&] { operator_.Copy(object_from.remote_path, object_to.remote_path); });
}

void OpenDALObjectStorage::shutdown() {}
void OpenDALObjectStorage::startup() {}

ObjectStorageKeyGeneratorPtr OpenDALObjectStorage::createKeyGenerator() const
{
    /// An empty prefix yields keys identical to the paths handed in, which is what
    /// the table function needs: `opendal('hf', path = 'data/x.parquet')` must read
    /// exactly that path on the service, not a generated blob name.
    return createObjectStorageKeyGeneratorByPrefix("");
}

}
