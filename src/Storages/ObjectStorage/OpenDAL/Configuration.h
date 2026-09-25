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

    static constexpr auto signatures =
        " - scheme, path = 'path', [format = 'format', structure = 'structure', compression_method = 'method',] [service_option = 'value', ...]\n"
        " - named_collection, [key = 'value', ...]\n";

    String scheme;
    String path;
    /// Options of the OpenDAL service itself, e.g. `repo_id` for "hf".
    std::unordered_map<String, String> config;

    void fromNamedCollection(const NamedCollection & collection, ContextPtr context);
    void fromAST(ASTs & args, ContextPtr context, bool with_structure);

private:
    /// Consumes `key` if it is one of ClickHouse's own arguments, otherwise stores it as a service option.
    void setArgument(const String & key, String value);
};

/// Configuration for the `opendal` table function: selects an OpenDAL service by its scheme and
/// passes every argument other than `path`, `format`, `structure` and `compression_method` to it.
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

    void check(ContextPtr context) override;

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
