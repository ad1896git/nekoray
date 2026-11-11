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

The script runs plain `go mod verify` in both modules with the explicit
`GOWORK=<repository>/go/verify.go.work` workspace. You can also set that absolute
`GOWORK` path yourself and run `go mod verify` in either module. The workspace
recognizes `grpc_server` as project source and resolves the libneko replacement
conflict to the existing core fork pin. It preserves the normal checksum checks
for downloaded dependencies; local source is not a downloaded module archive.

The non-default workspace filename prevents automatic discovery by builds or
packaging. Without explicitly selecting it, Go 1.22's verifier still attempts a
module-cache lookup for the local-only name `grpc_server` and reports a missing
ziphash. Existing `go.mod`/`go.sum` files and normal build graphs are unchanged.
