## 构建 nekobox_core

### 目录结构

```
  | nekoray
  |   go/cmd/*
  | sing-box-extra
  | sing-box
  | ......
```

### 常规构建

1. `bash libs/get_source.sh` （自动下载目录结构，自动 checkout commit）
2. `GOOS=windows GOARCH=amd64 bash libs/build_go.sh`

具体支持的 GOOS 和 GOARCH 请看 `libs/build_go.sh`

非官方构建无需编译 `updater` `launcher`

### sing-box tags

具体使用的 tags 请看 `libs/build_go.sh`

### Verify cached Go dependencies

After fetching the pinned sibling sources with `bash libs/get_source.sh`, run:

```sh
GOTOOLCHAIN=local bash libs/verify_go_modules.sh
```

This script provides **partial verification, not exact standalone build-list
integrity coverage**. It first runs `GOWORK=off go mod download all` for each
module using temporary copies of its `go.mod` and `go.sum`. This checks recorded
checksums and downloads the actual standalone selections without editing the
repository manifests. Missing sums are added only to the temporary copies using
Go's normal checksum handling; their absence in the original files does not fail
this check. Downloads can reuse cached hashes and do not rehash existing source
directories or archives.

It then runs one `go mod verify` with the explicit
`GOWORK=<repository>/go/verify.go.work` workspace. This rehashes the workspace
graph's cached archives/directories, not each module's standalone graph. Both
working directories select the same workspace graph, so repeating the command
would not add coverage. Local source replacements are not downloaded archives.

The precise limitations at the current dependency pins are:

| Standalone module | External selections | Missing archive sums in its own go.sum | Missing go.mod sums | Versions changed by the workspace |
| --- | ---: | ---: | ---: | ---: |
| `go/cmd/nekobox_core` | 159 | 62 | 51 | 9 |
| `go/grpc_server` | 62 | 47 | 6 | 37 |

Counts use `GOWORK=off go list -mod=readonly -m -e -json all`, follow versioned
replacements, and exclude the main module and local replacements. Exact
standalone verification that fails on every missing go.sum entry cannot pass
without changing the frozen checksum files. Plain standalone grpc_server listing
also requests `go mod tidy`; `-e` exposes its selected list without editing it.

The workspace changes core's selected versions of `cloud.google.com/go`,
`github.com/cncf/udpa/go`, `github.com/go-logr/logr`, `github.com/pierrec/lz4/v4`,
`github.com/prometheus/client_model`, `github.com/stretchr/objx`,
`golang.org/x/xerrors`, `gopkg.in/check.v1`, and `gopkg.in/yaml.v2`. For example,
standalone core selects lz4 v4.1.14 while the workspace selects v4.1.18: edits to
an already cached v4.1.14 source directory are **not detected** by this script.
grpc_server's 37 changed external versions include grpc v1.49.0 versus workspace
v1.63.2 and protobuf v1.28.1 versus v1.33.0. Its local libneko replacement also
becomes the core's downloaded fork pin. Thus a successful script does not establish
cache integrity for all versions used by either standalone build. The workspace
also adds 154 external paths relative to core and 251 relative to grpc_server.

The non-default workspace filename prevents automatic discovery by builds or
packaging. Without explicitly selecting it, Go 1.22's verifier still attempts a
module-cache lookup for the local-only name `grpc_server` and reports a missing
ziphash. Existing `go.mod`/`go.sum` files and normal build graphs are unchanged.

## Build path metadata

Both `libs/build_core.sh` and the release `libs/build_go.sh` use `go build -trimpath`. CMake maps the GUI's source/build roots with GCC/Clang prefix-map
options or MSVC `/pathmap`; the Windows executable records only the PDB basename
with `/PDBALTPATH`. `libs/build_deps_all.sh` applies the same mapping to protobuf,
ZXing and yaml-cpp. Rebuild existing static dependencies; previously built
archives retain their original paths. For a separately configured dependency,
pass `-DCMAKE_PROJECT_<project-name>_INCLUDE=/path/to/nekoray/cmake/build_paths.cmake`
(project names: `protobuf`, `ZXing`, `YAML_CPP`). Mapped source paths are relative
to `./src`, generated paths to `./build`; debuggers may need source substitution.

This removes project build prefixes, not all absolute strings or all causes of
non-reproducibility. Prebuilt Qt libraries and the Wintun images embedded in the
Windows Go core can still contain upstream build/PDB paths. No dependency pins
or published v5.0.1 assets are changed by these build settings.
