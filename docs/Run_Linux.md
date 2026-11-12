## Linux 安装

### Debian 系发行版

使用可信的 shell 和系统命令，先切换到可信目录（`cd /`）；不要在旧安装目录中以 root 运行文件。
使用 Debian 系发行版时，推荐使用已验证的 .deb 包安装；不安全的旧安装应先按
[管理员恢复步骤](PACKAGING.md#administrator-recovery-a-prime)处理：

```shell
# 替换为已验证、位于可信目录中的软件包绝对路径。
new_deb=/absolute/trusted/path/nekoray-version-debian-x64.deb
cd /
/usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/bin/apt-get install "$new_deb"
```

安装完成后，桌面快捷方式启动自带参数 `-appdata`，如果想要直接启动并使用之前的配置，注意附带本参数。

### Arch 系发行版

使用 Arch 系发行版时，推荐从 ```aur``` 或 ```archlinuxcn``` 安装：

#### AUR
##### 最新稳定版

```shell
[yay/paru] -S nekoray
```

##### 最新 Git 版 (开发版)

```shell
[yay/paru] -S nekoray-git
```

#### archlinuxcn

##### 最新稳定版

```shell
cd /
/usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/bin/pacman -S nekoray
```

##### 最新 Git 版 (开发版)

```shell
cd /
/usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/bin/pacman -S nekoray-git
```

### 其他发行版

下载 .zip 文件，解压到合适的路径，开箱即用。

或下载 .AppImage，并使用 `chmod +x nekoray-*-AppImage-x64.AppImage` 给予可执行权限。

具体使用方法见下文。

## Linux 运行

**使用 Linux 系统相信您已具备基本的排错能力，
本项目不提供特定发行版/架构的支持，预编译文件不能满足您的需求时，请自行编译/适配。**

已知部分 Linux 发行版无法使用、非 x86_64 暂无适配，可以尝试自行编译。

目前 Release 便携包解压后，有两种使用方法：

1. System: 若要使用系统的 Qt5 运行库，请执行 `./nekobox`
2. Bundle: 若要使用预编译的 Qt 运行库，请执行 `./launcher`

### Bundle

要求：已安装主流的发行版和 xcb 桌面环境。

运行： `./launcher` 或 部分系统可双击打开

安装目录路径不能包含冒号（`:`）或换行符；遇到此类路径请先移动或重命名目录。

launcher 参数

* `./launcher -appdata` ( 所有参数直接传递给主程序 )
* `./launcher -debug` Debug mode

Ubuntu 22.04: 先 `cd /`，然后 `/usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/bin/apt-get install libxcb-xinerama0`

### System

要求：已安装主流的发行版和 xcb 桌面环境，已安装 Qt5.12 ~ Qt5.15 环境。

运行： `./nekobox` 或 部分系统可双击打开。如果无法运行，建议使用 Bundle 版。
