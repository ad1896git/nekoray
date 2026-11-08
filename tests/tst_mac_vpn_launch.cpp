// PR-22: macOS Tun launcher helpers and VpnProcess launcher plumbing.
// Never uses "with administrator privileges": every osascript run here is
// an ordinary user-level "do shell script".
#include <QtTest>
#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

#include "sys/MacVpnLaunch.hpp"
#include "sys/VpnProcess.hpp"
#include "sys/LinuxVpnLaunch.hpp"
#include "sys/ExternalTempFiles.hpp"

using NekoGui_runtime::VpnProcess;
using namespace NekoGui_sys;

namespace {

struct OsaResult {
    int exitCode = -1;
    QProcess::ExitStatus status = QProcess::CrashExit;
    QByteArray out;
    QByteArray err;
};

OsaResult RunOsascript(const QString &appleScript) {
    QProcess process;
    process.start(QStringLiteral("/usr/bin/osascript"), QStringList{});
    OsaResult result;
    if (!process.waitForStarted(5000)) return result;
    process.write(appleScript.toUtf8());
    process.closeWriteChannel();
    process.waitForFinished(30000);
    result.exitCode = process.exitCode();
    result.status = process.exitStatus();
    result.out = process.readAllStandardOutput();
    result.err = process.readAllStandardError();
    return result;
}

QString ShellEcho(const QString &value) {
    QProcess process;
    process.start(QStringLiteral("/bin/sh"),
                  {QStringLiteral("-c"),
                   QStringLiteral("printf '%s' ") + ShellQuote(value)});
    process.waitForFinished(5000);
    return QString::fromUtf8(process.readAllStandardOutput());
}

const QStringList kTrickyArguments{
    QStringLiteral("plain"),
    QStringLiteral("two words"),
    QStringLiteral("it's"),
    QStringLiteral("say \"hi\""),
    QStringLiteral("back\\slash"),
    QStringLiteral("$HOME `id` $(id)"),
    QStringLiteral("/Users/张三/Library/配置.json"),
    QStringLiteral("/Users/张三/Profile '单引号' \"双引号\" $HOME `id` $(id)/配置.json"),
    QStringLiteral("profile: 東京 'a' \"b\" $ `uname`"),
};

} // namespace

class MacVpnLaunchTest final : public QObject {
    Q_OBJECT

private slots:
    void shellQuoteRoundTrip() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX shell is required");
#endif
        for (const auto &value : kTrickyArguments)
            QCOMPARE(ShellEcho(value), value);
        QCOMPARE(ShellEcho(QStringLiteral("line1\nline2")),
                 QStringLiteral("line1\nline2"));
        QCOMPARE(ShellQuote(QStringLiteral("a'b")),
                 QStringLiteral("'a'\\''b'"));
    }

    void appleScriptLiteralEscapes() {
        QCOMPARE(AppleScriptStringLiteral(QStringLiteral("a\"b\\c")),
                 QStringLiteral("\"a\\\"b\\\\c\""));
        QCOMPARE(AppleScriptStringLiteral(QStringLiteral("x\ny\tz\r")),
                 QStringLiteral("\"x\\ny\\tz\\r\""));
    }

    void bashEmbeddingRoundTrip() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX shell is required");
#endif
        QProcess process;
        process.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"),
            MacVpnShellCommand(QStringLiteral("printf '%s\\0' \"$0\" \"$@\""),
                               kTrickyArguments)});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        QByteArray expected("nekoray-vpn\0", 12);
        for (const auto &arg : kTrickyArguments) expected += arg.toUtf8() + '\0';
        QCOMPARE(process.readAllStandardOutput(), expected);
    }

    void bashIgnoresUserStartupFile() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX shell is required");
#endif
        QTemporaryDir dir;
        QFile startup(dir.filePath("startup"));
        QVERIFY(startup.open(QIODevice::WriteOnly));
        startup.write("printf injected; exit 19\n");
        startup.close();
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("BASH_ENV", startup.fileName());
        QProcess process;
        process.setProcessEnvironment(env);
        process.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"),
            MacVpnShellCommand(QStringLiteral("printf safe"), {})});
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 0);
        QCOMPARE(process.readAllStandardOutput(), QByteArray("safe"));
    }

    void prepareConfigCopiesAssetsAndDisablesWriters() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString name = QStringLiteral("geo '目录' \"q\" $ `id`.db");
        QFile asset(dir.filePath(name));
        QVERIFY(asset.open(QIODevice::WriteOnly));
        asset.write("asset data");
        asset.close();
        QJsonObject root{
            {"inbounds", QJsonArray{QJsonObject{{"type", "tun"}, {"interface_name", "utun9"}}}},
            {"outbounds", QJsonArray{QJsonObject{{"type", "socks"}}}},
            {"log", QJsonObject{{"level", "info"}, {"output", "/arbitrary/root/file"}}},
            {"experimental", QJsonObject{
                {"cache_file", QJsonObject{{"enabled", true}, {"path", "/arbitrary/cache"}}},
                {"clash_api", QJsonObject{{"external_ui", "/arbitrary/ui"},
                                         {"cache_file", "/old/cache"}}}}},
            {"route", QJsonObject{
                {"geoip", QJsonObject{{"path", name}, {"download_url", "https://invalid"}}},
                {"geosite", QJsonObject{{"path", asset.fileName()}}},
                {"rule_set", QJsonArray{QJsonObject{{"type", "local"}, {"tag", "test"},
                           {"format", "binary"}, {"path", name}}}}}}
        };
        QStringList args;
        QString error;
        const auto result = PrepareMacVpnConfig(QJsonDocument(root).toJson(), dir.path(), args, error);
        QVERIFY2(!result.isEmpty(), qPrintable(error));
        QCOMPARE(args.size(), 6);
        const auto prepared = QJsonDocument::fromJson(result).object();
        QVERIFY(!prepared.value("log").toObject().contains("output"));
        QVERIFY(!prepared.contains("experimental"));
        const auto route = prepared.value("route").toObject();
        QCOMPARE(route.value("geoip").toObject().value("path").toString(),
                 QString::fromLatin1(kMacVpnRunDir) + "/asset-0");
        QVERIFY(!route.value("geoip").toObject().contains("download_url"));
        QCOMPARE(route.value("geosite").toObject().value("path").toString(),
                 QString::fromLatin1(kMacVpnRunDir) + "/asset-1");
        QCOMPARE(route.value("rule_set").toArray().first().toObject().value("path").toString(),
                 QString::fromLatin1(kMacVpnRunDir) + "/asset-2");
        for (int i = 0; i < args.size(); i += 2) {
            QCOMPARE(QFileInfo(args[i]).absoluteFilePath(), asset.fileName());
            QCOMPARE(args[i + 1], Sha256Hex("asset data"));
        }
        // Unsupported core types and arbitrary file reads fail closed.
        root["outbounds"] = QJsonArray{QJsonObject{{"type", "tor"}, {"executable_path", "/tmp/evil"}}};
        QVERIFY(PrepareMacVpnConfig(QJsonDocument(root).toJson(), dir.path(), args, error).isEmpty());
        QVERIFY(args.isEmpty());
        root["outbounds"] = QJsonArray{QJsonObject{{"type", "direct"}, {"protect_path", "/tmp/socket"}}};
        QVERIFY(PrepareMacVpnConfig(QJsonDocument(root).toJson(), dir.path(), args, error).isEmpty());
        root["outbounds"] = QJsonArray{};
        root["route"] = QJsonObject{{"rule_set", QJsonArray{QJsonObject{{"type", "remote"}}}}};
        QVERIFY(PrepareMacVpnConfig(QJsonDocument(root).toJson(), dir.path(), args, error).isEmpty());
    }

    void implicitGeoAssetsAndMissingFiles() {
        QTemporaryDir dir;
        QJsonObject root{{"inbounds", QJsonArray{QJsonObject{{"type", "tun"}}}},
            {"route", QJsonObject{{"rules", QJsonArray{QJsonObject{{"geoip", "cn"}}}}}},
            {"dns", QJsonObject{{"rules", QJsonArray{QJsonObject{{"geosite", "cn"}}}}}}};
        QStringList args;
        QString error;
        QVERIFY(PrepareMacVpnConfig(QJsonDocument(root).toJson(), dir.path(), args, error).isEmpty());
        for (const auto &name : {"geoip.db", "geosite.db"}) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            // Empty geo files trigger sing-box's automatic downloader.
            QVERIFY(PrepareMacVpnConfig(QJsonDocument(root).toJson(), dir.path(), args, error).isEmpty());
            f.write("db");
        }
        QVERIFY2(!PrepareMacVpnConfig(QJsonDocument(root).toJson(), dir.path(), args, error).isEmpty(),
                 qPrintable(error));
        QCOMPARE(args.size(), 4);
    }

    void defaultTemplateIsSafe() {
        QFile f(QStringLiteral(NKR_VPN_TEMPLATE_PATH));
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto text = QString::fromUtf8(f.readAll());
        text.replace("//%IPV6_ADDRESS%", "").replace("//%PROCESS_NAME_RULE%", "")
            .replace("//%CIDR_RULE%", "").replace("//%SOCKS_USER_PASS%", "")
            .replace("%MTU%", "9000").replace("%STACK%", "gvisor")
            .replace("%TUN_NAME%", "utun9").replace("%STRICT_ROUTE%", "true")
            .replace("%FINAL_OUT%", "neko-socks").replace("%DNS_ADDRESS%", "local")
            .replace("%FAKE_DNS_INBOUND%", "tun-in").replace("%PORT%", "2080");
        QStringList args;
        QString error;
        const auto result = PrepareMacVpnConfig(text.toUtf8(), QDir::currentPath(), args, error);
        QVERIFY2(!result.isEmpty(), qPrintable(error));
        QCOMPARE(NekoGui_runtime::TunInterfaceName(result), QStringLiteral("utun9"));
        QVERIFY(args.isEmpty());
    }

    void launchConfigsAreUniqueAcrossFastSwitch() {
        QTemporaryDir dir;
        ExternalTempFiles oldRun(dir.path()), newRun(dir.path());
        QString error;
        const auto oldPath = WriteLinuxVpnConfig(oldRun, "old", error);
        const auto newPath = WriteLinuxVpnConfig(newRun, "new", error);
        QVERIFY(!oldPath.isEmpty());
        QVERIFY(!newPath.isEmpty());
        QVERIFY(oldPath != newPath);
        oldRun.clear();
        oldRun.clear(); // late callback cannot delete the new run's stop file
        QVERIFY(!QFile::exists(oldPath));
        QVERIFY(QFile::exists(newPath));
    }

    void adminClauseOnlyWhenRequested() {
        const auto user = MacVpnAppleScript(QStringLiteral("exit 0"), {}, false);
        const auto admin = MacVpnAppleScript(QStringLiteral("exit 0"), {}, true);
        QVERIFY(user.startsWith(QStringLiteral("do shell script \"exec /bin/bash -p -c ")));
        QVERIFY(!user.contains(QStringLiteral("administrator")));
        QVERIFY(admin.trimmed().endsWith(
            QStringLiteral("with administrator privileges")));
    }

    void osascriptRoundTripPreservesArguments() {
#ifndef Q_OS_MACOS
        QSKIP("osascript is macOS only");
#endif
        const auto script = QStringLiteral(
            "printf '[%s]\\n' \"$0\" \"$@\" >&2; exit 75");
        const auto result = RunOsascript(
            MacVpnAppleScript(script, kTrickyArguments, false));
        QCOMPARE(result.status, QProcess::NormalExit);
        QCOMPARE(result.exitCode, 1);
        QString text = QString::fromUtf8(result.err);
        text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        QVERIFY2(text.contains(QStringLiteral("[nekoray-vpn]")), qPrintable(text));
        for (const auto &argument : kTrickyArguments) {
            QVERIFY2(text.contains(QLatin1Char('[') + argument + QLatin1Char(']')),
                     qPrintable(argument + QStringLiteral(" in ") + text));
        }
        const auto exit = ClassifyMacVpnExit(result.exitCode, result.status,
                                             result.err, false);
        QCOMPARE(exit.kind, MacVpnExit::Failed);
        QCOMPARE(exit.code, 75);
        QVERIFY(exit.message.contains(QStringLiteral("Another Tun instance")));
    }

    void embeddedScriptRunsThroughOsascript() {
#ifndef Q_OS_MACOS
        QSKIP("osascript is macOS only");
#endif
        // The real script, embedded exactly as in production, reaches its
        // own privilege check (as a normal user) and reports code 77.
        QString error;
        const auto script = ReadMacVpnScript(error);
        QVERIFY2(!script.isEmpty(), qPrintable(error));
        const auto result = RunOsascript(MacVpnAppleScript(
            script, {QStringLiteral("cleanup"), QStringLiteral("501")}, false));
        const auto exit = ClassifyMacVpnExit(result.exitCode, result.status,
                                             result.err, false);
        QCOMPARE(exit.kind, MacVpnExit::Failed);
        QCOMPARE(exit.code, 77);
        QVERIFY2(exit.message.contains(QStringLiteral("root is required")),
                 qPrintable(exit.message));
    }

    void embeddedScriptMatchesSource() {
        QString error;
        const auto script = ReadMacVpnScript(error);
        QVERIFY2(!script.isEmpty(), qPrintable(error));
        QFile source(QStringLiteral(NKR_MAC_VPN_SCRIPT));
        QVERIFY(source.open(QIODevice::ReadOnly));
        QCOMPARE(script, QString::fromUtf8(source.readAll()));
        QVERIFY(script.contains(QStringLiteral("start-locked")));
        QVERIFY(!script.contains(QStringLiteral("pkill")));
        QVERIFY(!script.contains(QStringLiteral("killall")));
    }

    void classify_data() {
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("stderrText");
        QTest::addColumn<bool>("stopRequested");
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("code");
        QTest::addColumn<QString>("needle");

        const auto normal = int(QProcess::NormalExit);
        const auto crash = int(QProcess::CrashExit);
        QTest::newRow("clean") << 0 << normal << QByteArray() << false
                               << int(MacVpnExit::Stopped) << 0 << QString();
        QTest::newRow("cancel")
            << 1 << normal
            << QByteArray("-:0:120: execution error: User canceled. (-128)\n")
            << false << int(MacVpnExit::Cancelled) << -128 << QString();
        QTest::newRow("auth")
            << 1 << normal
            << QByteArray("-:0:1: execution error: The administrator user name or password was incorrect. (-60007)\n")
            << false << int(MacVpnExit::AuthFailed) << -60007
            << QStringLiteral("authorization failed");
        QTest::newRow("core")
            << 1 << normal
            << QByteArray("x: execution error: nekoray-vpn: core exited with status 1\rFATAL bad config (70)\n")
            << false << int(MacVpnExit::Failed) << 70
            << QStringLiteral("FATAL bad config");
        QTest::newRow("hash")
            << 1 << normal << QByteArray("x: execution error: changed (76)")
            << false << int(MacVpnExit::Failed) << 76
            << QStringLiteral("changed during");
        QTest::newRow("killed-while-stopping")
            << 9 << crash << QByteArray() << true
            << int(MacVpnExit::Stopped) << 0 << QString();
        QTest::newRow("cancelled-before-copy-while-stopping")
            << 1 << normal << QByteArray("x: execution error: missing (66)")
            << true << int(MacVpnExit::Stopped) << 66 << QString();
        QTest::newRow("cleanup-failed-while-stopping")
            << 1 << normal << QByteArray("x: execution error: core did not exit (74)")
            << true << int(MacVpnExit::Failed) << 74
            << QStringLiteral("core did not exit");
        QTest::newRow("unparsed")
            << 1 << crash << QByteArray("garbage") << false
            << int(MacVpnExit::Failed) << 1 << QStringLiteral("garbage");
    }

    void classify() {
        QFETCH(int, exitCode);
        QFETCH(int, status);
        QFETCH(QByteArray, stderrText);
        QFETCH(bool, stopRequested);
        QFETCH(int, kind);
        QFETCH(int, code);
        QFETCH(QString, needle);
        const auto exit = ClassifyMacVpnExit(
            exitCode, QProcess::ExitStatus(status), stderrText, stopRequested);
        QCOMPARE(int(exit.kind), kind);
        QCOMPARE(exit.code, code);
        if (needle.isEmpty())
            QVERIFY(exit.message.isEmpty());
        else
            QVERIFY2(exit.message.contains(needle), qPrintable(exit.message));
    }

    void interfaceNames() {
        QVERIFY(IsMacTunInterfaceName(QStringLiteral("utun9")));
        QVERIFY(IsMacTunInterfaceName(QStringLiteral("utun123")));
        QVERIFY(!IsMacTunInterfaceName(QStringLiteral("utun")));
        QVERIFY(!IsMacTunInterfaceName(QStringLiteral("utun1234")));
        QVERIFY(!IsMacTunInterfaceName(QStringLiteral("neko-tun")));
        QVERIFY(!IsMacTunInterfaceName(QStringLiteral("utun9;id")));
        QVERIFY(!IsMacTunInterfaceName(QStringLiteral("en0")));
    }

    void sha256() {
        QCOMPARE(Sha256Hex("abc"),
                 QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile file(dir.filePath("f"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("abc");
        file.close();
        QString error;
        QCOMPARE(Sha256File(file.fileName(), error), Sha256Hex("abc"));
        QVERIFY(Sha256File(dir.filePath("missing"), error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void internalTunRule() {
        QVERIFY(!MacUsesInternalTun(true, false));
        QVERIFY(MacUsesInternalTun(true, true));
        QVERIFY(!MacUsesInternalTun(false, true));
        QVERIFY(!MacUsesInternalTun(false, false));
    }

    // VpnProcess launcher plumbing used by the macOS path.
    void vpnProcessDeliversInputAndCapturesError() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX shell is required");
#endif
        VpnProcess process([] { return false; });
        QSignalSpy ended(&process, &VpnProcess::ended);
        process.setCaptureStandardError(true);
        process.setStandardInput("hello world\n");
        QVERIFY(process.start(QStringLiteral("/bin/sh"),
                              {QStringLiteral("-c"),
                               QStringLiteral("read -r line; printf '%s' \"$line\" >&2; exit 3")}));
        QTRY_COMPARE(ended.count(), 1);
        QCOMPARE(process.capturedStandardError(), QByteArray("hello world"));
        QCOMPARE(process.exitCode(), 3);
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    }

    void vpnProcessCapsCapturedError() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX shell is required");
#endif
        // Keep the 200 KB payload out of the test runner's log stream. The
        // captured-error size/suffix assertions below still exercise it all.
        auto *category = QLoggingCategory::defaultCategory();
        struct RestoreDebug {
            QLoggingCategory *category;
            bool enabled;
            ~RestoreDebug() { category->setEnabled(QtDebugMsg, enabled); }
        } restore{category, category->isDebugEnabled()};
        category->setEnabled(QtDebugMsg, false);
        VpnProcess process([] { return false; });
        QSignalSpy ended(&process, &VpnProcess::ended);
        process.setCaptureStandardError(true);
        QVERIFY(process.start(QStringLiteral("/bin/sh"),
                              {QStringLiteral("-c"),
                               QStringLiteral("head -c 200000 /dev/zero | tr '\\000' x >&2; printf '(75)' >&2")}));
        QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 10000);
        QCOMPARE(process.capturedStandardError().size(),
                 VpnProcess::kMaxCapturedError);
        QVERIFY(process.capturedStandardError().endsWith("(75)"));
    }

    void vpnProcessAbortEndsSilently() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX shell is required");
#endif
        VpnProcess process([] { return false; });
        QSignalSpy ended(&process, &VpnProcess::ended);
        QVERIFY(process.start(QStringLiteral("/bin/sleep"), {QStringLiteral("30")}));
        process.abort();
        QVERIFY(process.stopRequested());
        QTRY_COMPARE(ended.count(), 1);
        QVERIFY(ended.at(0).at(0).toString().isEmpty());
        QCOMPARE(process.exitStatus(), QProcess::CrashExit);
        QVERIFY(!process.isReady());
    }
};

QTEST_GUILESS_MAIN(MacVpnLaunchTest)
#include "tst_mac_vpn_launch.moc"
