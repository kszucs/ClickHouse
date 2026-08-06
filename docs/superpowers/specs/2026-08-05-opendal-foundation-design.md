# OpenDAL foundation: multi-backend reading and writing

Date: 2026-08-05
Branch: `opendal` (ClickHouse), `clickhouse` (OpenDAL fork)
Status: approved design; Sections 1.1 and 2 implemented, see Progress below

## Progress and corrections

Reality checks against `upstream/main` of `apache/opendal` (v0.58.1) changed
several assumptions this document was written under. Corrections, in place of
rewriting the sections wholesale:

- **Section 1.3 (recursive listing) is already upstream.** `FfiListOptions` has
  a `recursive` flag, exposed through `list_options` and `lister_options`. No
  patch needed; `OpenDALObjectStorage` simply does not use it yet.
- **Section 1.4 is largely already upstream.** `Reader::read_at(buf, offset)`
  gives positioned reads and `FfiReadOptions.range` gives ranged whole-object
  reads. A range-limited *Reader* still does not exist, but `read_at` is
  probably sufficient for what ClickHouse needs.
- **Section 1.2 (metadata in list entries) is still needed.** `Entry` remains
  `{ path: String }`.
- **Section 1.1 was implemented differently from what this document specified,
  and the specification was wrong.** Converting all ~40 bridge functions to
  status structs would fight every future upstream rebase. What landed instead
  is an `FfiError` type with `impl From<od::Error>`, so the existing `?`
  operators convert automatically and no function body changes. The kind and
  temporary flag are encoded into the cxx error payload with a versioned,
  separator-delimited format; `src/error.cpp` decodes it. Encoder and decoder
  are committed together. Same typed-error guarantee, a fraction of the diff.
- **A gap not anticipated here: the binding forwarded no service features.**
  `bindings/cpp/Cargo.toml` declared only `async` and `testing`, so
  `-DOPENDAL_FEATURES=services-s3` failed outright — including the
  `services-hf` that `contrib/opendal-cmake` already passes. Forwarding
  features for azblob, fs, gcs, hf, http, memory and s3 were added, which is
  also the mechanism Section 3.1 depends on.
- **Public headers must not name cxx types.** The generated cxxbridge include
  directory is only added to the public include path when the async feature is
  on, so a public header including `rust/cxx.h` breaks every consumer that has
  async off, ClickHouse included. `RethrowFfiError` and `Call` therefore live in
  `src/ffi_error.hpp`, not in `include/error.hpp`.

Still outstanding from Section 2: the write-path cancel-on-exception discipline
(2.2) is not done — error translation is wired through `OpenDALWriteBuffer`, but
partial-object cleanup and destructor safety remain.

## Goal

Make ClickHouse's OpenDAL integration a genuine multi-backend object storage
layer — services selected at build time — with correct, diagnosable reads and
writes. This is foundation work. It deliberately stops short of new SQL surface
(table engine, data lakes, cluster reads) and of the disk backend, because all
of those inherit their behaviour from the layer specified here.

## Starting point

Commit `d2ce9e14ca0` on branch `opendal` contains:

- `OpenDALObjectStorage` — an `IObjectStorage` over the OpenDAL C++ binding,
  wired into `ReadBufferFromRemoteFSGather` and `AsynchronousBoundedReadBuffer`.
- `OpenDALReadBuffer` — streaming reads with local seek reuse.
- `OpenDALWriteBuffer` — streaming writes (repeated `Writer::Write()`, one
  `Close()` on finalize). Note the class comment on `OpenDALObjectStorage.h`
  still claims whole-object in-memory buffering; it is stale and should be
  fixed.
- `StorageOpenDALConfiguration` + a registered `opendal()` table function,
  schema inference cache, and three `opendal_*` settings.
- One gtest that requires live network access to huggingface.co.

`contrib/opendal` is **not** committed: it is an unversioned copy of OpenDAL
v0.58.0 carrying ~10 GB of `bindings/cpp/target` build artifacts.

## Out of scope

These are real and mostly necessary, but not part of this spec:

- Table engine `ENGINE = OpenDAL(...)`, data lake configurations, cluster table
  function, disk backend (`object_storage_type: opendal`), backups, filesystem
  cache, `OpenDALQueue`.
- Rust vendoring into `contrib/rust_vendor` and building through corrosion.
  The binding currently builds via a raw `cargo build` custom target that
  downloads crates at build time, which upstream ClickHouse CI (hermetic,
  offline, pinned to `nightly-2024-04-01`) will not accept. This is the single
  largest blocker to upstreaming anything, and it is its own project.
- Memory accounting: OpenDAL's Rust-side allocations (tokio, reqwest) are
  invisible to ClickHouse's `MemoryTracker` and therefore sit outside
  `max_server_memory_usage`. Needs measurement before it can be designed; it
  could invalidate design choices elsewhere, so measure early even though the
  fix is out of scope.

## Section 0 — Source layout

`contrib/opendal` becomes a git submodule pointing at a fork
(`kszucs/opendal`, branch `clickhouse`, based at v0.58.0). Every other
ClickHouse contrib is a submodule and the build system has no patch-file
mechanism, so a fork branch is the only way to carry binding changes under
version control. The current untracked directory means the binding patches
described in Section 1 would otherwise not be tracked at all.

`contrib/opendal-cmake/CMakeLists.txt` keeps its present structure. The raw
`cargo build` custom target stays for now (see Out of scope).

## Section 1 — Binding patches

Four changes to `bindings/cpp` on the fork branch, to be upstreamed to
`apache/opendal` once their shape has settled in practice.

### 1.1 Non-throwing bridge with typed errors

Today every cxx bridge function returns `anyhow::Result<T>`. cxx converts that
into a C++ exception carrying only `e.to_string()`, so `opendal::ErrorKind` is
destroyed at the boundary. There is no `Error` type and no `ErrorKind` in the
binding's public headers.

Change the bridge to return status structs instead of `Result`, with an
`ErrorKind` enum crossing the boundary and `ErrorKind::Ok` signalling success:

```rust
struct StatResult { kind: ErrorKind, message: String, metadata: Metadata }
```

Rationale, in order of importance:

1. `ErrorKind` survives, which is the prerequisite for Section 2. The only
   alternative available without binding changes is parsing OpenDAL's `Display`
   format (`"NotFound (permanent) at stat => ..."`), which is too fragile to
   put under retry logic.
2. No exception unwinds across the Rust boundary. The current code has this
   hazard in `OpenDALWriteBuffer::nextImpl`.
3. The C++ side decides *when* to throw, which is what makes the destructor
   safety in Section 2.2 achievable.

### 1.2 Metadata in list entries

`struct Entry` is currently `{ path: String }` — no metadata — which is why
`OpenDALObjectStorage::listRecursive` issues one `Stat()` per file. A
10,000-file repository costs 10,000 round trips.

Add `metadata: Metadata` to `Entry`, populated on the Rust side via
`list_with(path).metakey(Metakey::ContentLength | Metakey::Etag |
Metakey::LastModified)`.

### 1.3 Recursive listing

Add a `recursive: bool` parameter to the `lister`/`list` bridge functions,
mapping to OpenDAL's own recursive list option. `Lister` is already exposed and
streaming; only the flag is missing.

### 1.4 Ranged reader

`fn reader(self: &Operator, path: &str)` takes no range, so
`OpenDALReadBuffer::setReadUntilPosition` can only clamp `max_bytes` locally
while the underlying reader still opens an unbounded read. A Parquet footer
read therefore transfers far more than it needs.

Add offset and size parameters mapping onto `reader_with(path).range(..)`.

## Section 2 — Error translation

### 2.1 Mapping

New `src/Disks/ObjectStorages/OpenDAL/OpenDALError.{h,cpp}`. Add
`OPENDAL_ERROR = 731` to `src/Common/ErrorCodes.cpp`, following the existing
one-code-per-backend convention (`S3_ERROR` 499, `AZURE_BLOB_STORAGE_ERROR`
500, `HDFS_ERROR` 660). 731 is the next free code as of commit `7d670bf3c2d`
and must be rechecked when rebasing onto a newer master.

| OpenDAL `ErrorKind`             | ClickHouse error code             |
| ------------------------------- | --------------------------------- |
| `NotFound`                      | `FILE_DOESNT_EXIST` (107)         |
| `PermissionDenied`              | `ACCESS_DENIED` (497)             |
| `ConfigInvalid`                 | `BAD_ARGUMENTS` (36)              |
| `AlreadyExists`                 | `FILE_ALREADY_EXISTS` (504)       |
| `IsADirectory`, `NotADirectory` | `BAD_FILE_TYPE` (624)             |
| `Unsupported`                   | `NOT_IMPLEMENTED` (48)            |
| `RangeNotSatisfied`             | `ARGUMENT_OUT_OF_BOUND` (69)      |
| `RateLimited`, `Unexpected`, and any kind not listed above | `OPENDAL_ERROR` (731) |

A single helper is applied at *every* bridge call site:

```cpp
void checkOpenDAL(std::string_view operation, std::string_view path,
                  const auto & result);
```

It throws `DB::Exception` with the mapped code, a message naming the operation,
the path and the storage description, and it preserves OpenDAL's `is_temporary`
flag so retry decisions have something to key on. Currently only
`getObjectMetadata` catches anything at all; `exists`, `List`, `Stat`, `Read`,
`Write`, `Remove` and `Copy` all let untyped exceptions escape.

### 2.2 Write path safety

`OpenDALWriteBuffer` adopts ClickHouse's finalize/cancel discipline: a failed
write must not leave a partial object behind, and no exception may escape a
destructor. Section 1.1 is what makes this possible, since the Rust calls stop
throwing.

## Section 3 — Service matrix, configuration, masking

### 3.1 Build-flag service selection

`OPENDAL_FEATURES` is currently hardcoded to `services-hf`. Replace with a
service list in `contrib/opendal-cmake/CMakeLists.txt`, one
`ENABLE_OPENDAL_<SVC>` option per service, defaulting to:

    memory, fs, http, s3, gcs, azblob, hf

`OPENDAL_FEATURES` is derived from whichever options are on. The enabled set is
also baked in as a compile definition so a small registry
(`isSchemeAvailable()`, `getAvailableSchemes()`) can validate schemes at
runtime. Requesting a scheme that was not compiled in produces a ClickHouse
error naming the available schemes, rather than an opaque Rust `ConfigInvalid`.

`memory` and `fs` are in the default set specifically because Section 4 needs
them; both are pure Rust with no network dependencies.

### 3.2 SQL surface

Replace the current `(scheme, config, path, format[, structure])` form — whose
`config` argument is a comma-separated `key=value` string — with key-value
arguments:

```sql
SELECT * FROM opendal('s3', path = 'bucket/data/*.parquet',
                      region = 'us-east-1',
                      secret_access_key = '...');
```

`path`, `format`, `structure` and `compression_method` are reserved and
consumed by ClickHouse. Every other key passes through to OpenDAL untouched, so
adding a service requires no argument-parsing work. Named collections use the
identical reserved-key split.

The comma-separated config string is dropped: it cannot represent values
containing commas, and it makes per-key secret masking impossible (see 3.3).

The `hf://` URI shorthand and its `parseHFUri` parser are dropped. A URI parser
per service is precisely the per-backend special-casing this design is trying to
eliminate. This is a small user-facing regression for the existing Hugging Face
use case; the key-value form covers it.

### 3.3 Secret masking

`src/Parsers/FunctionSecretArgumentsFinderAST.h` gains
`findOpenDALFunctionSecretArguments()`, plus a per-service table of keys known
to be **non-secret** (`region`, `bucket`, `endpoint`, `repo_id`, `repo_type`,
`revision`, ...).

Policy is **default-deny**: every named argument whose key is not on the
service's safe list is redacted. If the scheme argument is not a literal, every
named argument is redacted. The consequence is that a newly added service, or a
new OpenDAL config key, can never leak a credential into `query_log`,
`system.processes` or the server log — at the cost of redacting some harmless
values until the safe list catches up.

The alternative (a denylist of secret-looking key names) was rejected: it must
distinguish `secret_access_key` from `access_key_id`, and any unanticipated
secret key name leaks in cleartext, which is a vulnerability rather than an
inconvenience.

Mechanically this needs only a set-based generalisation of the existing
`findSecretNamedArgument`, which already matches `key = value` AST nodes by
exact key name. The safe-key table lives in the parser layer, so no dependency
from `src/Parsers` onto `src/Storages` or `src/Disks` is introduced.

## Section 4 — Hermetic tests

The `memory` and `fs` services from 3.1 give a network-free harness.
`src/Disks/ObjectStorages/OpenDAL/tests/gtest_opendal_object_storage.cpp`
becomes a value-parameterized suite over both backends, covering the
`IObjectStorage` contract:

- `exists` for present and absent objects
- recursive `listObjects`, including `max_keys` behaviour
- `getObjectMetadata` sizes and etags
- whole-object reads, ranged reads, mid-file seeks
- write / read-back / remove round trip, and `removeObjectIfExists` on a
  missing object
- `copyObject` where the backend advertises the capability

Error cases assert *specific* error codes from the Section 2 table, which is
what actually verifies the mapping rather than merely observing that something
threw.

The `fs` backend serves as ground truth for recursive listing: the same tree can
be created on disk and compared against what `listObjects` reports.

The existing Hugging Face network tests are kept but gated behind an
environment variable so they do not run by default.

## Verification

- Hermetic gtest suite passes against both `memory` and `fs`.
- Error-code assertions cover every row of the Section 2.1 table that the
  `memory` or `fs` backends can produce; rows only reachable on network
  backends (`RateLimited`, `PermissionDenied`) are covered by the gated HF
  tests.
- A build with only `ENABLE_OPENDAL_MEMORY` compiles, and
  `opendal('s3', ...)` on that build fails with an error listing `memory` as
  the available scheme.
- `SELECT * FROM opendal('s3', ..., secret_access_key = 'x')` appears in
  `system.query_log` with the secret redacted and `region` intact.

## Sequencing

1. Section 0 (submodule) — unblocks everything, since the binding patches need
   somewhere to live.
2. Section 1 (binding patches) — unblocks Section 2, and the ranged-read and
   listing fixes listed under follow-on work.
3. Section 2 (error translation) — every later failure becomes legible, and
   typed errors are cheap now but expensive to retrofit.
4. Section 3.1 (service matrix) — unblocks Section 4 by enabling `memory` and
   `fs`.
5. Section 4 (hermetic tests) — makes Sections 2 and 3 verifiable without
   network access.
6. Sections 3.2 and 3.3 (SQL surface, masking), verified by the harness from
   step 5.

Measure the Rust-side memory behaviour (see Out of scope) alongside step 2,
early enough that a bad result can still change the design.

## Follow-on work, now unblocked

Ranged reads honouring `setReadUntilPosition`, `read_hint` and
`restricted_seek`; listing via recursive `Lister` with metadata in entries;
`WriteMode::Append`; `ObjectAttributes` and `WriteSettings` (throttling);
ProfileEvents for remote read/write accounting. Then the SQL surface work:
table engine, data lakes, cluster reads, disk backend.
