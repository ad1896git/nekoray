#include "LinuxVpnLaunch.hpp"
#include "ExternalTempFiles.hpp"

#include <QFile>
#include <QUuid>

namespace NekoGui_sys {

QString ReadLinuxVpnScript(QString &error) {
    error.clear();
    QFile file(QStringLiteral(":/neko/vpn/vpn-run-root.sh"));
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("Cannot open embedded Linux VPN script.");
        return {};
    }

    const QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError ||
        contents.isEmpty() || contents.contains('\0')) {
        error = QStringLiteral("Cannot read embedded Linux VPN script.");
        return {};
    }

    return QString::fromUtf8(contents);
}

QStringList LinuxVpnArguments(
    const QString &script,
    const QStringList &parameters) {
    QStringList arguments{
        QStringLiteral("/bin/bash"),
        QStringLiteral("-c"),
        script,
        QStringLiteral("nekoray-vpn")
    };
    arguments.append(parameters);
    return arguments;
}

QString WriteLinuxVpnConfig(
    ExternalTempFiles &files,
    const QByteArray &contents,
    QString &error) {
    const QString name =
        QStringLiteral("vpn-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces) +
        QStringLiteral(".json");

    QString path;
    if (!files.write(name, contents, path, error))
        return {};
    return path;
}

} // namespace NekoGui_sys
