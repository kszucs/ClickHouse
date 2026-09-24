#pragma once

#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>

#include <opendal.hpp>

#include <unordered_map>

namespace DB
{

/// Generic IObjectStorage over an Apache OpenDAL service, selected by its scheme (e.g. "hf", "fs")
/// and configured with that service's own options. Write, delete and copy are available only when
/// the service's capabilities say so (e.g. "hf" needs a token to write).
class OpenDALObjectStorage : public IObjectStorage
{
public:
    OpenDALObjectStorage(
        String scheme_,
        const std::unordered_map<String, String> & config,
        String description_,
        String object_namespace_);

    std::string getName() const override { return "OpenDALObjectStorage(" + scheme + ")"; }

    ObjectStorageType getType() const override { return ObjectStorageType::OpenDAL; }

    std::string getCommonKeyPrefix() const override { return object_namespace; }

    std::string getDescription() const override { return description; }

    bool exists(const StoredObject & object) const override;

    void listObjects(const std::string & path, RelativePathsWithMetadata & children, size_t max_keys) const override;

    ObjectMetadata getObjectMetadata(const std::string & path, bool with_tags) const override;

    /// Tolerates only a missing object: any other failure, such as a permission denial, still throws.
    std::optional<ObjectMetadata> tryGetObjectMetadata(const std::string & path, bool with_tags) const override;

    std::unique_ptr<ReadBufferFromFileBase> readObject( /// NOLINT
        const StoredObject & object,
        const ReadSettings & read_settings,
        std::optional<size_t> read_hint = {},
        bool use_external_buffer = false,
        bool restrict_seek = false) const override;

    std::unique_ptr<WriteBufferFromFileBase> writeObject( /// NOLINT
        const StoredObject & object,
        WriteMode mode,
        std::optional<ObjectAttributes> attributes = {},
        size_t buf_size = DBMS_DEFAULT_BUFFER_SIZE,
        const WriteSettings & write_settings = {}) override;

    void removeObjectIfExists(const StoredObject & object) override;
    void removeObjectsIfExist(const StoredObjects & objects, StoredObjects * successful_objects = nullptr) override; /// NOLINT

    void copyObject( /// NOLINT
        const StoredObject & object_from,
        const StoredObject & object_to,
        const ReadSettings & read_settings,
        const WriteSettings & write_settings,
        std::optional<ObjectAttributes> object_to_attributes = {}) override;

    void shutdown() override;
    void startup() override;

    String getObjectsNamespace() const override { return object_namespace; }

    ObjectStorageKeyGeneratorPtr createKeyGenerator() const override;

    bool isRemote() const override { return true; }

    bool isReadOnly() const override { return !capability.write; }

private:
    const String scheme;
    const String description;
    const String object_namespace;

    mutable opendal::Operator operator_;
    const opendal::Capability capability;
};

}
