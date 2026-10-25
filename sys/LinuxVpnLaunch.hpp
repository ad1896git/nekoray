#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace NekoGui_sys {

class ExternalTempFiles;

QString ReadLinuxVpnScript(QString &error);

QStringList LinuxVpnArguments(
    const QString &script,
    const QStringList &parameters);

QString WriteLinuxVpnConfig(
    ExternalTempFiles &files,
    const QByteArray &contents,
    QString &error);

} // namespace NekoGui_sys
