#pragma once

#include <QByteArray>
#include <QProcess>
#include <QString>
#include <QStringList>

// macOS Tun launcher helpers (PR-22). Pure functions; they compile on every
// platform so that their behaviour is unit-testable without privileges.
namespace NekoGui_sys {

// Root runtime directory used by res/vpn/vpn-run-root-macos.sh.
inline constexpr const char *kMacVpnRunDir = "/var/run/nekoray-vpn";

// Reads the embedded script. It is never read from disk.
QString ReadMacVpnScript(QString &error);

// POSIX shell single-quoted word.
QString ShellQuote(const QString &value);

// AppleScript string literal, including the surrounding double quotes.
QString AppleScriptStringLiteral(const QString &value);

// "exec /bin/bash -p -c '<script>' nekoray-vpn '<arg>'..."
QString MacVpnShellCommand(const QString &script, const QStringList &arguments);

// AppleScript text for osascript's standard input. `admin` adds
// "with administrator privileges"; tests must pass false.
QString MacVpnAppleScript(const QString &script,
                          const QStringList &arguments,
                          bool admin);

// Lowercase hex SHA-256 of a file, or empty with `error` set.
QString Sha256File(const QString &path, QString &error);
QString Sha256Hex(const QByteArray &data);

// Constrain the separate root core to Tun + SOCKS/direct routing. File assets
// are hashed here and rewritten to root-owned copies. assetArguments contains
// ordered <absolute source, sha256> pairs for the supervisor (asset-0, ...).
// Experimental writers/APIs and log.output are disabled. Unsupported external
// file references and remote rule-set downloads fail before authorization.
QByteArray PrepareMacVpnConfig(const QByteArray &config, const QString &baseDir,
                               QStringList &assetArguments, QString &error);

// sing-box on macOS requires utunN interface names.
bool IsMacTunInterfaceName(const QString &name);

// The GUI is never root on macOS, so the single-core Tun mode (main core
// owning the tun inbound) only applies when the GUI itself runs as root.
inline bool MacUsesInternalTun(bool configured, bool isRoot) {
    return configured && isRoot;
}

struct MacVpnExit {
    enum Kind {
        Stopped,    // normal stop, or a stop the GUI requested
        Cancelled,  // the user cancelled the administrator prompt
        AuthFailed, // authorization failed (for example, wrong password)
        Failed,     // the launcher or supervisor reported an error
    };
    Kind kind = Failed;
    int code = 0;    // script exit status or AppleScript error number
    QString message; // user-facing text; empty for Stopped/Cancelled
};

// Classifies the end of an `osascript` launcher from its exit status and
// captured standard error ("...execution error: <text> (<code>)").
MacVpnExit ClassifyMacVpnExit(int exitCode,
                              QProcess::ExitStatus exitStatus,
                              const QByteArray &standardError,
                              bool stopRequested);

} // namespace NekoGui_sys
