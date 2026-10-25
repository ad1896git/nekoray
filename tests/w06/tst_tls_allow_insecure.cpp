#include <QtTest/QtTest>
#include <QJsonObject>
#include <QScopedValueRollback>
#include <QStringList>

#include "fmt/TrojanVLESSBean.hpp"
#include "fmt/VMessBean.hpp"

class TlsAllowInsecureTest : public QObject {
    Q_OBJECT

private:
    template<typename Bean>
    void verifyImport(Bean &bean,
                      const QString &protocol,
                      const QString &suffix,
                      bool expectedFlag,
                      bool reuse,
                      bool skipCert) {
        const QString base =
            QStringLiteral(
                "%1://11111111-1111-4111-8111-111111111111@example.test:443"
                "?security=tls&type=tcp&sni=tls.example&fp=chrome")
                .arg(protocol);

        QCOMPARE(bean.stream->allow_insecure, false);

        if (reuse) {
            QVERIFY(bean.TryParseLink(
                base + QStringLiteral("&allowInsecure=1")));
            QCOMPARE(bean.stream->allow_insecure, true);
        }

        QVERIFY(bean.TryParseLink(base + suffix));
        QCOMPARE(bean.stream->allow_insecure, expectedFlag);
        QCOMPARE(bean.stream->security, QStringLiteral("tls"));
        QCOMPARE(bean.serverAddress, QStringLiteral("example.test"));
        QCOMPARE(bean.serverPort, 443);

        const auto result = bean.BuildCoreObjSingBox();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.outbound.value(QStringLiteral("type")).toString(),
                 protocol);

        const QJsonValue tlsValue =
            result.outbound.value(QStringLiteral("tls"));
        QVERIFY(tlsValue.isObject());

        const QJsonObject tls = tlsValue.toObject();
        const QJsonValue enabled = tls.value(QStringLiteral("enabled"));
        QVERIFY(enabled.isBool());
        QVERIFY(enabled.toBool());
        QCOMPARE(tls.value(QStringLiteral("server_name")).toString(),
                 QStringLiteral("tls.example"));

        const bool expectedInsecure = expectedFlag || skipCert;
        QCOMPARE(tls.contains(QStringLiteral("insecure")), expectedInsecure);

        if (expectedInsecure) {
            const QJsonValue insecure =
                tls.value(QStringLiteral("insecure"));
            QVERIFY(insecure.isBool());
            QVERIFY(insecure.toBool());
        }
    }

private slots:
    void importLink_data() {
        QTest::addColumn<QString>("protocol");
        QTest::addColumn<QString>("suffix");
        QTest::addColumn<bool>("expectedFlag");
        QTest::addColumn<bool>("reuse");
        QTest::addColumn<bool>("skipCert");

        struct QueryCase {
            const char *name;
            const char *suffix;
            bool expectedFlag;
        };

        const QueryCase cases[] = {
            {"absent", "", false},
            {"bare", "&allowInsecure", false},
            {"empty", "&allowInsecure=", false},
            {"zero", "&allowInsecure=0", false},
            {"one", "&allowInsecure=1", true},
            {"true", "&allowInsecure=true", true},
            {"false", "&allowInsecure=false", false},
            {"unknown", "&allowInsecure=xyz", false},
            {"two", "&allowInsecure=2", false},
            {"yes", "&allowInsecure=yes", false},
            {"uppercase", "&allowInsecure=TRUE", true},
            {"mixed-case", "&allowInsecure=TrUe", true},
            {"padded-true", "&allowInsecure=%20true%20", true},
            {"padded-zero", "&allowInsecure=%200%20", false},
            {"literal-plus", "&allowInsecure=+true+", false},
            {"trailing-junk", "&allowInsecure=truejunk", false},
            {"duplicate-zero-first",
             "&allowInsecure=0&allowInsecure=1", false},
            {"duplicate-one-first",
             "&allowInsecure=1&allowInsecure=0", true}
        };

        const QStringList protocols{
            QStringLiteral("trojan"),
            QStringLiteral("vless"),
            QStringLiteral("vmess")
        };

        for (const QString &protocol : protocols) {
            for (const QueryCase &queryCase : cases) {
                for (bool reuse : {false, true}) {
                    for (bool skipCert : {false, true}) {
                        const QByteArray name =
                            QStringLiteral("%1-%2-reuse%3-skip%4")
                                .arg(protocol)
                                .arg(QString::fromLatin1(queryCase.name))
                                .arg(reuse ? 1 : 0)
                                .arg(skipCert ? 1 : 0)
                                .toLatin1();

                        QTest::newRow(name.constData())
                            << protocol
                            << QString::fromLatin1(queryCase.suffix)
                            << queryCase.expectedFlag
                            << reuse
                            << skipCert;
                    }
                }
            }
        }
    }

    void importLink() {
        QFETCH(QString, protocol);
        QFETCH(QString, suffix);
        QFETCH(bool, expectedFlag);
        QFETCH(bool, reuse);
        QFETCH(bool, skipCert);

        QVERIFY(NekoGui::dataStore != nullptr);
        QScopedValueRollback<bool> restoreSkipCert(
            NekoGui::dataStore->skip_cert, skipCert);

        if (protocol == QStringLiteral("vmess")) {
            NekoGui_fmt::VMessBean bean;
            verifyImport(bean, protocol, suffix, expectedFlag, reuse, skipCert);
            return;
        }

        const int proxyType =
            protocol == QStringLiteral("vless")
                ? NekoGui_fmt::TrojanVLESSBean::proxy_VLESS
                : NekoGui_fmt::TrojanVLESSBean::proxy_Trojan;

        NekoGui_fmt::TrojanVLESSBean bean(proxyType);
        verifyImport(bean, protocol, suffix, expectedFlag, reuse, skipCert);
    }
};

QTEST_GUILESS_MAIN(TlsAllowInsecureTest)

#include "tst_tls_allow_insecure.moc"
