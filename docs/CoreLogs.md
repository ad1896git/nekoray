# Core log location and migration (LN#1)

The GUI log pane receives the core's stdout/stderr; it does not read `neko.log`.
The ordinary `nekobox` core now writes its optional file log to
`<selected config directory>/logs/neko.log`. The GUI passes that absolute config
directory as `-data-dir`, including when restarting the core.

- Installed/appdata mode: the config directory is `config` below the GUI's
  platform app-config location, or below the explicitly selected `-appdata` root.
- Portable mode: the config directory is `config` beside the GUI executable.
- Direct core invocation: pass `nekobox -data-dir /absolute/path/to/config` along
  with the normal RPC arguments. The config directory must already exist.
  Omitting the flag or giving a relative path disables file logging.
- The privileged VPN `run -c ...` entry point uses sing-box's logging and does
  not call this setup. This change does not move that entry point's logs.

On Unix a new `logs` directory uses `0700` and a new log uses `0600`. libneko
validates the owner, permissions, file type, link count and ancestors through
file descriptors; symlinks in any path component are refused. On macOS this
includes aliases such as `/var` and extended ACL grants. Use a physical path
such as `/private/var` for custom appdata directories. Windows uses libneko's
owner-only protected file ACL and rejects reparse points and unsafe ancestors.
Existing directories and files are never chmodded or given new ACLs automatically.

If the path is unsafe, read-only or unavailable, the core continues with console
logging and reports `set up core log: ... file logging disabled ...` in the GUI
log pane. No alternative file path is selected. Existing private logs at the new
location continue to use libneko's 50 KiB startup tail limit.

Old `./neko.log` files (usually `<selected config directory>/neko.log`) are left
untouched: they are not copied, deleted, reopened, or tightened. They may contain
sensitive history and retain their old permissions. Preserve them for review;
any archiving, permission change or deletion is an owner decision pending LN#1.
An unsafe file already at the new location is also preserved and refused. This
change provides a new private log; it does not remediate previously exposed data.

For log collection or export, collect the new file above if available, or copy
from the GUI log pane. There is no file-reader/export feature to reconfigure.
