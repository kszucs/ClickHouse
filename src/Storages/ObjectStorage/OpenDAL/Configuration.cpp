#include <Storages/ObjectStorage/OpenDAL/Configuration.h>

#include <Core/Settings.h>
#include <Interpreters/Context.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Storages/NamedCollectionsHelpers.h>
#include <Storages/checkAndGetLiteralArgument.h>
#include <Common/FieldVisitorToString.h>
#include <Common/NamedCollections/NamedCollections.h>
#include <Common/RemoteHostFilter.h>
#include <Common/filesystemHelpers.h>

#include <Poco/URI.h>

#include <filesystem>
#include <map>
#include <ranges>
#include <span>

namespace fs = std::filesystem;

namespace DB
{

namespace Setting
{
    extern const SettingsBool opendal_create_new_file_on_insert;
    extern const SettingsBool opendal_skip_empty_files;
    extern const SettingsBool opendal_truncate_on_insert;
    extern const SettingsSchemaInferenceMode schema_inference_mode;
    extern const SettingsBool schema_inference_use_cache_for_opendal;
}

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int DATABASE_ACCESS_DENIED;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

namespace
{
    /// The services the table function supports, each with the option that names where its data lives,
    /// which makes the namespace of its paths (like the bucket of S3).
    const std::map<std::string_view, std::string_view> supported_services{{"fs", "root"}, {"hf", "repo_id"}};
}

void OpenDALStorageParsedArguments::setArgument(const String & key, String value)
{
    if (key == "path")
        path = std::move(value);
    else if (key == "format")
        format = std::move(value);
    else if (key == "structure")
        structure = std::move(value);
    else if (key == "compression_method" || key == "compression")
        compression_method = std::move(value);
    else
        config[key] = std::move(value);
}

void OpenDALStorageParsedArguments::fromNamedCollection(const NamedCollection & collection, ContextPtr)
{
    for (const auto & key : collection.getKeys())
    {
        if (key == "scheme")
            scheme = collection.get<String>(key);
        else
            setArgument(key, collection.get<String>(key));
    }
}

void OpenDALStorageParsedArguments::fromAST(ASTs & args, ContextPtr context, bool /* with_structure */)
{
    if (args.empty())
        throw Exception(
            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "Table function opendal requires arguments. All supported signatures:\n{}",
            signatures);

    args[0] = evaluateConstantExpressionOrIdentifierAsLiteral(args[0], context);
    scheme = checkAndGetLiteralArgument<String>(args[0], "scheme");

    for (const auto & arg : std::span(args).subspan(1))
    {
        auto [key, value] = getKeyValueFromAST(arg, context);
        setArgument(key, convertFieldToString(value));
    }
}

void StorageOpenDALConfiguration::initializeFromParsedArguments(OpenDALStorageParsedArguments && parsed_arguments)
{
    StorageObjectStorageConfiguration::initializeFromParsedArguments(parsed_arguments);

    scheme = std::move(parsed_arguments.scheme);
    const auto service = supported_services.find(scheme);
    if (service == supported_services.end())
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "OpenDAL service '{}' is not supported by table function opendal. Supported services: {}",
            scheme, fmt::join(std::views::keys(supported_services), ", "));
    if (parsed_arguments.path.empty())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Table function opendal requires a `path` argument");

    config = std::move(parsed_arguments.config);
    if (scheme == "hf")
    {
        /// The environment of the server must not leak into requests: its Hugging Face token,
        /// or an endpoint that `remote_url_allow_hosts` would not see.
        config["disable_config_load"] = "true";
        config.try_emplace("endpoint", "https://huggingface.co");
    }

    auto namespace_option = config.find(String(service->second));
    object_namespace = namespace_option == config.end() ? "" : namespace_option->second;
    path = std::move(parsed_arguments.path);
    raw_uri = getDataSourceDescription() + "/" + path.path;
    paths = {path};
}

void StorageOpenDALConfiguration::fromNamedCollection(const NamedCollection & collection, ContextPtr context)
{
    OpenDALStorageParsedArguments parsed_arguments;
    parsed_arguments.fromNamedCollection(collection, context);
    initializeFromParsedArguments(std::move(parsed_arguments));
}

void StorageOpenDALConfiguration::fromAST(ASTs & args, ContextPtr context, bool with_structure)
{
    OpenDALStorageParsedArguments parsed_arguments;
    parsed_arguments.fromAST(args, context, with_structure);
    initializeFromParsedArguments(std::move(parsed_arguments));
}

void StorageOpenDALConfiguration::check(ContextPtr context)
{
    if (auto endpoint = config.find("endpoint"); endpoint != config.end())
        context->getGlobalContext()->getRemoteHostFilter().checkURL(Poco::URI(endpoint->second));
    StorageObjectStorageConfiguration::check(context);
}

ObjectStoragePtr StorageOpenDALConfiguration::createObjectStorage(ContextPtr context, bool, CredentialsConfigurationCallback)
{
    auto service_config = config;
    if (scheme == "fs" && context->getApplicationType() != Context::ApplicationType::LOCAL)
    {
        /// Like the `file` table function, a server only reads and writes under `user_files_path`.
        const fs::path user_files_path = context->getUserFilesPath();
        const auto root = fs::weakly_canonical(user_files_path / service_config["root"]).string();
        if (!fileOrSymlinkPathStartsWith(root, user_files_path))
            throw Exception(
                ErrorCodes::DATABASE_ACCESS_DENIED,
                "OpenDAL fs root `{}` is not inside `{}`",
                root, user_files_path.string());
        service_config["root"] = root;
    }
    return std::make_shared<OpenDALObjectStorage>(scheme, service_config, getDataSourceDescription(), object_namespace);
}

StorageObjectStorageQuerySettings StorageOpenDALConfiguration::getQuerySettings(const ContextPtr & context) const
{
    const auto & settings = context->getSettingsRef();
    return StorageObjectStorageQuerySettings{
        .truncate_on_insert = settings[Setting::opendal_truncate_on_insert],
        .create_new_file_on_insert = settings[Setting::opendal_create_new_file_on_insert],
        .schema_inference_use_cache = settings[Setting::schema_inference_use_cache_for_opendal],
        .schema_inference_mode = settings[Setting::schema_inference_mode],
        .skip_empty_files = settings[Setting::opendal_skip_empty_files],
        .list_object_keys_size = 0,
        .throw_on_zero_files_match = false,
        .ignore_non_existent_file = false};
}

}
