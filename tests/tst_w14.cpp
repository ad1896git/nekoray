#include <QtTest>
#include <random>

#include "main/pure/ArgCodec.hpp"

class W14Test : public QObject {
    Q_OBJECT

private slots:
    void decode_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QStringList>("expected");

        QTest::newRow("empty") << QString() << QStringList();
        QTest::newRow("whitespace")
            << QStringLiteral(" \t\r\n ") << QStringList();
        QTest::newRow("repeated-space")
            << QStringLiteral("a  b") << QStringList({"a", "b"});
        QTest::newRow("empty-argument")
            << QStringLiteral("a \"\" b") << QStringList({"a", "", "b"});
        QTest::newRow("empty-single-quote")
            << QStringLiteral("''") << QStringList({""});
        QTest::newRow("concatenated-segments")
            << QStringLiteral("ab\" c\"'d'") << QStringList({"ab cd"});
        QTest::newRow("outside-backslash")
            << QStringLiteral("C:\\path\\file") << QStringList({"C:\\path\\file"});
        QTest::newRow("outside-backslash-before-space")
            << QStringLiteral("a\\ b") << QStringList({"a\\", "b"});
        QTest::newRow("unknown-double-escape")
            << QStringLiteral("\"a\\qb\"") << QStringList({"a\\qb"});
        QTest::newRow("double-quote-escape")
            << QStringLiteral("\"a\\\"b\"") << QStringList({"a\"b"});
        QTest::newRow("double-backslash-escape")
            << QStringLiteral("\"a\\\\b\"") << QStringList({"a\\b"});
        QTest::newRow("single-quote-literal")
            << QStringLiteral("'a\\\"b'") << QStringList({"a\\\"b"});
        QTest::newRow("literal-shell-characters")
            << QStringLiteral("$HOME $(id) %1 ;")
            << QStringList({"$HOME", "$(id)", "%1", ";"});
        QTest::newRow("unicode-space")
            << (QStringLiteral("a") + QChar(0x2003) + QStringLiteral("b"))
            << QStringList({"a", "b"});
    }

    void decode() {
        QFETCH(QString, text);
        QFETCH(QStringList, expected);
        QStringList actual;
        QVERIFY(NkrInput::decodeArgs(text, actual));
        QCOMPARE(actual, expected);
    }

    void malformedLeavesOutputUnchanged() {
        const QStringList malformed{
            QStringLiteral("\""),
            QStringLiteral("'"),
            QStringLiteral("ok \"bad"),
            QStringLiteral("\"ends\\"),
            QStringLiteral("'ends\\")
        };
        for (const QString &text : malformed) {
            QStringList actual{"existing", ""};
            const QStringList previous = actual;
            QVERIFY(!NkrInput::decodeArgs(text, actual));
            QCOMPARE(actual, previous);
        }
    }

    void canonicalEncoding() {
        QCOMPARE(NkrInput::encodeArgs(QStringList()), QString());
        QCOMPARE(NkrInput::encodeArgs(QStringList({""})), QStringLiteral("\"\""));
        QCOMPARE(NkrInput::encodeArgs(QStringList({"a", "b"})), QStringLiteral("a b"));
        QCOMPARE(NkrInput::encodeArgs(QStringList({"a b"})), QStringLiteral("\"a b\""));
        QCOMPARE(NkrInput::encodeArgs(QStringList({"a'b"})), QStringLiteral("\"a'b\""));
        QCOMPARE(NkrInput::encodeArgs(QStringList({"a\"b"})),
                 QStringLiteral("\"a\\\"b\""));
        QCOMPARE(NkrInput::encodeArgs(QStringList({"C:\\Program Files\\x\\"})),
                 QStringLiteral("\"C:\\\\Program Files\\\\x\\\\\""));

        const QStringList original{
            "", "C:\\Program Files\\x\\", "a  b", "a\"b", "a'b", "\\",
            "\t", "\n", "\r", "$HOME", "%config%", "%1",
            QString::fromUtf8("中文")
        };
        QStringList decoded;
        QVERIFY(NkrInput::decodeArgs(NkrInput::encodeArgs(original), decoded));
        QCOMPARE(decoded, original);
        QCOMPARE(NkrInput::encodeArgs(decoded), NkrInput::encodeArgs(original));
    }

    void randomRoundTrip() {
        std::mt19937 random(0x11A14u);
        const QString alphabet =
            QString::fromUtf8("ab09 \t\n\r\"'\\$%;中文") + QChar(0x2003);

        for (int trial = 0; trial < 200000; ++trial) {
            QStringList original;
            const int count = static_cast<int>(random() % 9u);
            for (int i = 0; i < count; ++i) {
                QString arg;
                const int length = static_cast<int>(random() % 21u);
                for (int j = 0; j < length; ++j) {
                    arg += alphabet.at(
                        static_cast<int>(random() %
                                         static_cast<unsigned>(alphabet.size())));
                }
                original.append(arg);
            }

            QStringList decoded;
            const bool ok =
                NkrInput::decodeArgs(NkrInput::encodeArgs(original), decoded);
            const QByteArray context =
                QStringLiteral("seed=0x11A14 trial=%1").arg(trial).toUtf8();
            QVERIFY2(ok, context.constData());
            QVERIFY2(decoded == original, context.constData());
        }
    }
};

QTEST_GUILESS_MAIN(W14Test)
#include "tst_w14.moc"
