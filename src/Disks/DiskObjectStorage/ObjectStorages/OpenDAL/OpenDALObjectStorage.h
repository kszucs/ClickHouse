#pragma once

#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Common/Logger.h>

#include <opendal.hpp>

#include <unordered_map>

namespace DB
{

/// Generic IObjectStorage backend on top of Apache OpenDAL: constructed with an OpenDAL
/// service scheme name (e.g. "hf", "s3", "gcs", ...) and its string config map, so the same
/// implementation can back any OpenDAL-supported service - see HFOpenDAL.h for the hf://
/// specific factory that builds the config for this class. Reads stream lazily via
/// OpenDALReadBuffer (real range reads, not whole-file buffering); listing walks directories
/// via List()+Stat() per entry, because the binding's list entries carry no metadata (the
/// binding does expose a recursive list option, which this class does not use yet). Writes
/// stream via repeated Write() calls with a single Close() on finalize (see
/// OpenDALWriteBuffer) - only available when the backend's capability says so (e.g. the "hf"
/// service requires a token). remove()/copyObject() are likewise
/// gated on the backend's own delete/copy capability flags (for "hf", delete requires a
/// token same as write; copy isn't advertised at all, so it always throws there).
class OpenDALObjectStorage : public IObjectStorage
{
public:
    OpenDALObjectStorage(
        String scheme_,
        std::unordered_map<String, String> config_,
        String description_,
        String object_namespace_);

    std::string getName() const override { return "OpenDALObjectStorage(" + scheme + ")"; }

    ObjectStorageType getType() const override { return ObjectStorageType::OpenDAL; }

    std::string getCommonKeyPrefix() const override { return object_namespace; }

    std::string getDescription() const override { return description; }

    bool exists(const StoredObject & object) const override;

    void listObjects(const std::string & path, RelativePathsWithMetadata & children, size_t max_keys) const override;

    ObjectMetadata getObjectMetadata(const std::string & path, bool with_tags) const override;

    /// Differs from getObjectMetadata() only in tolerating a missing object, which it
    /// reports as an empty optional rather than as FILE_DOESNT_EXIST. Every other
    /// failure - a permission denial, a rate limit - still throws, so that callers
    /// which use this to mean "does it exist" cannot mistake "I may not look" for "it
    /// is not there".
    std::optional<ObjectMetadata> tryGetObjectMetadata(const std::string & path, bool with_tags) const override;

    /// Returns the bare OpenDAL-backed buffer. Gathering, prefetching and caching are
    /// layered on by ReadPipeline via the default prepareRead(), so this must not wrap
    /// the buffer itself.
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
    void removeObjectsIfExist(const StoredObjects & objects) override;

    void copyObject( /// NOLINT
        const StoredObject & object_from,
        const StoredObject & object_to,
        const ReadSettings & read_settings,
        const WriteSettings & write_settings,
        std::optional<ObjectAttributes> object_to_attributes = {}) override;

    void shutdown() override;
    void startup() override;

    String getObjectsNamespace() const override { return object_namespace; }

    /// Paths are used verbatim: an OpenDAL service is addressed by the same path the
    /// caller supplies, with no generated key layout on top of it.
    ObjectStorageKeyGeneratorPtr createKeyGenerator() const override;

    bool isRemote() const override { return true; }

    /// Reflects the backend's actual capability (e.g. the "hf" service only advertises
    /// `write` when a token was configured - anonymous access is always read-only).
    bool isReadOnly() const override { return !capability.write; }

private:
    /// Recursively walks `path` (via non-recursive List() + per-file Stat()) appending file
    /// entries to `children`. No caching - each call does fresh calls against the backend,
    /// same as exists()/getObjectMetadata(), which favors correctness/freshness and bounded
    /// memory over a one-shot full-repo prefetch.
    void listRecursive(const std::string & path, RelativePathsWithMetadata & children, size_t max_keys) const;

    const String scheme;
    const std::unordered_map<String, String> config;
    const String description;
    const String object_namespace;
    LoggerPtr log;

    mutable opendal::Operator operator_;
    /// Negotiated once at construction and fixed for the operator's lifetime - cached so
    /// isReadOnly()/writeObject()/removeObjectIfExists()/copyObject() don't cross the FFI
    /// boundary again on every call.
    const opendal::Capability capability;
};

}
