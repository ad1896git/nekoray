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
retry `/usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/bin/dpkg --configure nekoray`
from a trusted directory (`cd /`). Postinst abort recovery actions require no capability setup; postrm unwind actions inspect safety as described below.

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

There is no ownership/mode repair in any maintainer script. The upstream 4.0.1
Debian archive assigns `/opt` and `/opt/nekoray` UID/GID 1001:127, so upgrading
an unsafe install is deliberately refused with
`nekoray: refusing unsafe pre-existing install:`. A failed preinst causes dpkg
to call the new postrm during error unwind. For `abort-upgrade`, `abort-install`
and `failed-upgrade`, that postrm performs the same read-only inspection as
preinst (including ancestors, absent-prefix handling and inspection errors).
It returns nonzero on an unsafe tree, preventing dpkg from continuing into old
maintainer scripts. This matters because 4.0.1's postinst ignores its action and
unconditionally runs `setcap` on the old, user-controlled core path. A safe tree
still returns zero and unwinds normally. The guard is embedded in each script;
no code is sourced from the installed tree, and tests enforce identical
preinst/postrm inspection bytes.

## Administrator recovery (A-prime)

An unsafe upgrade with blocked unwind leaves 4.0.1 Half-Installed
(`Status: install reinstreq half-installed`). Plain `dpkg --purge` fails in
that state. **Before any recovery step, require that no process of the old
owning UID holds capabilities, including CAP_NET_ADMIN.** From a trusted
administrator shell, identify the numeric owner using `/usr/bin/stat -c %u
/opt/nekoray` (lstat; do not follow a prefix symlink). For example, with
`old_uid=1001`, inspect processes and their capabilities using:

```bash
/usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/bin/ps -U "$old_uid" -u "$old_uid" -o pid,uid,euid,args
# For EACH listed PID, substitute its numeric value:
/usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/sbin/getpcaps PID
```

Alternatively inspect `Uid:`, `CapEff:` and `CapPrm:` in `/proc/PID/status`
for every process whose real, effective, saved or filesystem UID is the old
UID; bit 12 (`0x1000`) is CAP_NET_ADMIN. Terminate those processes (suspending
them retains capabilities), stop the UID's services and sessions, and prevent
new logins/jobs throughout recovery. Repeat the inspection and require no
remaining processes of that UID. An offline rescue environment is preferable
when this exclusion cannot be established. These commands are observations,
not an enforcement mechanism: file xattr revocation does not remove capabilities
already held by a running process, and a live scan cannot prove completeness
against concurrent hostile activity. Do not declare recovery complete without
process exclusion and coverage of all relevant mounts.

Only then back up wanted local data without executing untrusted files. This is an
administrator procedure on a trusted local filesystem, with `/` controlled by
root and no concurrent administrator or mount changes. Do not recursively
change ownership or permissions of the old tree.

Use a trusted Bash session and run `cd /` as an extra precaution before the
following blocks. Use the system executables shown, not copies from the old
install or aliases/functions supplied by an untrusted shell setup. Both Python
commands use `/usr/bin/python3 -I`: isolated mode excludes the working directory
and user site from imports and ignores `PYTHON*` variables, even if sudo policy
preserves them. The absolute `/usr/bin/sudo` uses the system's trusted sudo
policy; its setuid startup strips loader variables such as `LD_PRELOAD` and
`LD_LIBRARY_PATH`. Package-manager commands additionally start with an empty
environment and a trusted system PATH, including for their maintainer scripts
and helpers, without depending on sudo's `env_keep` or `secure_path` settings.
All filesystem paths are absolute or relative to the checked `/opt` descriptor;
`new_deb` must name an authenticated package at an absolute path in a trusted
directory. Do not preserve an untrusted shell configuration or package-manager
configuration. These steps assume the system executables and configuration are
trusted, as well as `/`.

Run the following blocks in order, in the same Bash session. Preparation first
opens `/opt` with `O_DIRECTORY|O_NOFOLLOW`, verifies its identity, and performs
**non-recursive fchown root:root and fchmod 0755 on that descriptor** before
inspecting any child or quarantine destination. It then opens the real legacy
prefix and core relative to descriptors, without following symlinks. It verifies
the core is regular, records `(st_dev, st_ino)`, removes `security.capability`
through the core descriptor, and requires empty `getcap /proc/self/fd/N` output.
Revocation changes the inode, covering every hardlink or moved alias, rather
than just unlinking a quarantine name. A prefix symlink is never traversed;
the scan below must find any displaced capable core instead. A symlink or
special file at the core of a real prefix stops for manual inspection.

The same block scans regular files as root, using descriptor-relative,
no-follow traversal bounded to each scan root's filesystem (like `find -P
-xdev` plus `getcap -r`). It reports every capable inode, and flags and revokes
capabilities if the inode is not root-owned, has unsafe modes/ancestors, or is
outside trusted system/package paths. Only root-controlled `/usr`, `/bin`,
`/sbin`, `/lib` and `/lib64` trees are allowlisted here; review that trust
assumption against authenticated system packages. Other paths lose capabilities,
even when root-owned; reinstall authenticated packages if needed afterward.
The scan catches inodes that older postinst runs may have made capable before
an attacker swapped the core path. No symlink is followed during traversal,
opening or revocation. Errors stop recovery rather than imply an empty scan.

**Mount coverage is an administrator responsibility.** Inspect mounts first
with `/usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin
/usr/bin/findmnt -rn -o TARGET,FSTYPE,OPTIONS`. The default roots cover `/` and
the filesystem containing `/opt`. Every skipped boundary is reported. Add each
other local filesystem (including separate `/home`, `/var`, bind mounts and
volumes holding old aliases) to `scan_roots` below, using physical paths with
no symlink components; review nested boundaries too. Do not scan kernel virtual
filesystems such as `/proc`, `/sys` or `/dev`. Unavailable/unmounted filesystems,
remote filesystems and snapshots may contain additional capable copies; inspect
and remediate them offline before reuse. This scan neither authenticates bytes
nor revokes process-held capabilities.

Only after lockdown, revocation and scanning does preparation check the
quarantine destination. It renames relative to the `/opt` descriptor with Linux
`renameat2(RENAME_NOREPLACE)`; an existing destination always causes STOP and
is preserved. Unsupported renameat2 also stops. A prefix symlink is renamed,
never followed. Save the printed original owning UID for the final cleanup.

<!-- aprime-prepare -->
```bash
quarantine_uid=$(/usr/bin/sudo /usr/bin/python3 -I - <<'PYRECOVER'
import ctypes, errno, os, stat, subprocess, sys

def stop(message):
    sys.exit("STOP: " + message)

clean_env = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin"}
missing_xattr = (errno.ENODATA, errno.EOPNOTSUPP)
def capability(core_fd):
    try:
        return os.getxattr(core_fd, "security.capability")
    except OSError as error:
        if error.errno not in missing_xattr:
            raise
        return b""

def revoke(core_fd, label):
    info = os.fstat(core_fd)
    if not stat.S_ISREG(info.st_mode):
        stop("not a regular file: " + repr(label))
    identity = (info.st_dev, info.st_ino)
    if capability(core_fd):
        os.removexattr(core_fd, "security.capability")
    result = subprocess.run(
        ["/usr/sbin/getcap", "/proc/self/fd/" + str(core_fd)],
        pass_fds=(core_fd,), env=clean_env, check=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    if result.stdout or result.stderr or capability(core_fd):
        stop("capability revocation verification failed: " + repr(label))
    print(f"REVOKED {label!r} dev,ino={identity}; getcap empty", file=sys.stderr)

# Edit this list to include ALL other relevant local mount roots (see above).
scan_roots = ["/", "/opt"]
trusted_paths = ("/usr/", "/bin/", "/sbin/", "/lib/", "/lib64/")
def safe(info):
    return info.st_uid == 0 and info.st_gid == 0 and not info.st_mode & 0o6022

def scan(directory_fd, path, device, trusted_ancestors):
    with os.scandir(directory_fd) as entries:
        for entry in entries:
            info = entry.stat(follow_symlinks=False)
            name = path.rstrip("/") + "/" + entry.name
            if info.st_dev != device:
                print("SCAN boundary (review mount coverage): " + repr(name), file=sys.stderr)
                continue
            if stat.S_ISLNK(info.st_mode):
                continue
            if not (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)):
                continue
            flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
            if stat.S_ISDIR(info.st_mode):
                flags |= os.O_DIRECTORY
            child = os.open(entry.name, flags, dir_fd=directory_fd)
            try:
                opened = os.fstat(child)
                if (opened.st_dev, opened.st_ino, stat.S_IFMT(opened.st_mode)) != (
                        info.st_dev, info.st_ino, stat.S_IFMT(info.st_mode)):
                    stop("scan entry changed: " + repr(name))
                trusted = trusted_ancestors and safe(opened)
                if stat.S_ISDIR(opened.st_mode):
                    scan(child, name, device, trusted)
                elif capability(child):
                    print(f"CAP_SCAN found {name!r} uid={opened.st_uid} dev,ino="
                          f"{(opened.st_dev, opened.st_ino)}", file=sys.stderr)
                    if not trusted or not name.startswith(trusted_paths):
                        print("CAP_SCAN flagged: " + repr(name), file=sys.stderr)
                        revoke(child, name)
            finally:
                os.close(child)

# Open every root component physically; O_NOFOLLOW applies at each level.
def open_directory(path):
    if not path.startswith("/") or ".." in path.split("/"):
        stop("scan root must be an absolute physical path")
    current = os.open("/", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    trusted = safe(os.fstat(current))
    try:
        for component in filter(None, path.split("/")):
            child = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                            dir_fd=current)
            os.close(current)
            current = child
            trusted = trusted and safe(os.fstat(current))
        return current, trusted
    except BaseException:
        os.close(current)
        raise

opt = os.stat("/opt", follow_symlinks=False)
print(f"/opt: uid={opt.st_uid} gid={opt.st_gid} mode={stat.S_IMODE(opt.st_mode):04o}", file=sys.stderr)
if not stat.S_ISDIR(opt.st_mode):
    stop("/opt is a symlink or not a directory; administrator inspection required")
fd = os.open("/opt", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
try:
    opened = os.fstat(fd)
    if (opened.st_dev, opened.st_ino) != (opt.st_dev, opt.st_ino):
        stop("/opt changed during inspection")
    os.fchown(fd, 0, 0)
    os.fchmod(fd, 0o755)
    prefix = os.stat("nekoray", dir_fd=fd, follow_symlinks=False)
    print(f"/opt/nekoray: uid={prefix.st_uid} gid={prefix.st_gid} mode={stat.S_IMODE(prefix.st_mode):04o}", file=sys.stderr)
    if stat.S_ISDIR(prefix.st_mode):
        prefix_fd = os.open("nekoray", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
        try:
            actual = os.fstat(prefix_fd)
            if (actual.st_dev, actual.st_ino) != (prefix.st_dev, prefix.st_ino):
                stop("legacy prefix changed")
            core_fd = os.open("nekobox_core", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                              dir_fd=prefix_fd)
            try:
                revoke(core_fd, "/opt/nekoray/nekobox_core")
            finally:
                os.close(core_fd)
        finally:
            os.close(prefix_fd)
    elif not stat.S_ISLNK(prefix.st_mode):
        stop("legacy prefix is neither directory nor symlink")
    for root in scan_roots:
        root_fd, trusted = open_directory(root)
        try:
            scan(root_fd, root, os.fstat(root_fd).st_dev, trusted)
        finally:
            os.close(root_fd)
    try:
        os.stat("nekoray.untrusted", dir_fd=fd, follow_symlinks=False)
    except FileNotFoundError:
        pass
    else:
        stop("/opt/nekoray.untrusted already exists; preserve it and inspect manually")
    libc = ctypes.CDLL(None, use_errno=True)
    rename = libc.renameat2
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    if rename(fd, b"nekoray", fd, b"nekoray.untrusted", 1) != 0:  # RENAME_NOREPLACE
        stop("quarantine rename refused: " + os.strerror(ctypes.get_errno()))
    quarantined = os.stat("nekoray.untrusted", dir_fd=fd, follow_symlinks=False)
    if (quarantined.st_dev, quarantined.st_ino) != (prefix.st_dev, prefix.st_ino):
        stop("legacy prefix changed during quarantine")
    print(quarantined.st_uid)
finally:
    os.close(fd)
PYRECOVER
) || { echo "STOP: recovery preparation failed" >&2; false; }
```

If this block fails, **STOP**; do not run dpkg removal against an untrusted path.
On success, `/opt` is root:root 0755 and the old prefix is absent. Now reconcile
package state before installing the new package (set `new_deb` to the absolute
path of your trusted new Debian package):

<!-- aprime-remove -->
```bash
/usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/bin/dpkg --remove --force-remove-reinstreq nekoray &&
/usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/bin/dpkg --purge nekoray
```

<!-- aprime-install -->
```bash
/usr/bin/sudo /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/bin/apt-get install -y "$new_deb"
```

Stop on any error. The authenticated upstream 4.0.1 archive has neither `prerm`
nor `postrm`; CI verifies both are absent. From Half-Installed, forced removal
runs no old removal script and does not call the old `postinst`. With the prefix
renamed first, dpkg's recorded `/opt/nekoray/*` paths are absent, including when
the quarantined entry is a symlink to an outside directory. The following plain
purge succeeds, and installation creates a new root-owned prefix. The old
untracked desktop launcher can remain until the new package adopts it.

After successful installation, delete the quarantine **as its original owning
UID**, never recursively as root. Root-owned 0755 `/opt` does not permit that
UID to unlink the quarantine entry. This block therefore renames the quarantine
into a fresh private disposal directory in `/var/tmp`, hands only that disposal
directory to the recorded UID, then runs `rm` as that UID. The original UID
must have a local account usable by `sudo`; never substitute root. A quarantined symlink
is unlinked without traversing its target. A UID of zero stops for manual
inspection; permission errors also require inspection, not a root recursive retry.

<!-- aprime-dispose -->
```bash
disposal=$(/usr/bin/sudo /usr/bin/python3 -I - "$quarantine_uid" <<'PYDISPOSE'
import os, sys, tempfile
uid = int(sys.argv[1])
if uid <= 0:
    sys.exit("STOP: quarantine owner is root; inspect manually")
if os.stat("/opt/nekoray.untrusted", follow_symlinks=False).st_uid != uid:
    sys.exit("STOP: quarantine owner changed; inspect manually")
disposal = tempfile.mkdtemp(prefix="nekoray-dispose-", dir="/var/tmp")
os.rename("/opt/nekoray.untrusted", disposal + "/nekoray.untrusted")
os.chown(disposal, uid, -1)
print(disposal)
PYDISPOSE
) && /usr/bin/sudo -u "#$quarantine_uid" -- /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin /usr/bin/rm -rf -- "$disposal"
```

If `/var/tmp` is on a different filesystem, the rename fails safely; an
administrator must choose a trusted disposal parent on the same filesystem.
No command above changes ownership or modes inside the quarantined tree.
Capability revocation occurs before quarantine/disposal, so disposal is not the
security boundary for surviving hardlinks or moved aliases.
Ordinary new-package postrm remove/purge actions remain available without a
prefix guard.

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

The archive validator requires exact root-owned executable preinst/postinst/postrm
contents matching `libs/debian/`, rejects stale/omitted scripts, extra prerm/config entry points and forbidden
repair commands; postrm must include the full read-only guard and unwind dispatch, and rejects unsafe payload ownership/modes. The Python tests
use native traversal/mode/link checks in a translated scratch tree, simulate
numeric root ownership/device boundaries on unprivileged hosts, and verify
refusal, command failures, a synchronized path replacement and outside sentinel
owner/mode/content preservation. A static guard prevents chown/chmod (and other
repair commands) from returning to the maintainer scripts.

The build matrix's `deb-upgrade` jobs run
`bash tests/verify_debian_lifecycle.sh package.deb` on disposable Ubuntu 22.04
and 24.04 runners in matching clean Ubuntu containers. Hosted runner `/opt` can
fail the required ancestor check; the container supplies trusted ancestors without
weakening the
production refusal or changing host directory ownership/modes. They use the same
run's newly built package, verify the exact
upstream 4.0.1 SHA256 before installation, require preinst and postrm refusal,
assert the exact Half-Installed state, then execute the documented dpkg
A-prime quarantine/forced-remove/purge/reinstall/disposal remedy from the blocks
above. Additional cases retain the genuine capable 4.0.1 core, create hardlinks
as UID 1001 outside the prefix, move that inode as UID 1001 after quarantine,
and plant a separate UID-1001-owned capable inode for scan/revocation. Nonempty
capabilities before recovery are required positive controls. A synchronized
UID-1001 quarantine collision between the early observation and lockdown must
STOP and preserve the planted object; an ordinary-rename control demonstrates
the old overwrite hazard. Replacement native core bytes have no capability before
or after refusal. A test-only repack of 4.0.1 traces its original postinst with a
configure positive control and proves it is never called during unsafe unwind.
It also tests an attacker-owned nested directory with a planted symlink, and a
prefix symlink to an outside native-file sentinel. The sentinel's owner, mode,
bytes and all xattrs (including security.capability and a planted user xattr)
must remain unchanged through refusal, removal, reinstall and unprivileged
quarantine disposal. The prefix-symlink case uses the same quarantine procedure
without restoring the old prefix. No test hook is shipped.
Every complete recovery also runs the exact blocks from a UID 1001-owned
directory with planted Python modules, startup hooks and command names, hostile
`PYTHONPATH`/`PYTHONSTARTUP`/`PYTHONUSERBASE`, and a sudo policy preserving those
variables. No planted marker may appear. A root non-isolated interpreter must
import the planted `tempfile.py` as a positive control before recovery.
Fresh install, actual apt reinstall of a safe tree, safe reconfiguration,
forced failed-upgrade unwind over a safe tree (postrm exit zero), numeric
ownership and modes of every prefix entry, capability, dpkg-tracked launcher,
and removal/purge are asserted. Package-manager bookkeeping still runs during
refusal; these tests do not claim that apt/dpkg makes no writes anywhere.
This script changes system packages; run it only in a disposable test environment.
