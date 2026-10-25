#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "main/pure/DesktopExec.hpp"

class W17Test : public QObject {
    Q_OBJECT

private slots:
    void serialization() {
        using NkrInput::desktopExecArg;
        const QString quote(1, QLatin1Char('"'));
        const QString slash(1, QLatin1Char('\\'));

        QCOMPARE(desktopExecArg(QString()), QStringLiteral("\"\""));
        QCOMPARE(desktopExecArg(QStringLiteral("plain")), QStringLiteral("plain"));
        QCOMPARE(desktopExecArg(QStringLiteral("a b")), QStringLiteral("\"a b\""));
        QCOMPARE(desktopExecArg(QStringLiteral("100%")), QStringLiteral("100%%"));
        QCOMPARE(desktopExecArg(QStringLiteral("%f")), QStringLiteral("%%f"));
        QCOMPARE(desktopExecArg(QStringLiteral("\\")),
                 quote + QString(4, QLatin1Char('\\')) + quote);
        QCOMPARE(desktopExecArg(QStringLiteral("$")),
                 quote + slash + slash + QStringLiteral("$") + quote);
        QCOMPARE(desktopExecArg(QStringLiteral("\"")),
                 quote + slash + slash + quote + quote);
        QCOMPARE(desktopExecArg(QStringLiteral("`")),
                 quote + slash + slash + QStringLiteral("`") + quote);
        QCOMPARE(desktopExecArg(QStringLiteral("\n")),
                 quote + QStringLiteral("\\n") + quote);
        QCOMPARE(desktopExecArg(QStringLiteral("\t")),
                 quote + QStringLiteral("\\t") + quote);
        QCOMPARE(desktopExecArg(QStringLiteral("\r")),
                 quote + QStringLiteral("\\r") + quote);
        QCOMPARE(desktopExecArg(QStringLiteral("'")), QStringLiteral("\"'\""));
        QCOMPARE(desktopExecArg(QStringLiteral(";&|")), QStringLiteral("\";&|\""));
        QCOMPARE(desktopExecArg(QString::fromUtf8("中文")),
                 QString::fromUtf8("中文"));

        QCOMPARE(NkrInput::desktopExec(
                     QStringList({"/opt/app dir/launcher", "--", "-tray", "-appdata"})),
                 QStringLiteral("\"/opt/app dir/launcher\" -- -tray -appdata"));

        const QString controls =
            desktopExecArg(QStringLiteral("a\nb\tc\rd\\"));
        QVERIFY(!controls.contains(QLatin1Char('\n')));
        QVERIFY(!controls.contains(QLatin1Char('\t')));
        QVERIFY(!controls.contains(QLatin1Char('\r')));
    }

#ifdef Q_OS_LINUX
    void launcherOracle() {
        const QString gio = QStandardPaths::findExecutable(QStringLiteral("gio"));
        const QString validator =
            QStandardPaths::findExecutable(QStringLiteral("desktop-file-validate"));
        QVERIFY2(!gio.isEmpty(), "Install libglib2.0-bin");
        QVERIFY2(!validator.isEmpty(), "Install desktop-file-utils");

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());

        // gio 2.72 cannot load a desktop entry whose program path contains '%',
        // even when it is escaped as '%%'. Keep '%' in arguments (covered below)
        // and leave it out of the executable path.
        const QString binDirectory =
            temporary.path() + QStringLiteral("/app space $ '");
        QVERIFY(QDir().mkpath(binDirectory));
        const QString probe = binDirectory + QStringLiteral("/argv-probe");
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), probe));
        QVERIFY(QFile::setPermissions(
            probe, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                       QFileDevice::ExeOwner));

        const QString output = temporary.path() + QStringLiteral("/received.json");
        const QString desktop = temporary.path() + QStringLiteral("/probe.desktop");
        const QStringList cases{
            QStringLiteral("plain"),
            QStringLiteral("a b"),
            QStringLiteral("back\\slash"),
            QStringLiteral("C:\\Program Files\\x\\"),
            QStringLiteral("$HOME"),
            QStringLiteral("a\"b"),
            QStringLiteral("`echo x`"),
            QStringLiteral("100%"),
            QStringLiteral("%f %u %U %i"),
            QStringLiteral("a\tb"),
            QStringLiteral("a\nb"),
            QStringLiteral("a\rb"),
            QString::fromUtf8("中文"),
            QStringLiteral("a'b"),
            QStringLiteral(";&|"),
            QString(),
            QStringLiteral("mix \\ $ \" ` % ' ;")
        };

        QStringList command{probe, QStringLiteral("--dump-argv"), output};
        command.append(cases);
        const QByteArray entry =
            (QStringLiteral("[Desktop Entry]\nType=Application\n"
                            "Name=Nekoray argv test\nTerminal=false\nExec=") +
             NkrInput::desktopExec(command) + QLatin1Char('\n'))
                .toUtf8();
        QFile desktopFile(desktop);
        QVERIFY(desktopFile.open(QIODevice::WriteOnly));
        QCOMPARE(desktopFile.write(entry), static_cast<qint64>(entry.size()));
        desktopFile.close();

        QProcess validate;
        validate.start(validator, QStringList({desktop}));
        QVERIFY(validate.waitForStarted(5000));
        QVERIFY(validate.waitForFinished(10000));
        const QByteArray validateLog =
            validate.readAllStandardOutput() + validate.readAllStandardError();
        QCOMPARE(validate.exitStatus(), QProcess::NormalExit);
        QVERIFY2(validate.exitCode() == 0, validateLog.constData());

        QProcess launch;
        launch.start(gio, QStringList({"launch", desktop}));
        QVERIFY(launch.waitForStarted(5000));
        QVERIFY(launch.waitForFinished(10000));
        const QByteArray launchLog =
            launch.readAllStandardOutput() + launch.readAllStandardError();
        QCOMPARE(launch.exitStatus(), QProcess::NormalExit);
        QVERIFY2(launch.exitCode() == 0, launchLog.constData());

        // The probe commits its output atomically.
        QTRY_VERIFY(QFileInfo::exists(output));
        QFile received(output);
        QVERIFY(received.open(QIODevice::ReadOnly));
        QJsonParseError error;
        const QJsonDocument document =
            QJsonDocument::fromJson(received.readAll(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(document.isArray());

        QStringList actual;
        for (const QJsonValue &value : document.array()) {
            QVERIFY(value.isString());
            actual.append(value.toString());
        }
        QCOMPARE(actual, cases);
    }
#endif
};

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);

    if (argc >= 3 && QByteArray(argv[1]) == QByteArray("--dump-argv")) {
        QJsonArray args;
        for (int i = 3; i < argc; ++i) {
            args.append(QString::fromUtf8(argv[i]));
        }
        const QByteArray bytes =
            QJsonDocument(args).toJson(QJsonDocument::Compact);
        QSaveFile output(QString::fromUtf8(argv[2]));
        if (!output.open(QIODevice::WriteOnly)) {
            return 2;
        }
        if (output.write(bytes) != bytes.size()) {
            return 3;
        }
        return output.commit() ? 0 : 4;
    }

    W17Test test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_w17.moc"
