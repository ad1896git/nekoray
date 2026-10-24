#include "VpnAuth.hpp"

#include <QJsonDocument>
#include <QJsonObject>

namespace NkrInput {

QString applyVpnAuth(QString config, bool enabled,
                     const QString &username, const QString &password) {
    QString members;
    if (enabled) {
        const QJsonObject auth{
            {QStringLiteral("username"), username},
            {QStringLiteral("password"), password}
        };
        const QByteArray object =
            QJsonDocument(auth).toJson(QJsonDocument::Compact);

        // The marker occupies an object-member position in the template.
        members = QString::fromUtf8(object.mid(1, object.size() - 2));
        members += QLatin1Char(',');
    }

    config.replace(QStringLiteral("//%SOCKS_USER_PASS%"), members);
    return config;
}

} // namespace NkrInput
