#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include "main/pure/VpnAuth.hpp"

class W13Test : public QObject {
    Q_OBJECT

private slots:
    void authRoundTrip() {
        QFile file(QString::fromUtf8(NKR_VPN_TEMPLATE_PATH));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(file.readAll());
        QVERIFY(source.contains(QStringLiteral("//%SOCKS_USER_PASS%")));

        QString prepared = source;
        prepared.replace(QStringLiteral("//%IPV6_ADDRESS%"), QString());
        prepared.replace(QStringLiteral("//%PROCESS_NAME_RULE%"), QString());
        prepared.replace(QStringLiteral("//%CIDR_RULE%"), QString());
        prepared.replace(QStringLiteral("%MTU%"), QStringLiteral("1500"));
        prepared.replace(QStringLiteral("%STACK%"), QStringLiteral("system"));
        prepared.replace(QStringLiteral("%TUN_NAME%"), QStringLiteral("nkr-test"));
        prepared.replace(QStringLiteral("%STRICT_ROUTE%"), QStringLiteral("false"));
        prepared.replace(QStringLiteral("%FINAL_OUT%"), QStringLiteral("direct"));
        prepared.replace(QStringLiteral("%DNS_ADDRESS%"), QStringLiteral("local"));
        prepared.replace(QStringLiteral("%FAKE_DNS_INBOUND%"), QStringLiteral("empty"));
        prepared.replace(QStringLiteral("%PORT%"), QStringLiteral("2080"));

        const QStringList values{
            QStringLiteral("ordinary"),
            QString(),
            QStringLiteral("quote\" backslash\\ newline\n tab\t return\r"),
            QString::fromUtf8("用户名 密码"),
            QStringLiteral("%1 %2 %PORT% %STACK% %DNS_ADDRESS% "
                           "//%SOCKS_USER_PASS%"),
            QStringLiteral("\",\"server\":\"evil\",\"password\":\"injected"),
            QStringLiteral("before") + QChar(0) + QStringLiteral("after")
        };

        const QString noAuth = NkrInput::applyVpnAuth(
            prepared, false, QStringLiteral("ignored"), QStringLiteral("ignored"));
        QJsonParseError error;
        const QJsonDocument baseline = QJsonDocument::fromJson(noAuth.toUtf8(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(baseline.isObject());

        const QJsonArray baselineOutbounds =
            baseline.object().value(QStringLiteral("outbounds")).toArray();
        QVERIFY(!baselineOutbounds.isEmpty());
        const QJsonObject baselineSocks = baselineOutbounds.at(0).toObject();
        QVERIFY(!baselineSocks.contains(QStringLiteral("username")));
        QVERIFY(!baselineSocks.contains(QStringLiteral("password")));

        for (const QString &username : values) {
            for (const QString &password : values) {
                const QString result =
                    NkrInput::applyVpnAuth(prepared, true, username, password);
                const QJsonDocument actual =
                    QJsonDocument::fromJson(result.toUtf8(), &error);
                QCOMPARE(error.error, QJsonParseError::NoError);
                QVERIFY(actual.isObject());

                QJsonObject expected = baseline.object();
                QJsonArray outbounds = baselineOutbounds;
                QJsonObject socks = baselineSocks;
                socks.insert(QStringLiteral("username"), username);
                socks.insert(QStringLiteral("password"), password);
                outbounds.replace(0, socks);
                expected.insert(QStringLiteral("outbounds"), outbounds);
                QCOMPARE(actual.object(), expected);

                QCOMPARE(NkrInput::applyVpnAuth(prepared, false, username, password),
                         noAuth);
            }
        }
    }

    void customTemplateCompatibility() {
        const QString custom =
            QStringLiteral("{\"custom\":true,\"proxy\":{"
                           "//%SOCKS_USER_PASS%\"server\":\"localhost\"}}");
        QJsonParseError error;
        const QJsonDocument result = QJsonDocument::fromJson(
            NkrInput::applyVpnAuth(
                custom, true, QStringLiteral("u\""), QStringLiteral("p\\"))
                .toUtf8(),
            &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(result.object().value(QStringLiteral("custom")).toBool());
        const QJsonObject proxy =
            result.object().value(QStringLiteral("proxy")).toObject();
        QCOMPARE(proxy.value(QStringLiteral("username")).toString(),
                 QStringLiteral("u\""));
        QCOMPARE(proxy.value(QStringLiteral("password")).toString(),
                 QStringLiteral("p\\"));
        QCOMPARE(proxy.value(QStringLiteral("server")).toString(),
                 QStringLiteral("localhost"));

        const QString withoutMarker = QStringLiteral("{\"custom\":true}");
        QCOMPARE(NkrInput::applyVpnAuth(
                     withoutMarker, true, QStringLiteral("u"), QStringLiteral("p")),
                 withoutMarker);
    }
};

QTEST_GUILESS_MAIN(W13Test)
#include "tst_w13.moc"
