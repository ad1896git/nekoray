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
directory, and have no group/world-write, setuid or setgid bits. Empty, malformed and
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

Debian preinst inspects existing installs before unpacking, using physical
(`find -P`), filesystem-bounded (`-xdev`) traversal and lstat metadata. `/` and
`/opt` must be real root:root directories without group/world write permission
(an absent `/opt` is allowed for a fresh install). An absent `/opt/nekoray` is a
fresh install. Otherwise the prefix and every entry must be root:root without
group/world write permission. Symlinks (including a dangling prefix), hard-linked
files, special files and nested filesystem boundaries are refused. Inspection
errors also fail closed. Postinst repeats this read-only assertion before the
unchanged capability setup, including on `dpkg-reconfigure`.

There is no ownership/mode repair in either script. The upstream 4.0.1 Debian
package leaves a non-root-owned `/opt/nekoray`, so upgrading it is deliberately
refused with `nekoray: refusing unsafe pre-existing install:` and an English
remedy. Run `sudo apt purge nekoray` (or `sudo dpkg --purge nekoray`), then
reinstall the new Debian package. Alternatively, have an administrator inspect
and remove `/opt/nekoray` before retrying. Back up any wanted local data before
purging/removal. Do not make a user-controlled tree pass by recursively changing
its ownership as root. CI verifies that the refused 4.0.1 upgrade leaves version
4.0.1 in `install ok installed` state with an empty dpkg audit. Its upstream
untracked desktop launcher can remain after purge; the new package adopts it as
a tracked payload file and removes it on its own removal/purge.

The checks only read metadata; racing the checks cannot redirect a privileged
repair because no repair commands exist. A non-root user cannot replace an
already-checked safe directory through its checked, unwritable parent, nor
change that root-owned directory's permissions. Any attacker-controlled nested
directory fails ownership/write checks; it is never secured by mutation. dpkg
then unpacks root:root package files into the trusted tree. This does not protect
against a concurrent administrator/root process, privileged mount changes or
filesystems that change metadata outside normal local permission rules. A safe
but unexpected root-owned file left by an administrator can remain untracked;
these checks establish ownership/type/write safety, not an exact file inventory
or file-content authenticity. No GUI, TUN operation or local data migration is
performed by these checks.

The archive validator requires exact root-owned executable preinst/postinst
contents matching `libs/debian/`, rejects stale/omitted scripts and forbidden
repair commands, and rejects unsafe payload ownership/modes. The Python tests
use native traversal/mode/link checks in a translated scratch tree, simulate
numeric root ownership/device boundaries on unprivileged hosts, and verify
refusal, command failures, a synchronized path replacement and outside sentinel
owner/mode/content preservation. A static guard prevents chown/chmod (and other
repair commands) from returning to the maintainer scripts.

The build matrix's `deb-upgrade` jobs run
`bash tests/verify_debian_lifecycle.sh package.deb` on disposable Ubuntu 22.04
and 24.04 runners. They use the same run's newly built package, verify the exact
upstream 4.0.1 SHA256 before installation, require preinst refusal and a consistent
old package, then test the documented purge/reinstall remedy. Fresh install,
actual apt reinstall of a safe tree, safe reconfiguration, numeric ownership and
modes of every prefix entry, capability, dpkg-tracked launcher, removal/purge,
and an attacker-owned nested directory with a symlink to an outside sentinel
are asserted. Package-manager bookkeeping and upstream abort recovery still
run during refusal; the sentinel, old core/launcher bytes and prefix metadata
are checked, not every filesystem write made by apt/dpkg. This script changes
system packages; run it only in a disposable test environment.
