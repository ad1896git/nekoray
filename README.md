# NekoBox For PC（个人分支 / personal fork）

Qt based cross-platform GUI proxy configuration manager (backend: sing-box)

基于 Qt 的跨平台代理配置管理器 (后端 sing-box)

本仓库是 [MatsuriDayo/nekoray](https://github.com/MatsuriDayo/nekoray) 的个人分支（`ad1896git/nekoray`），仅供个人使用。安全性和稳定性优先，功能不追求丰富。

This repository is a personal fork of [MatsuriDayo/nekoray](https://github.com/MatsuriDayo/nekoray) (`ad1896git/nekoray`), kept for personal use. Security and stability come first; features are secondary.

## 平台 / Platforms

- Windows (x64)：CI 构建，便携 ZIP。
- Linux (x64)：CI 构建。
- macOS (Apple Silicon, arm64)：预览版，要求 macOS 14.4 或更新。目前只在 macOS 27 上测试过。

## 下载 / Download

预编译包发布在本仓库的 [Releases](https://github.com/ad1896git/nekoray/releases)。macOS 版本仍在测试中，尚未正式发布。

Pre-built packages are published on this fork's [Releases](https://github.com/ad1896git/nekoray/releases). The macOS build is still under testing and not yet released.

## 依赖与版本 / Dependencies

核心依赖固定到具体 commit，见 [`libs/get_source_env.sh`](libs/get_source_env.sh)：sing-box 和 sing-quic 使用本仓库所有者的 fork，libneko 和 quic-go 分别来自上游和 sagernet。

The core dependencies are pinned to specific commits in [`libs/get_source_env.sh`](libs/get_source_env.sh). sing-box and sing-quic come from this owner's forks; libneko and quic-go come from upstream.

## 项目主页 & 文档 / Homepage & Documents

本分支的技术文档在 [docs/](docs/) 目录。上游项目：[MatsuriDayo/nekoray](https://github.com/MatsuriDayo/nekoray)。

Technical documentation for this fork is in [docs/](docs/). Upstream project: [MatsuriDayo/nekoray](https://github.com/MatsuriDayo/nekoray).

## 代理 / Proxy

- SOCKS (4/4a/5)
- HTTP(S)
- Shadowsocks
- VMess
- VLESS
- Trojan
- TUIC ( sing-box )
- NaïveProxy ( Custom Core )
- Hysteria2 ( Custom Core or sing-box )
- Custom Outbound
- Custom Config
- Custom Core

## 订阅 / Subscription

- Raw: some widely used formats (like Shadowsocks, Clash and v2rayN)
- 原始格式: 一些广泛使用的格式 (如 Shadowsocks、Clash 和 v2rayN)

## 运行参数

[运行参数](docs/RunFlags.md)

## Windows 运行

若提示 DLL 缺失，无法运行，请下载 安装 [微软 C++ 运行库](https://aka.ms/vs/17/release/vc_redist.x64.exe)

## Linux 运行

[Linux 运行教程](docs/Run_Linux.md)

## 编译教程 / Compile Tutorial

请看 [技术文档 / Technical documentation](docs/)

## Credits

本分支基于 MatsuriDayo/nekoray，感谢上游项目及以下依赖的作者。

This fork is based on MatsuriDayo/nekoray. Thanks to the upstream project and the following dependencies.


Core:

- [v2fly/v2ray-core](https://github.com/v2fly/v2ray-core) ( < 3.10 )
- [MatsuriDayo/Matsuri](https://github.com/MatsuriDayo/Matsuri) ( < 3.10 )
- [MatsuriDayo/v2ray-core](https://github.com/MatsuriDayo/v2ray-core) ( < 3.10 )
- [XTLS/Xray-core](https://github.com/XTLS/Xray-core) ( 3.10 <= Version <= 3.26 )
- [MatsuriDayo/Xray-core](https://github.com/MatsuriDayo/Xray-core) ( 3.10 <= Version <= 3.26 )
- [SagerNet/sing-box](https://github.com/SagerNet/sing-box)
- [Matsuridayo/sing-box-extra](https://github.com/MatsuriDayo/sing-box-extra)

Gui:

- [Qv2ray](https://github.com/Qv2ray/Qv2ray)
- [Qt](https://www.qt.io/)
- [protobuf](https://github.com/protocolbuffers/protobuf)
- [yaml-cpp](https://github.com/jbeder/yaml-cpp)
- [zxing-cpp](https://github.com/nu-book/zxing-cpp)
- [QHotkey](https://github.com/Skycoder42/QHotkey)
- [AppImageKit](https://github.com/AppImage/AppImageKit)
