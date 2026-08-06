#pragma once

#include <memory>

#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALObjectStorage.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>

#include <unordered_map>

namespace DB
{

/// Configuration for the `opendal(scheme, config, path, format)` table function: a thin
/// SQL-argument-parsing layer over the generic OpenDALObjectStorage. `scheme` selects the
/// OpenDAL service (e.g. "hf"), `config` is a comma-separated "key=value" list of that
/// service's own config options (e.g. "repo_type=datasets,repo_id=org/name,revision=main"),
/// and `path` is the file (or glob) to read within that service/repo.
class StorageOpenDALConfiguration : public StorageObjectStorage::Configuration
{
public:
    using ConfigurationPtr = StorageObjectStorage::ConfigurationPtr;

    static constexpr auto type_name = "opendal";

    StorageOpenDALConfiguration() = default;
    StorageOpenDALConfiguration(const StorageOpenDALConfiguration & other) = default;

    std::string getTypeName() const override { return type_name; }
    std::string getEngineName() const override { return "OpenDAL"; }

    Path getPath() const override { return path; }
    void setPath(const Path & path_) override { path = path_; }

    const Paths & getPaths() const override { return paths; }
    void setPaths(const Paths & paths_) override { paths = paths_; }

    String getNamespace() const override { return object_namespace; }
    String getDataSourceDescription() const override { return scheme; }
    StorageObjectStorage::QuerySettings getQuerySettings(const ContextPtr &) const override;

    ConfigurationPtr clone() override { return std::make_shared<StorageOpenDALConfiguration>(*this); }

    ObjectStoragePtr createObjectStorage(ContextPtr context, bool is_readonly) override;

    void addStructureAndFormatToArgs(ASTs &, const String &, const String &, ContextPtr) override { }

private:
    void fromNamedCollection(const NamedCollection & collection, ContextPtr context) override;
    void fromAST(ASTs & args, ContextPtr context, bool with_structure) override;

    String scheme;
    std::unordered_map<String, String> config;
    String object_namespace;
    Path path;
    Paths paths;
};

}
