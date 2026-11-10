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
