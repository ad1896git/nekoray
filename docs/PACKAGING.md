# Distribution notices

The release matrix collects notices for the compiled Go packages with the same
GOOS, GOARCH and build tags as the core. It copies module LICENSE/COPYING/NOTICE,
patent and copyright documents plus source header attributions. A missing
module license fails the build, with two documented upstream exceptions:
libneko has no upstream license document (an explicit notice is generated);
cloudflare-tls's README identifies the parent source whose actual LICENSE is
included. This does not resolve libneko's missing upstream license grant.

`libs/package_licenses.sh DEST linux|windows|macos` adds the project LICENSE,
source access directions, carried patch, and checked-in C++/runtime notices.
The source URLs and refs for those documents are in
`libs/licenses/inventory.json` and in each text collection. Qt collections
include upstream attribution metadata and referenced license texts. They are
conservative module-level collections, including optional third-party code.
Update the documents and inventory when changing dependency or runtime inputs.
No new license is assigned to an upstream component.

Windows and Linux ZIPs and AppImage keep LICENSE, THIRD-PARTY-NOTICES.txt and
licenses beside the executables. Debian moves them to
`/usr/share/doc/nekoray/`, using `copyright` for the inventory/source directions.
System Qt remains a Debian dependency; its runtime is removed from `/opt`.

For macOS, first build the GUI/core and run macdeployqt as usual. Then invoke:

```sh
bash libs/package_macos.sh /path/NekoBox.app /path/core-output /path/NekoBox.dmg
```

`core-output` must include `licenses/go` from `libs/build_core.sh`. This entry
point adds notices to `Contents/Resources`, signs the bundle ad hoc, verifies
its signature and creates a new DMG. The output must not already exist. A
Developer ID signing/notarization workflow must replace the final signing step
when used; no Developer ID or notarization is claimed here.

Debian declares `libcap2-bin` in Depends. Its executable POSIX postinst runs
configuration only for `configure`, uses `set -e`, and reports a capability
setup failure before returning nonzero. This follows Debian policy sections
6.1/6.4: https://www.debian.org/doc/debian-policy/ch-maintainerscripts.html
A failed setup must not be represented as configured. The command is
idempotent, so the administrator can fix the dependency/filesystem issue and
retry `dpkg --configure nekoray`. Abort recovery actions require no setup.

`python3 tests/test_package_debian.py` checks generated script handling with
harmless command stubs. The release workflow additionally unpacks the actual
package in disposable Debian bookworm, confirms missing dependencies prevent
configuration, installs them, then checks successful configuration and the
file capability. It does not start the application or create a TUN interface.

Debian uses `dpkg-deb --root-owner-group` so runner ownership never enters the
payload. `libs/validate_debian.py` inspects the built archive before packaging
succeeds: every data entry must have numeric UID/GID 0:0, be a regular file or
directory, and have no world-write, setuid or setgid bits. Empty, malformed and
unsupported data archives fail closed. `python3 tests/test_debian_archive.py`
tests both accepted root ownership and rejected owners/modes. CI also checks
installed ownership and modes. The desktop launcher is a tracked payload file;
postinst refreshes the desktop database after capability setup, and postrm
refreshes it on removal/purge when the tool is available. CI verifies that dpkg
tracks the launcher and removes it on removal and purge.

Before copying native notices, the helper checks every common/platform manifest
entry and the aggregate attribution files for nonempty contents. Each final ZIP,
Debian, AppImage and macOS packaging entry point validates the assembled notice
tree before archiving or signing. Go's `version -m` reads the target core's build
metadata without executing it: its exact dependency names, replacement sources,
versions and target OS must match the inventory. Every inventoried legal and
generated source notice, Go-LICENSE, Go-PATENTS, project LICENSE and native notice
must exist and be nonempty. Packaging therefore requires Python 3 and Go.
Notice trees may contain only regular files and directories: all symlinks
(including in-tree and dangling links) and special files are rejected, even if
not listed in an inventory. Each format checks the final copied/moved notice
tree as well as its input; Debian checks the relocated documentation layout.

AUTHORS, PATENTS, COPYRIGHT and NOTICE supplement a module's LICENSE/COPYING;
they cannot alone satisfy the collector. The two documented exceptions are tied
to their exact source/version pins; libneko requires its unchanged factual NOTICE.
`python3 tests/test_package_licenses.py` builds tiny target fixtures, checks
complete trees, and invokes every packaging entry point with missing/empty Go,
runtime, project and Qt notices, inventory-only trees and mismatched module
inventories. On macOS, add `--macos-real` to also sign a fixture app, create a DMG,
mount it read-only and verify its notices/signature. These fixtures never run the
application and do not substitute for a full GUI build.
