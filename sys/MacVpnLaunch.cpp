#include "MacVpnLaunch.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <functional>

namespace NekoGui_sys {

namespace {

QString tr(const char *text) {
    return QCoreApplication::translate("MacVpnLaunch", text);
}

QString codeMessage(int code) {
    switch (code) {
        case 64:
            return tr("The Tun launcher rejected its arguments.");
        case 66:
            return tr("The Tun launch was cancelled before it started.");
        case 69:
            return tr("A macOS tool required by Tun mode is missing.");
        case 70:
            return tr("nekobox_core exited unexpectedly in Tun mode.");
        case 74:
            return tr("Tun state handling or cleanup failed. Use Tun Settings, "
                      "Troubleshooting, Reset.");
        case 75:
            return tr("Another Tun instance is running, or the Tun interface "
                      "already exists. Turn that Tun off, or restart macOS.");
        case 76:
            return tr("nekobox_core or the Tun configuration changed during "
                      "authorization. Try again.");
        case 77:
            return tr("Tun mode was refused for safety (unsafe file ownership "
                      "or permissions).");
        default:
            return tr("The Tun launcher failed (code %1).").arg(code);
    }
}

} // namespace

QString ReadMacVpnScript(QString &error) {
    error.clear();
    QFile file(QStringLiteral(":/neko/vpn/vpn-run-root-macos.sh"));
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot open embedded macOS VPN script.");
        return {};
    }
    const QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError || contents.isEmpty() ||
        contents.contains('\0')) {
        error = QStringLiteral("Cannot read embedded macOS VPN script.");
        return {};
    }
    return QString::fromUtf8(contents);
}

QString ShellQuote(const QString &value) {
    QString quoted = value;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString AppleScriptStringLiteral(const QString &value) {
    QString escaped;
    escaped.reserve(value.size() + 16);
    escaped += QLatin1Char('"');
    for (const QChar c : value) {
        switch (c.unicode()) {
            case '\\': escaped += QStringLiteral("\\\\"); break;
            case '"': escaped += QStringLiteral("\\\""); break;
            case '\n': escaped += QStringLiteral("\\n"); break;
            case '\r': escaped += QStringLiteral("\\r"); break;
            case '\t': escaped += QStringLiteral("\\t"); break;
            default: escaped += c; break;
        }
    }
    escaped += QLatin1Char('"');
    return escaped;
}

QString MacVpnShellCommand(const QString &script, const QStringList &arguments) {
    QStringList words{
        QStringLiteral("exec"),
        QStringLiteral("/bin/bash"),
        QStringLiteral("-p"), // ignore BASH_ENV and imported shell functions
        QStringLiteral("-c"),
        ShellQuote(script),
        QStringLiteral("nekoray-vpn"),
    };
    for (const auto &argument : arguments)
        words << ShellQuote(argument);
    return words.join(QLatin1Char(' '));
}

QString MacVpnAppleScript(const QString &script,
                          const QStringList &arguments,
                          bool admin) {
    QString text = QStringLiteral("do shell script ") +
                   AppleScriptStringLiteral(MacVpnShellCommand(script, arguments));
    if (admin)
        text += QStringLiteral(" with administrator privileges");
    return text + QLatin1Char('\n');
}

QString Sha256Hex(const QByteArray &data) {
    return QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString Sha256File(const QString &path, QString &error) {
    error.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot open %1 for hashing.").arg(path);
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || file.error() != QFileDevice::NoError) {
        error = QStringLiteral("Cannot read %1 for hashing.").arg(path);
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

QByteArray PrepareMacVpnConfig(const QByteArray &config, const QString &baseDir,
                               QStringList &assetArguments, QString &error) {
    error.clear();
    assetArguments.clear();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(config, &parseError);
    auto fail = [&](const QString &detail) -> QByteArray {
        assetArguments.clear();
        error = tr("Unsafe or unsupported macOS Tun configuration: %1").arg(detail);
        return {};
    };
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(tr("expected a JSON object without comments"));
    auto root = document.object();
    const QStringList sections{"$schema", "log", "dns", "inbounds", "outbounds",
                               "route", "experimental"};
    for (auto it = root.begin(); it != root.end(); ++it)
        if (!sections.contains(it.key())) return fail(it.key());
    // The supervisor captures stdout/stderr. No config-selected root writes,
    // cache databases, external UI downloads, debug listeners or API servers.
    auto log = root.value("log").toObject();
    log.remove("output");
    root["log"] = log;
    root.remove("experimental");
    const auto inbounds = root.value("inbounds").toArray();
    if (inbounds.size() != 1 ||
        inbounds.first().toObject().value("type").toString() != "tun")
        return fail(tr("only one tun inbound is supported"));
    for (const auto &outbound : root.value("outbounds").toArray()) {
        const auto type = outbound.toObject().value("type").toString();
        if (!QStringList{"socks", "direct", "block", "dns"}.contains(type))
            return fail(tr("unsupported outbound %1").arg(type));
    }

    QStringList assetTargets;
    auto asset = [&](QJsonObject &options, const QString &fallback) -> bool {
        if (assetTargets.size() >= 128) {
            error = tr("too many file assets (maximum 128)");
            return false;
        }
        auto path = options.value("path").toString();
        if (path.isEmpty()) path = fallback;
        const auto source = QDir(baseDir).absoluteFilePath(path);
        if (!QFileInfo(source).isFile() || QFileInfo(source).size() <= 0) {
            error = tr("missing or empty asset %1").arg(source);
            return false;
        }
        const auto hash = Sha256File(source, error);
        if (hash.isEmpty()) return false;
        const auto target = QString::fromLatin1(kMacVpnRunDir) +
                            "/asset-" + QString::number(assetTargets.size());
        assetArguments << source << hash;
        assetTargets << target;
        options = QJsonObject{{"path", target}}; // disable download fallback
        return true;
    };
    // A geo match without explicit database options loads the core's default
    // database. Make those implicit reads explicit and verified as well.
    std::function<bool(const QJsonValue &, const QString &)> usesKey;
    usesKey = [&](const QJsonValue &v, const QString &key) {
        if (v.isArray()) {
            for (const auto &child : v.toArray()) if (usesKey(child, key)) return true;
        } else if (v.isObject()) {
            const auto object = v.toObject();
            if (object.contains(key)) return true;
            for (auto it = object.begin(); it != object.end(); ++it)
                if (usesKey(it.value(), key)) return true;
        }
        return false;
    };
    auto route = root.value("route").toObject();
    for (const QString key : {QStringLiteral("geoip"), QStringLiteral("geosite")}) {
        if (route.contains(key) || usesKey(route.value("rules"), key) ||
            usesKey(root.value("dns"), key)) {
            auto options = route.value(key).toObject();
            if (!asset(options, key + ".db")) return fail(error);
            route[key] = options;
        }
    }
    auto ruleSets = route.value("rule_set").toArray();
    for (int i = 0; i < ruleSets.size(); ++i) {
        auto rule = ruleSets.at(i).toObject();
        if (rule.value("type").toString() != "local" ||
            rule.value("path").toString().isEmpty())
            return fail(tr("rule sets must be local files"));
        auto path = QJsonObject{{"path", rule.value("path")}};
        if (!asset(path, {})) return fail(error);
        rule["path"] = path.value("path");
        ruleSets[i] = rule;
    }
    if (route.contains("rule_set")) route["rule_set"] = ruleSets;
    root["route"] = route;
    // Other file/socket/executable paths are outside this root core's scope.
    // process_path is a routing matcher, never a file the core opens.
    std::function<bool(const QJsonValue &)> safe;
    safe = [&](const QJsonValue &v) {
        if (v.isArray()) {
            for (const auto &child : v.toArray()) if (!safe(child)) return false;
        } else if (v.isObject()) {
            const auto object = v.toObject();
            for (auto it = object.begin(); it != object.end(); ++it) {
                const auto key = it.key();
                if (key != "process_path" &&
                    (key == "path" || key.endsWith("_path") ||
                     key.endsWith("_file") || key.endsWith("_directory") ||
                     key == "output" || key == "external_ui")) {
                    if (key != "path" || !assetTargets.contains(it.value().toString())) {
                        error = key;
                        return false;
                    }
                }
                if (!safe(it.value())) return false;
            }
        }
        return true;
    };
    if (!safe(root)) return fail(error);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool IsMacTunInterfaceName(const QString &name) {
    static const QRegularExpression pattern(
        QStringLiteral("^utun[0-9]{1,3}$"));
    return pattern.match(name).hasMatch();
}

MacVpnExit ClassifyMacVpnExit(int exitCode,
                              QProcess::ExitStatus exitStatus,
                              const QByteArray &standardError,
                              bool stopRequested) {
    MacVpnExit result;
    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        result.kind = MacVpnExit::Stopped;
        return result;
    }

    QString text = QString::fromUtf8(standardError);
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    static const QRegularExpression pattern(
        QStringLiteral("execution error: (.*) \\((-?[0-9]+)\\)\\s*$"),
        QRegularExpression::DotMatchesEverythingOption);
    const auto match = pattern.match(text);
    bool parsed = false;
    QString detail;
    if (match.hasMatch()) {
        result.code = match.captured(2).toInt(&parsed);
        detail = match.captured(1).trimmed();
    }

    // A stop the GUI asked for ends silently unless cleanup failed.
    if (stopRequested && !(parsed && result.code == 74)) {
        result.kind = MacVpnExit::Stopped;
        return result;
    }

    if (!parsed) {
        result.kind = MacVpnExit::Failed;
        result.code = exitCode;
        result.message =
            tr("The Tun launcher exited (code %1, %2).")
                .arg(exitCode)
                .arg(exitStatus == QProcess::CrashExit ? tr("crashed")
                                                       : tr("normal exit"));
        const auto trimmed = text.trimmed();
        if (!trimmed.isEmpty())
            result.message += QStringLiteral("\n\n") + trimmed.right(4000);
        return result;
    }

    if (result.code == -128) {
        result.kind = MacVpnExit::Cancelled;
        return result;
    }
    if (result.code < 0) {
        result.kind = MacVpnExit::AuthFailed;
        result.message =
            tr("macOS authorization failed (error %1). Tun mode remains off.")
                .arg(result.code);
        if (!detail.isEmpty())
            result.message += QStringLiteral("\n\n") + detail.right(4000);
        return result;
    }

    result.kind = MacVpnExit::Failed;
    result.message = codeMessage(result.code);
    if (!detail.isEmpty())
        result.message += QStringLiteral("\n\n") + detail.right(4000);
    return result;
}

} // namespace NekoGui_sys
