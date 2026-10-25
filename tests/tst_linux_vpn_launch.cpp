#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>

#include "sys/ExternalTempFiles.hpp"
#include "sys/LinuxVpnLaunch.hpp"

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

class LinuxVpnLaunchTest final : public QObject {
    Q_OBJECT

private slots:
    void embeddedResourceIgnoresDiskOverride() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir("vpn"));

        QFile overrideFile(directory.path() + "/vpn/vpn-run-root.sh");
        QVERIFY(overrideFile.open(QIODevice::WriteOnly));
        QCOMPARE(overrideFile.write("DISK_OVERRIDE"), qint64(13));
        overrideFile.close();

        const QString previous = QDir::currentPath();
        QVERIFY(QDir::setCurrent(directory.path()));
        QString error;
        const QString script = NekoGui_sys::ReadLinuxVpnScript(error);
        const bool restored = QDir::setCurrent(previous);

        QVERIFY(restored);
        QVERIFY2(!script.isEmpty(), qPrintable(error));
        QVERIFY(error.isEmpty());
        QVERIFY(script.startsWith("#!/bin/bash\n"));
        QVERIFY(!script.contains("DISK_OVERRIDE"));

        QFile resource(":/neko/vpn/vpn-run-root.sh");
        QVERIFY(resource.open(QIODevice::ReadOnly));
        QCOMPARE(script.toUtf8(), resource.readAll());
    }

    void argumentsRemainSeparate() {
        const QString script = "printf '%s\\0' \"$@\"";
        const QStringList paths{
            "/tmp/core with 'quotes' and `ticks`",
            "/tmp/$(touch must-not-exist)/config \"quoted\".json",
            "/tmp/work with spaces\nand newline"
        };
        const QStringList arguments =
            NekoGui_sys::LinuxVpnArguments(script, paths);

        QCOMPARE(arguments.size(), 7);
        QCOMPARE(arguments.at(0), QString("/bin/bash"));
        QCOMPARE(arguments.at(1), QString("-c"));
        QCOMPARE(arguments.at(2), script);
        QCOMPARE(arguments.at(3), QString("nekoray-vpn"));
        QCOMPARE(arguments.mid(4), paths);

#ifdef Q_OS_UNIX
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QProcess process;
        process.setWorkingDirectory(directory.path());
        process.start(arguments.first(), arguments.mid(1));
        QVERIFY(process.waitForStarted(5000));
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);

        QByteArray expected;
        for (const QString &path : paths) {
            expected.append(path.toUtf8());
            expected.append('\0');
        }
        QCOMPARE(process.readAllStandardOutput(), expected);
        QVERIFY(!QFileInfo::exists(directory.path() + "/must-not-exist"));
#endif
    }

    void controlArguments() {
        const QString script = "constant script";
        QCOMPARE(
            NekoGui_sys::LinuxVpnArguments(script, {"--terminate"}),
            (QStringList{
                "/bin/bash", "-c", script, "nekoray-vpn", "--terminate"}));
        QCOMPARE(
            NekoGui_sys::LinuxVpnArguments(script, {"--stop"}),
            (QStringList{
                "/bin/bash", "-c", script, "nekoray-vpn", "--stop"}));
    }

    void privateConfigurationOwnership() {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        QString first;
        QString second;
        {
            NekoGui_sys::ExternalTempFiles files(root.path());
            QString error;
            first = NekoGui_sys::WriteLinuxVpnConfig(
                files, QByteArray("{\"secret\":\"one\"}"), error);
            QVERIFY2(!first.isEmpty(), qPrintable(error));
            second = NekoGui_sys::WriteLinuxVpnConfig(
                files, QByteArray("{\"secret\":\"two\"}"), error);
            QVERIFY2(!second.isEmpty(), qPrintable(error));
            QVERIFY(first != second);
            QVERIFY(QFileInfo(first).isAbsolute());

            QFile input(first);
            QVERIFY(input.open(QIODevice::ReadOnly));
            QCOMPARE(input.readAll(), QByteArray("{\"secret\":\"one\"}"));
            input.close();

#ifdef Q_OS_UNIX
            struct stat fileStat {};
            struct stat directoryStat {};
            const QByteArray fileName = QFile::encodeName(first);
            const QByteArray directoryName =
                QFile::encodeName(QFileInfo(first).absolutePath());
            QCOMPARE(::stat(fileName.constData(), &fileStat), 0);
            QCOMPARE(::stat(directoryName.constData(), &directoryStat), 0);
            QCOMPARE(int(fileStat.st_mode & 07777), 0600);
            QCOMPARE(int(directoryStat.st_mode & 07777), 0700);
            QVERIFY(fileStat.st_uid == ::geteuid());
            QVERIFY(directoryStat.st_uid == ::geteuid());
#endif
        }

        QVERIFY(!QFileInfo::exists(first));
        QVERIFY(!QFileInfo::exists(second));
    }

    void configurationFailureIsReported() {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        QFile blocker(root.path() + "/temp");
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();

        NekoGui_sys::ExternalTempFiles files(root.path());
        QString error;
        const QString path = NekoGui_sys::WriteLinuxVpnConfig(
            files, QByteArray("secret"), error);
        QVERIFY(path.isEmpty());
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(LinuxVpnLaunchTest)
#include "tst_linux_vpn_launch.moc"
