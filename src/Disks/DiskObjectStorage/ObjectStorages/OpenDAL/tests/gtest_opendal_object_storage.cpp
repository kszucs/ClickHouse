#include "config.h"

#if USE_OPENDAL

#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/ObjectStorages/OpenDAL/OpenDALObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>
#include <IO/ReadBufferFromFileBase.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/ReadHelpers.h>
#include <Common/Exception.h>

#include <algorithm>
#include <filesystem>
#include <vector>
#include <unistd.h>

using namespace DB;

namespace DB::ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int NOT_IMPLEMENTED;
}

#define EXPECT_THROW_ERROR_CODE(statement, expected_code) \
    EXPECT_THROW( \
        try \
        { \
            statement; \
        } \
        catch (const Exception & e) \
        { \
            EXPECT_EQ(expected_code, e.code()); \
            throw; \
        }, \
        Exception)

namespace
{

/// The backends this suite runs the IObjectStorage contract against. Both are
/// pure-Rust and need no network, so these tests are safe to run anywhere.
struct Backend
{
    /// OpenDAL service name.
    std::string scheme;
    /// Backends without copy support must report NOT_IMPLEMENTED rather than silently do nothing.
    bool supports_copy;
};

std::ostream & operator<<(std::ostream & out, const Backend & backend)
{
    return out << backend.scheme;
}

class OpenDALObjectStorageTest : public ::testing::TestWithParam<Backend>
{
protected:
    void SetUp() override
    {
        const auto & backend = GetParam();
        std::unordered_map<String, String> config;

        /// "fs" stores under a real directory; "memory" is self-contained.
        if (backend.scheme == "fs")
        {
            /// Include the test name and pid so that concurrently running tests
            /// cannot collide on the same directory.
            const auto * info = ::testing::UnitTest::GetInstance()->current_test_info();
            root = std::filesystem::temp_directory_path()
                / fmt::format("ch_opendal_{}_{}", info->name(), getpid());
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root);
            config["root"] = root.string();
        }

        storage = std::make_unique<OpenDALObjectStorage>(
            backend.scheme, config, backend.scheme + "://gtest", /* object_namespace */ "");
    }

    void TearDown() override
    {
        storage.reset();
        if (!root.empty())
            std::filesystem::remove_all(root);
    }

    void write(const std::string & path, const std::string & content)
    {
        auto buffer = storage->writeObject(StoredObject(path), WriteMode::Rewrite);
        buffer->write(content.data(), content.size());
        buffer->finalize();
    }

    std::string read(const std::string & path)
    {
        auto metadata = storage->getObjectMetadata(path, /* with_tags */ false);
        auto buffer = storage->readObject(StoredObject(path, /* local_path */ "", metadata.size_bytes), ReadSettings{});
        std::string content;
        readStringUntilEOF(content, *buffer);
        return content;
    }

    std::filesystem::path root;
    std::unique_ptr<OpenDALObjectStorage> storage;
};

}

TEST_P(OpenDALObjectStorageTest, WriteReadRoundTrip)
{
    write("greeting.txt", "hello opendal");
    EXPECT_EQ(read("greeting.txt"), "hello opendal");
}

TEST_P(OpenDALObjectStorageTest, CancelledWriteLeavesNoObject)
{
    /// "fs" writes the file in place, like the `file` table function.
    if (GetParam().scheme == "fs")
        GTEST_SKIP() << "fs does not stage writes";

    auto buffer = storage->writeObject(StoredObject("cancelled.txt"), WriteMode::Rewrite);
    buffer->write("partial", 7);
    buffer->next();
    buffer->cancel();
    EXPECT_FALSE(storage->exists(StoredObject("cancelled.txt")));
}

TEST_P(OpenDALObjectStorageTest, ExistsReflectsWrites)
{
    EXPECT_FALSE(storage->exists(StoredObject("absent.txt")));
    write("present.txt", "x");
    EXPECT_TRUE(storage->exists(StoredObject("present.txt")));
}

TEST_P(OpenDALObjectStorageTest, GetObjectMetadataReportsSize)
{
    write("sized.bin", "0123456789");
    EXPECT_EQ(storage->getObjectMetadata("sized.bin", /* with_tags */ false).size_bytes, 10u);
}

/// A missing object is ErrorKind::NotFound, which must arrive as FILE_DOESNT_EXIST.
TEST_P(OpenDALObjectStorageTest, StatOnMissingObjectThrowsFileDoesntExist)
{
    EXPECT_THROW_ERROR_CODE(storage->getObjectMetadata("no/such/object.bin", /* with_tags */ false), ErrorCodes::FILE_DOESNT_EXIST);
}

TEST_P(OpenDALObjectStorageTest, ReadOnMissingObjectThrowsFileDoesntExist)
{
    /// Without a known size, readObject() stats the object, which reports it missing.
    EXPECT_THROW_ERROR_CODE(storage->readObject(StoredObject("no/such/object.bin"), ReadSettings{}), ErrorCodes::FILE_DOESNT_EXIST);
}

/// "fs" reports the listing root back as an entry ("/" when listing ""), which must not be listed again.
TEST_P(OpenDALObjectStorageTest, ListObjectsFindsNestedFilesExactlyOnce)
{
    write("top.txt", "aaa");
    write("nested/middle.txt", "bb");
    write("nested/deep/bottom.txt", "c");

    RelativePathsWithMetadata children;
    storage->listObjects("", children, /* max_keys */ 0);

    std::vector<std::string> found;
    for (const auto & child : children)
        found.push_back(child->relative_path);
    std::sort(found.begin(), found.end());

    /// A sorted sequence rather than a set, so that duplicates are caught.
    EXPECT_EQ(
        found,
        (std::vector<std::string>{"nested/deep/bottom.txt", "nested/middle.txt", "top.txt"}));
}

TEST_P(OpenDALObjectStorageTest, ListObjectsHonoursMaxKeys)
{
    write("a.txt", "1");
    write("b.txt", "2");
    write("c.txt", "3");

    RelativePathsWithMetadata children;
    storage->listObjects("", children, /* max_keys */ 2);
    EXPECT_LE(children.size(), 2u);
}

TEST_P(OpenDALObjectStorageTest, SeekReadsTail)
{
    const std::string content = "0123456789abcdef";
    write("seekable.bin", content);

    auto buffer = storage->readObject(StoredObject("seekable.bin", /* local_path */ "", content.size()), ReadSettings{});
    buffer->seek(10, SEEK_SET);

    std::string tail;
    readStringUntilEOF(tail, *buffer);
    EXPECT_EQ(tail, content.substr(10));
}

TEST_P(OpenDALObjectStorageTest, RemoveObjectIfExists)
{
    write("doomed.txt", "x");
    ASSERT_TRUE(storage->exists(StoredObject("doomed.txt")));

    storage->removeObjectIfExists(StoredObject("doomed.txt"));
    EXPECT_FALSE(storage->exists(StoredObject("doomed.txt")));

    EXPECT_NO_THROW(storage->removeObjectIfExists(StoredObject("doomed.txt")));
}

TEST_P(OpenDALObjectStorageTest, TryGetObjectMetadataToleratesMissingObject)
{
    EXPECT_FALSE(storage->tryGetObjectMetadata("no/such/object.bin", /* with_tags */ false).has_value());

    write("here.txt", "1234");
    auto metadata = storage->tryGetObjectMetadata("here.txt", /* with_tags */ false);
    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->size_bytes, 4u);
}

TEST_P(OpenDALObjectStorageTest, CopyObjectFollowsCapability)
{
    write("source.txt", "payload");
    StoredObject from("source.txt");
    StoredObject to("destination.txt");

    if (GetParam().supports_copy)
    {
        storage->copyObject(from, to, ReadSettings{}, WriteSettings{});
        EXPECT_EQ(read("destination.txt"), "payload");
    }
    else
    {
        EXPECT_THROW_ERROR_CODE(storage->copyObject(from, to, ReadSettings{}, WriteSettings{}), ErrorCodes::NOT_IMPLEMENTED);
    }
}

INSTANTIATE_TEST_SUITE_P(
    Backends,
    OpenDALObjectStorageTest,
    ::testing::Values(
        Backend{.scheme = "memory", .supports_copy = false},
        Backend{.scheme = "fs", .supports_copy = true}),
    [](const ::testing::TestParamInfo<Backend> & param_info) { return param_info.param.scheme; });


/// Live tests against huggingface.co. Opt-in: they need network, and the write
/// test additionally needs credentials with write access to the target repo.
namespace
{
    class OpenDALHuggingFace : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            if (!std::getenv("CLICKHOUSE_TEST_OPENDAL_HF")) // NOLINT(concurrency-mt-unsafe)
                GTEST_SKIP() << "set CLICKHOUSE_TEST_OPENDAL_HF=1 to run tests that reach huggingface.co";
        }
    };

    /// A small, long-lived HF Transformers test fixture repo - stable and tiny, used
    /// across the HF ecosystem's own CI for years, so a reasonable choice for a network test.
    std::unique_ptr<OpenDALObjectStorage> makeHFStorage()
    {
        /// Without this, the "hf" backend falls back to whatever's ambient on the machine
        /// running the test (HF_TOKEN env, or a cached `huggingface-cli login` token file) -
        /// these tests want deterministic, unauthenticated behavior against a public repo
        /// regardless of the environment they happen to run in.
        setenv("HF_HUB_DISABLE_IMPLICIT_TOKEN", "1", /* overwrite */ 1); // NOLINT(concurrency-mt-unsafe)

        std::unordered_map<String, String> config{
            {"repo_type", "models"},
            {"repo_id", "hf-internal-testing/tiny-random-bert"},
            {"revision", "main"},
        };
        return std::make_unique<OpenDALObjectStorage>(
            "hf", config, "hf-opendal://models/hf-internal-testing/tiny-random-bert@main", "hf-internal-testing/tiny-random-bert");
    }
}

TEST_F(OpenDALHuggingFace, ListExistsMetadataRead)
{
    auto storage = makeHFStorage();

    RelativePathsWithMetadata children;
    storage->listObjects(/* path */ "", children, /* max_keys */ 0);
    ASSERT_FALSE(children.empty());

    const auto & entry = *children.front();
    const String & path = entry.relative_path;
    ASSERT_TRUE(entry.metadata.has_value());
    const size_t expected_size = entry.metadata->size_bytes;

    StoredObject object(path, /* local_path */ "", expected_size);
    EXPECT_TRUE(storage->exists(object));
    EXPECT_FALSE(storage->exists(StoredObject("this/path/does/not/exist.bin")));

    auto metadata = storage->getObjectMetadata(path, /* with_tags */ false);
    EXPECT_EQ(metadata.size_bytes, expected_size);

    auto read_buffer = storage->readObject(object, ReadSettings{});
    String content;
    readStringUntilEOF(content, *read_buffer);
    EXPECT_EQ(content.size(), expected_size);
}

TEST_F(OpenDALHuggingFace, GetObjectMetadataThrowsForMissingFile)
{
    auto storage = makeHFStorage();
    EXPECT_THROW_ERROR_CODE(storage->getObjectMetadata("this/path/does/not/exist.bin", /* with_tags */ false), ErrorCodes::FILE_DOESNT_EXIST);
}

/// No token was configured in makeHFStorage(), so the backend must report itself (and
/// behave) as read-only - this needs no credentials, unlike an actual write round-trip.
TEST_F(OpenDALHuggingFace, ReadOnlyWithoutToken)
{
    auto storage = makeHFStorage();
    EXPECT_TRUE(storage->isReadOnly());
    EXPECT_THROW(
        storage->writeObject(StoredObject("some/new/file.bin"), WriteMode::Rewrite), Exception);
}

/// Write, list, and remove round-trip against a real writable repo. Unlike the other
/// tests here, this one relies on ambient HF credentials (HF_TOKEN env, or a cached
/// `huggingface-cli login` token file) actually having write access to the target repo.
TEST_F(OpenDALHuggingFace, WriteAndRemoveRoundTrip)
{
    unsetenv("HF_HUB_DISABLE_IMPLICIT_TOKEN"); // NOLINT(concurrency-mt-unsafe)

    std::unordered_map<String, String> config{
        {"repo_type", "datasets"},
        {"repo_id", "kszucs/opendal"},
        {"revision", "main"},
    };
    auto storage = std::make_unique<OpenDALObjectStorage>(
        "hf", config, "hf-opendal://datasets/kszucs/opendal@main", "kszucs/opendal");

    if (storage->isReadOnly())
        GTEST_SKIP() << "No write-capable HF credentials available in this environment";

    const String path = "gtest_write_remove_roundtrip.bin";
    const String content = "hello from OpenDALObjectStorage gtest";

    {
        auto write_buffer = storage->writeObject(StoredObject(path), WriteMode::Rewrite);
        write_buffer->write(content.data(), content.size());
        write_buffer->finalize();
    }

    EXPECT_TRUE(storage->exists(StoredObject(path)));
    EXPECT_EQ(storage->getObjectMetadata(path, /* with_tags */ false).size_bytes, content.size());

    storage->removeObjectIfExists(StoredObject(path));
    EXPECT_FALSE(storage->exists(StoredObject(path)));
    EXPECT_NO_THROW(storage->removeObjectIfExists(StoredObject(path)));
}

#endif
