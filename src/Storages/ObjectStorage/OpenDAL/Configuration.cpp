#include <Storages/ObjectStorage/OpenDAL/Configuration.h>

#include <Core/Settings.h>
#include <Interpreters/Context.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Storages/checkAndGetLiteralArgument.h>
#include <Common/NamedCollections/NamedCollections.h>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/split.hpp>

namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int BAD_ARGUMENTS;
}

namespace
{
    /// Parses "key1=value1,key2=value2" into a config map.
    std::unordered_map<String, String> parseConfigString(const String & config_str)
    {
        std::unordered_map<String, String> result;
        std::vector<String> pairs;
        boost::split(pairs, config_str, boost::is_any_of(","), boost::token_compress_on);
        for (const auto & pair : pairs)
        {
            if (pair.empty())
                continue;
            auto eq_pos = pair.find('=');
            if (eq_pos == String::npos)
                throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Expected 'key=value' pairs in OpenDAL config, got: {}", pair);
            result.emplace(pair.substr(0, eq_pos), pair.substr(eq_pos + 1));
        }
        return result;
    }

    struct ParsedUri
    {
        String scheme;
        std::unordered_map<String, String> config;
        String path;
        String object_namespace;
    };

    /// hf://<repo_type>/<org>/<name>[@<revision>]/<path/to/file>
    /// Revisions containing '/' (e.g. "refs/convert/parquet") aren't representable in this
    /// shorthand - use the explicit (scheme, config, path, format) form for those instead.
    ParsedUri parseHFUri(const String & rest)
    {
        std::vector<String> parts;
        boost::split(parts, rest, boost::is_any_of("/"));
        if (parts.size() < 4)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Expected hf://<repo_type>/<org>/<name>[@<revision>]/<path>, got: hf://{}", rest);

        const String & repo_type = parts[0];
        if (repo_type != "datasets" && repo_type != "models" && repo_type != "spaces")
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "hf:// URI has unknown repo type '{}', expected one of datasets/models/spaces", repo_type);

        const String & org = parts[1];
        String name = parts[2];
        String revision = "main";
        if (auto at_pos = name.find('@'); at_pos != String::npos)
        {
            revision = name.substr(at_pos + 1);
            name.resize(at_pos);
        }

        String repo_id = org + "/" + name;
        String path = boost::join(std::vector<String>(parts.begin() + 3, parts.end()), "/");
        if (path.empty())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "hf:// URI is missing a file path: hf://{}", rest);

        return ParsedUri{
            .scheme = "hf",
            .config = {{"repo_type", repo_type}, {"repo_id", repo_id}, {"revision", revision}},
            .path = path,
            .object_namespace = repo_id};
    }

    ParsedUri parseUri(const String & uri)
    {
        auto scheme_pos = uri.find("://");
        if (scheme_pos == String::npos)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Expected a '<scheme>://...' URI, got: {}", uri);

        String scheme = uri.substr(0, scheme_pos);
        String rest = uri.substr(scheme_pos + 3);

        /// Optional "?key=value&key2=value2" suffix, merged into the config map - e.g.
        /// 'hf://datasets/org/name/path.parquet?download_mode=http'.
        std::unordered_map<String, String> query_config;
        if (auto query_pos = rest.find('?'); query_pos != String::npos)
        {
            String query_str = rest.substr(query_pos + 1);
            rest.resize(query_pos);
            query_config = parseConfigString(boost::replace_all_copy(query_str, "&", ","));
        }

        ParsedUri result;
        /// Add further schemes here as more OpenDAL services are wired in (s3, gcs, ...).
        if (scheme == "hf")
            result = parseHFUri(rest);
        else
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "No URI shorthand for OpenDAL scheme '{}' - use the explicit (scheme, config, path, format) form instead", scheme);

        for (auto & [key, value] : query_config)
            result.config[key] = value;
        return result;
    }
}

void StorageOpenDALConfiguration::fromNamedCollection(const NamedCollection & collection, ContextPtr)
{
    scheme = collection.get<String>("scheme");
    config = parseConfigString(collection.getOrDefault<String>("config", ""));
    path = collection.get<String>("path");
    format = collection.getOrDefault<String>("format", "auto");
    compression_method = collection.getOrDefault<String>("compression_method", collection.getOrDefault<String>("compression", "auto"));
    structure = collection.getOrDefault<String>("structure", "auto");
    object_namespace = config.contains("repo_id") ? config.at("repo_id") : "";
    paths = {path};
}

void StorageOpenDALConfiguration::fromAST(ASTs & args, ContextPtr context, bool /* with_structure */)
{
    if (args.empty() || args.size() > 5)
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
            "OpenDAL table function requires either (uri[, format[, structure]]) "
            "or (scheme, config, path, format[, structure]) arguments");

    for (auto & arg : args)
        arg = evaluateConstantExpressionOrIdentifierAsLiteral(arg, context);

    if (args.size() <= 3)
    {
        /// Convenient shorthand: a single URI encodes scheme, config and path together,
        /// e.g. opendal('hf://datasets/org/name/path/to/file.parquet').
        auto parsed = parseUri(checkAndGetLiteralArgument<String>(args[0], "uri"));
        scheme = parsed.scheme;
        config = parsed.config;
        path = parsed.path;
        object_namespace = parsed.object_namespace;

        if (args.size() > 1)
            format = checkAndGetLiteralArgument<String>(args[1], "format");
        if (args.size() > 2)
            structure = checkAndGetLiteralArgument<String>(args[2], "structure");
    }
    else
    {
        /// Explicit form: full control over the config map for any OpenDAL scheme.
        scheme = checkAndGetLiteralArgument<String>(args[0], "scheme");
        config = parseConfigString(checkAndGetLiteralArgument<String>(args[1], "config"));
        path = checkAndGetLiteralArgument<String>(args[2], "path");
        format = checkAndGetLiteralArgument<String>(args[3], "format");
        if (args.size() > 4)
            structure = checkAndGetLiteralArgument<String>(args[4], "structure");

        object_namespace = config.contains("repo_id") ? config.at("repo_id") : "";
    }

    paths = {path};
}

ObjectStoragePtr StorageOpenDALConfiguration::createObjectStorage(ContextPtr, bool)
{
    return std::make_shared<OpenDALObjectStorage>(scheme, config, scheme + "://" + object_namespace, object_namespace);
}

StorageObjectStorage::QuerySettings StorageOpenDALConfiguration::getQuerySettings(const ContextPtr & context) const
{
    const auto & settings = context->getSettingsRef();
    return StorageObjectStorage::QuerySettings{
        .truncate_on_insert = settings.opendal_truncate_on_insert,
        .create_new_file_on_insert = settings.opendal_create_new_file_on_insert,
        .schema_inference_use_cache = settings.schema_inference_use_cache_for_opendal,
        .schema_inference_mode = settings.schema_inference_mode,
        .skip_empty_files = settings.opendal_skip_empty_files,
        .list_object_keys_size = 0,
        .throw_on_zero_files_match = false,
        .ignore_non_existent_file = false};
}

}
