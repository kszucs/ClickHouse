#pragma once

#include "config.h"

#if USE_OPENDAL

#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALObjectStorage.h>
#include <Storages/ObjectStorage/Common.h>
#include <Storages/ObjectStorage/StorageObjectStorageConfiguration.h>

#include <unordered_map>

namespace DB
{

struct OpenDALStorageParsedArguments : private StorageParsedArguments
{
    friend class StorageOpenDALConfiguration;

    static constexpr auto max_number_of_arguments = 5;
    static constexpr auto signatures =
        " - uri\n"
        " - uri, format\n"
        " - uri, format, structure\n"
        " - scheme, config, path, format\n"
        " - scheme, config, path, format, structure\n";

    String scheme;
    std::unordered_map<String, String> config;
    String object_namespace;
    String path;
    /// The URI as written by the user, or `scheme://path` for the explicit form.
    String raw_uri;

    void fromNamedCollection(const NamedCollection & collection, ContextPtr context);
    void fromAST(ASTs & args, ContextPtr context, bool with_structure);
};

/// Configuration for the `opendal(scheme, config, path, format)` table function: a thin
/// SQL-argument-parsing layer over the generic OpenDALObjectStorage. `scheme` selects the
/// OpenDAL service (e.g. "hf"), `config` is a comma-separated "key=value" list of that
/// service's own config options (e.g. "repo_type=datasets,repo_id=org/name,revision=main"),
/// and `path` is the file (or glob) to read within that service/repo.
class StorageOpenDALConfiguration : public StorageObjectStorageConfiguration
{
public:
    static constexpr auto type = ObjectStorageType::OpenDAL;
    static constexpr auto type_name = "opendal";
    static constexpr auto engine_name = "OpenDAL";

    StorageOpenDALConfiguration() = default;

    ObjectStorageType getType() const override { return type; }
    std::string getTypeName() const override { return type_name; }
    std::string getEngineName() const override { return engine_name; }

    Path getRawPath() const override { return path; }
    void setRawPath(const Path & path_) override { path = path_; }
    const String & getRawURI() const override { return raw_uri; }

    const Paths & getPaths() const override { return paths; }
    void setPaths(const Paths & paths_) override { paths = paths_; }

    String getNamespace() const override { return object_namespace; }
    String getDataSourceDescription() const override { return scheme + "://" + object_namespace; }
    StorageObjectStorageQuerySettings getQuerySettings(const ContextPtr &) const override;

    ObjectStoragePtr createObjectStorage(ContextPtr context, bool is_readonly, CredentialsConfigurationCallback refresh_credentials_callback) override;

    void addStructureAndFormatToArgsIfNeeded(ASTs &, const String &, const String &, ContextPtr, bool) override { }

protected:
    void fromNamedCollection(const NamedCollection & collection, ContextPtr context) override;
    void fromAST(ASTs & args, ContextPtr context, bool with_structure) override;

private:
    void initializeFromParsedArguments(OpenDALStorageParsedArguments && parsed_arguments);

    String scheme;
    std::unordered_map<String, String> config;
    String object_namespace;
    String raw_uri;
    Path path;
    Paths paths;
};

}

#endif
