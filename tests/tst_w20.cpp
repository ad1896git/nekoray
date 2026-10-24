#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdlib>
#include "sys/TimedProcess.hpp"

using NekoGui_sys::TimedProcess;
namespace {
bool Put(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(bytes) != bytes.size()) return false;
    return file.flush();
}
int RunChild(QCoreApplication &application, const QStringList &arguments) {
    const QString mode = arguments.value(2);
    if (mode == "exit") return arguments.value(3).toInt();
    if (mode == "crash") std::abort();
    if (mode == "args") {
        QJsonArray array;
        for (int i = 4; i < arguments.size(); ++i) array.append(arguments.at(i));
        return Put(arguments.at(3), QJsonDocument(array).toJson(QJsonDocument::Compact)) ? 0 : 2;
    }
    if (mode == "hold") {
        const QString ready = arguments.at(3);
        const QString late = arguments.at(4);
        if (!Put(ready, "ready")) return 2;
        QTimer::singleShot(1200, &application, [late] {
            Put(late, "unexpected continuation");
            QCoreApplication::exit(0);
        });
        return application.exec();
    }
    return 2;
}
QStringList Child(const QString &mode) { return {QStringLiteral("--w20-child"), mode}; }
}

class TimedProcessTest : public QObject {
    Q_OBJECT
private slots:
    void outcomes_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("code");
        QTest::addColumn<int>("outcome");
        QTest::addColumn<int>("exitCode");
        QTest::newRow("success") << QString("exit") << QString("0") << int(TimedProcess::Succeeded) << 0;
        QTest::newRow("failure") << QString("exit") << QString("7") << int(TimedProcess::Failed) << 7;
        QTest::newRow("denied") << QString("exit") << QString("126") << int(TimedProcess::Failed) << 126;
        QTest::newRow("crash") << QString("crash") << QString() << int(TimedProcess::Failed) << -1;
    }
    void outcomes() {
        QFETCH(QString, mode);
        QFETCH(QString, code);
        QFETCH(int, outcome);
        QFETCH(int, exitCode);
        TimedProcess job;
        QSignalSpy spy(&job, &TimedProcess::finished);
        auto arguments = Child(mode);
        if (!code.isEmpty()) arguments.append(code);
        job.start(QCoreApplication::applicationFilePath(), arguments, 5000);
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), outcome);
        QCOMPARE(spy.at(0).at(1).toInt(), exitCode);
        job.cancel();
        QTest::qWait(50);
        QCOMPARE(spy.count(), 1);
    }
    void missingProgram() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TimedProcess job;
        QSignalSpy spy(&job, &TimedProcess::finished);
        job.start(directory.filePath("does-not-exist"), {}, 5000);
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), int(TimedProcess::FailedToStart));
    }
    void argumentsAreLiteral() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString output = directory.filePath("arguments.json");
        const QStringList payload{"path with spaces", "$(not-a-command)", "\"quoted\"", "a'b", "back\\slash"};
        auto arguments = Child("args");
        arguments.append(output);
        arguments.append(payload);
        TimedProcess job;
        QSignalSpy spy(&job, &TimedProcess::finished);
        job.start(QCoreApplication::applicationFilePath(), arguments, 5000);
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), int(TimedProcess::Succeeded));
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto array = QJsonDocument::fromJson(file.readAll()).array();
        QCOMPARE(static_cast<int>(array.size()), payload.size());
        for (int i = 0; i < payload.size(); ++i) QCOMPARE(array.at(i).toString(), payload.at(i));
    }
    void timeoutKeepsEventLoopResponsive() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString ready = directory.filePath("ready");
        const QString late = directory.filePath("late");
        TimedProcess job;
        QSignalSpy spy(&job, &TimedProcess::finished);
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&ticks] { ++ticks; });
        heartbeat.start(10);
        auto arguments = Child("hold");
        arguments << ready << late;
        QElapsedTimer elapsed;
        elapsed.start();
        job.start(QCoreApplication::applicationFilePath(), arguments, 400);
        QTRY_COMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), int(TimedProcess::TimedOut));
        QVERIFY(ticks > 0);
        QVERIFY(elapsed.elapsed() < 3000);
        QTest::qWait(1400);
        QVERIFY(!QFile::exists(late));
        QCOMPARE(spy.count(), 1);
    }
    void cancellationStopsChild() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString ready = directory.filePath("ready");
        const QString late = directory.filePath("late");
        TimedProcess job;
        QSignalSpy spy(&job, &TimedProcess::finished);
        auto arguments = Child("hold");
        arguments << ready << late;
        job.start(QCoreApplication::applicationFilePath(), arguments, 5000);
        QTRY_VERIFY(QFile::exists(ready));
        job.cancel();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), int(TimedProcess::Cancelled));
        job.cancel();
        QTest::qWait(1400);
        QVERIFY(!QFile::exists(late));
        QCOMPARE(spy.count(), 1);
    }
    void immediateCancellation() {
        TimedProcess job;
        QSignalSpy spy(&job, &TimedProcess::finished);
        job.start(QCoreApplication::applicationFilePath(), {"--w20-child", "exit", "0"}, 5000);
        job.cancel();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), int(TimedProcess::Cancelled));
        QTest::qWait(200);
        QCOMPARE(spy.count(), 1);
    }
    void ownerDestructionStopsChildWithoutWaiting() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString ready = directory.filePath("ready");
        const QString late = directory.filePath("late");
        auto *owner = new QObject;
        QPointer<TimedProcess> job = new TimedProcess(owner);
        auto arguments = Child("hold");
        arguments << ready << late;
        job->start(QCoreApplication::applicationFilePath(), arguments, 5000);
        QTRY_VERIFY(QFile::exists(ready));
        QElapsedTimer elapsed;
        elapsed.start();
        delete owner;
        QVERIFY(job.isNull());
        QVERIFY(elapsed.elapsed() < 500);
        QTest::qWait(1400);
        QVERIFY(!QFile::exists(late));
    }
};
int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.value(1) == "--w20-child") return RunChild(application, arguments);
    TimedProcessTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_w20.moc"
