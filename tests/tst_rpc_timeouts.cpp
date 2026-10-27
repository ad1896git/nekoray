#include <QtTest>

#include "main/NekoGui.hpp"
#include "rpc/gRPC.h"
#include "rpc/GrpcFrame.hpp"
#include "rpc/RpcTransport.hpp"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

QByteArray frame(unsigned char type,
                 unsigned char flags,
                 quint32 stream,
                 const QByteArray &payload = {}) {
    QByteArray result(9, '\0');
    const quint32 size = static_cast<quint32>(payload.size());
    result[0] = static_cast<char>((size >> 16) & 0xff);
    result[1] = static_cast<char>((size >> 8) & 0xff);
    result[2] = static_cast<char>(size & 0xff);
    result[3] = static_cast<char>(type);
    result[4] = static_cast<char>(flags);
    result[5] = static_cast<char>((stream >> 24) & 0x7f);
    result[6] = static_cast<char>((stream >> 16) & 0xff);
    result[7] = static_cast<char>((stream >> 8) & 0xff);
    result[8] = static_cast<char>(stream & 0xff);
    result += payload;
    return result;
}

quint32 byteAt(const QByteArray &bytes, int index) {
    return static_cast<unsigned char>(bytes.at(index));
}

class Peer {
public:
    QTcpServer server;
    bool respond = false;
    bool closeRequests = false;
    int rejectedRequests = 0;
    int requests = 0;
    int responses = 0;

    Peer() {
        QObject::connect(
            &server, &QTcpServer::newConnection, &server, [this] {
                while (server.hasPendingConnections()) {
                    auto *socket = server.nextPendingConnection();
                    const auto state = std::make_shared<Connection>();

                    QObject::connect(
                        socket, &QTcpSocket::readyRead, socket,
                        [this, socket, state] {
                            state->input += socket->readAll();

                            if (!state->prefaceRead) {
                                const QByteArray preface(
                                    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
                                if (state->input.size() < preface.size()) {
                                    return;
                                }
                                if (!state->input.startsWith(preface)) {
                                    qFatal("RPC test expected HTTP/2 prior knowledge");
                                }
                                state->input.remove(0, preface.size());
                                state->prefaceRead = true;
                                socket->write(frame(4, 0, 0));
                            }

                            while (state->input.size() >= 9) {
                                const quint32 length =
                                    (byteAt(state->input, 0) << 16) |
                                    (byteAt(state->input, 1) << 8) |
                                    byteAt(state->input, 2);
                                if (state->input.size() <
                                    static_cast<qint64>(9 + length)) {
                                    return;
                                }

                                const auto type = byteAt(state->input, 3);
                                const auto flags = byteAt(state->input, 4);
                                const quint32 stream =
                                    ((byteAt(state->input, 5) & 0x7f) << 24) |
                                    (byteAt(state->input, 6) << 16) |
                                    (byteAt(state->input, 7) << 8) |
                                    byteAt(state->input, 8);
                                const QByteArray payload =
                                    state->input.mid(9, length);
                                state->input.remove(0, 9 + length);

                                if (type == 4 && !(flags & 1)) {
                                    socket->write(frame(4, 1, 0));
                                } else if (type == 6 && !(flags & 1)) {
                                    socket->write(frame(6, 1, 0, payload));
                                }

                                if ((type == 0 || type == 1) &&
                                    (flags & 1) && stream != 0) {
                                    ++requests;
                                    if (closeRequests) {
                                        // Signal an explicit HTTP/2 stream failure before
                                        // closing TCP. A bare FIN exercises Qt's EOF path,
                                        // rather than an unambiguous failed RPC stream.
                                        const QByteArray internalError("\x00\x00\x00\x02", 4);
                                        socket->write(frame(3, 0, stream, internalError));
                                        ++rejectedRequests;
                                        socket->disconnectFromHost();
                                        return;
                                    }
                                    if (respond) {
                                        // HPACK static-table :status = 200.
                                        socket->write(frame(
                                            1, 4, stream,
                                            QByteArray(1, char(0x88))));

                                        // Uncompressed empty protobuf message.
                                        socket->write(frame(
                                            0, 0, stream,
                                            QByteArray(5, '\0')));

                                        // Literal grpc-status: 0 trailer.
                                        const QByteArray trailers(
                                            "\x00\x0b"
                                            "grpc-status"
                                            "\x01"
                                            "0",
                                            15);
                                        socket->write(frame(
                                            1, 5, stream, trailers));
                                        ++responses;
                                    }
                                }
                            }
                        });
                }
            });
    }

private:
    struct Connection {
        QByteArray input;
        bool prefaceRead = false;
    };
};

// Error callbacks run on RPC caller threads. Snapshot after joining, keeping
// all reads and writes protected even if the callback dispatch changes later.
class Messages {
public:
    void append(const QString &message) {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.append(message);
    }

    QStringList snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.clear();
    }

private:
    mutable std::mutex mutex_;
    QStringList messages_;
};

struct Case {
    const char *operation;
    qint64 budget;
    bool hasRpcOK;
};

const std::array<Case, 7> Cases{{
    {"Exit", 500, false},
    {"Start", 10000, true},
    {"Stop", 10000, true},
    {"QueryStats", 500, false},
    {"ListConnections", 500, false},
    {"Test", 15000, true},
    {"Test", 45000, true},
}};

struct Result {
    bool ok = true;
    bool empty = true;
    qint64 elapsed = 0;
};

Result invoke(NekoGui_rpc::Client &client, int index) {
    Result result;
    QElapsedTimer elapsed;
    elapsed.start();

    switch (index) {
    case 0:
        client.Exit();
        break;
    case 1: {
        libcore::LoadConfigReq request;
        result.empty = client.Start(&result.ok, request).isEmpty();
        break;
    }
    case 2:
        result.empty = client.Stop(&result.ok).isEmpty();
        break;
    case 3:
        result.empty =
            client.QueryStats("proxy", "uplink") == 0;
        break;
    case 4:
        result.empty = client.ListConnections().empty();
        break;
    case 5:
    case 6: {
        libcore::TestReq request;
        request.set_timeout(10000);
        request.set_mode(
            index == 5 ? libcore::UrlTest : libcore::FullTest);
        request.set_full_speed(index == 6);
        request.set_full_speed_timeout(30);
        const auto reply = client.Test(&result.ok, request);
        result.empty =
            reply.error().empty() &&
            reply.full_report().empty() &&
            reply.ms() == 0;
        break;
    }
    default:
        qFatal("Invalid RPC test case");
    }

    result.elapsed = elapsed.elapsed();
    return result;
}

std::vector<Result> runCalls(NekoGui_rpc::Client &client,
                             const std::vector<int> &indices,
                             qint64 watchdogMs) {
    if (indices.size() > Cases.size()) {
        qFatal("Too many RPC test cases");
    }

    std::vector<Result> results(indices.size());
    std::array<std::atomic_bool, 7> done;
    for (auto &value : done) value.store(false);

    std::vector<std::thread> callers;
    callers.reserve(indices.size());

    for (size_t i = 0; i < indices.size(); ++i) {
        callers.emplace_back([&, i] {
            results[i] = invoke(client, indices[i]);
            done[i].store(true);
        });
    }

    QElapsedTimer elapsed;
    elapsed.start();

    for (;;) {
        bool finished = true;
        for (size_t i = 0; i < indices.size(); ++i) {
            if (!done[i].load()) finished = false;
        }
        if (finished) break;

        if (elapsed.elapsed() > watchdogMs) {
            // Fail the process instead of hanging in a thread destructor/join.
            qFatal("RPC deadline watchdog expired");
        }
        QTest::qWait(5);
    }

    for (auto &caller : callers) caller.join();
    return results;
}

QString expectedTimeout(const Case &testCase) {
    QString text =
        QStringLiteral(
            "RPC %1 timed out after %2 ms "
            "(QNetworkReply::TimeoutError).")
            .arg(QString::fromLatin1(testCase.operation))
            .arg(testCase.budget);
    if (QString::fromLatin1(testCase.operation) == QStringLiteral("Start") ||
        QString::fromLatin1(testCase.operation) == QStringLiteral("Stop")) {
        text += QStringLiteral(
            " Core state is unknown; restart the core before retrying.");
    }
    return text;
}

} // namespace

class RpcTimeoutTest : public QObject {
    Q_OBJECT

private:
    bool previousRunning_ = false;
    std::function<void(QString)> previousLog_;

private slots:
    void init() {
        previousRunning_ = NekoGui::dataStore->core_running;
        previousLog_ = MW_show_log;
        NekoGui::dataStore->core_running = true;
        MW_show_log = [](QString) {};
    }

    void cleanup() {
        NekoGui::dataStore->core_running = previousRunning_;
        MW_show_log = previousLog_;
    }

    void metadataSnapshots() {
        using Headers = NkrW04::ReplyMetadata::Headers;
        using Error = NkrGrpcFrame::Error;
        const Headers initial{{"content-type", "application/grpc"}};
        Headers complete = initial;
        complete.append(QNetworkReply::RawHeaderPair{"grpc-status", "0"});
        complete.append(QNetworkReply::RawHeaderPair{"grpc-message", "OK"});

        NkrW04::ReplyMetadata metadata;
        metadata.update(initial);
        QVERIFY(metadata.grpcStatus.isEmpty());
        metadata.update(complete); // Qt 5 replaces the snapshot.
        QCOMPARE(metadata.grpcStatus, QByteArray("0"));
        QCOMPARE(metadata.grpcMessage, QByteArray("OK"));

        // Qt 6 appends the whole snapshot, including earlier fields. When
        // HEADERS and trailers arrive together, both notifications can already
        // contain grpc-status. The final rawHeader() would then be "0, 0".
        NkrW04::ReplyMetadata coalesced;
        coalesced.update(complete);
        coalesced.update(complete + complete);
        coalesced.update(complete + complete); // finished() sees no new batch.
        QCOMPARE(coalesced.grpcStatus, QByteArray("0"));
        QCOMPARE(coalesced.grpcMessage, QByteArray("OK"));
        const QByteArray wire(5, '\0');
        QVERIFY(NkrGrpcFrame::decode(wire, coalesced.grpcStatus).error ==
                Error::None);

        NkrW04::ReplyMetadata separated;
        separated.update(initial);
        separated.update(initial + complete);
        QCOMPARE(separated.grpcStatus, QByteArray("0"));

        // A real duplicate field within one snapshot must still fail.
        Headers duplicate = complete;
        duplicate.append(QNetworkReply::RawHeaderPair{"grpc-status", "0"});
        NkrW04::ReplyMetadata invalid;
        invalid.update(duplicate);
        QVERIFY(NkrGrpcFrame::decode(wire, invalid.grpcStatus).error ==
                Error::InvalidStatus);
        coalesced.update(complete + complete + duplicate);
        QVERIFY(NkrGrpcFrame::decode(wire, coalesced.grpcStatus).error ==
                Error::InvalidStatus);

        Headers failed = initial;
        failed.append(QNetworkReply::RawHeaderPair{"grpc-status", "7"});
        metadata.update(failed); // An in-place update must not retain success.
        QVERIFY(NkrGrpcFrame::decode({}, metadata.grpcStatus).error ==
                Error::NonzeroStatus);
        metadata.update(initial);
        QVERIFY(NkrGrpcFrame::decode(wire, metadata.grpcStatus).error ==
                Error::MissingStatus);

        Headers malformed = initial;
        malformed.append(QNetworkReply::RawHeaderPair{"grpc-status", "0,0"});
        metadata.update(malformed);
        QVERIFY(NkrGrpcFrame::decode(wire, metadata.grpcStatus).error ==
                Error::InvalidStatus);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        // Exercise the actual public reply API: rawHeaderPairs() merges
        // duplicate values too, so extraction must use QHttpHeaders instead.
        class Reply : public QNetworkReply {
        public:
            using QNetworkReply::setHeaders;
            void abort() override {}
            qint64 readData(char *, qint64) override { return -1; }
        } reply;
        QHttpHeaders headers;
        headers.append("content-type", "application/grpc");
        headers.append("grpc-status", "0");
        reply.setHeaders(headers);
        NkrW04::ReplyMetadata actual;
        actual.update(reply);
        headers.append("content-type", "application/grpc");
        headers.append("grpc-status", "0");
        reply.setHeaders(headers);
        QCOMPARE(reply.rawHeader("grpc-status"), QByteArray("0, 0"));
        actual.update(reply);
        QCOMPARE(actual.grpcStatus, QByteArray("0"));
        QVERIFY(NkrGrpcFrame::decode(wire, actual.grpcStatus).error == Error::None);

        NkrW04::ReplyMetadata actualDuplicate;
        actualDuplicate.update(reply); // Both fields in the first snapshot.
        QVERIFY(NkrGrpcFrame::decode(wire, actualDuplicate.grpcStatus).error ==
                Error::InvalidStatus);
#endif
    }

    void successfulCallsWithTrailers() {
        Peer peer;
        peer.respond = true;
        QVERIFY2(peer.server.listen(QHostAddress::LocalHost),
                 qPrintable(peer.server.errorString()));

        Messages messages;
        Messages logs;
        MW_show_log = [&](const QString &message) { logs.append(message); };
        NekoGui_rpc::Client client(
            [&](const QString &message) { messages.append(message); },
            QStringLiteral("127.0.0.1:%1").arg(peer.server.serverPort()),
            QStringLiteral("test-token"));
        const auto results = runCalls(client, {0, 1, 2, 3, 4, 5, 6}, 3000);
        QCOMPARE(peer.responses, int(Cases.size()));
        QVERIFY2(messages.snapshot().isEmpty(),
                 qPrintable((messages.snapshot() + logs.snapshot()).join('\n')));
        for (size_t i = 0; i < Cases.size(); ++i) {
            QVERIFY2(results[i].ok, Cases[i].operation);
            QVERIFY2(results[i].empty, Cases[i].operation);
        }
    }

    void stalledCallsAndSubsequentSuccess() {
        Peer peer;
        QVERIFY2(peer.server.listen(QHostAddress::LocalHost),
                 qPrintable(peer.server.errorString()));

        Messages messages;
        Messages logs;
        MW_show_log = [&](const QString &message) { logs.append(message); };
        NekoGui_rpc::Client client(
            [&](const QString &message) {
                messages.append(message);
            },
            QStringLiteral("127.0.0.1:%1")
                .arg(peer.server.serverPort()),
            QStringLiteral("test-token"));

        const std::vector<int> indices{0, 1, 2, 3, 4, 5, 6};
        const auto failed = runCalls(client, indices, 305000);

        QVERIFY(peer.requests > 0);
        const auto failureMessages = messages.snapshot();
        QCOMPARE(failureMessages.size(), int(Cases.size()));

        for (size_t i = 0; i < Cases.size(); ++i) {
            const auto &testCase = Cases[i];
            QVERIFY2(failed[i].elapsed >= testCase.budget - 100,
                     testCase.operation);
            QVERIFY2(failed[i].elapsed <= testCase.budget + 3000,
                     testCase.operation);
            QVERIFY2(failed[i].empty, testCase.operation);
            if (testCase.hasRpcOK) {
                QVERIFY2(!failed[i].ok, testCase.operation);
            }
            QVERIFY2(failureMessages.contains(expectedTimeout(testCase)),
                     qPrintable(expectedTimeout(testCase)));
        }

        // The same client remains usable after all timed-out calls return.
        messages.clear();
        peer.respond = true;
        const int before = peer.responses;
        const auto succeeded = runCalls(client, indices, 3000);

        QCOMPARE(peer.responses - before, int(Cases.size()));
        QVERIFY2(messages.snapshot().isEmpty(),
                 qPrintable((messages.snapshot() + logs.snapshot()).join('\n')));
        for (size_t i = 0; i < Cases.size(); ++i) {
            QVERIFY2(succeeded[i].ok, Cases[i].operation);
            QVERIFY2(succeeded[i].empty, Cases[i].operation);
            QVERIFY2(succeeded[i].elapsed < 3000, Cases[i].operation);
        }
    }

    void immediateTransportFailureIsNotReportedAsTimeout() {
        Peer peer;
        peer.closeRequests = true;
        QVERIFY2(peer.server.listen(QHostAddress::LocalHost),
                 qPrintable(peer.server.errorString()));

        Messages messages;
        NekoGui_rpc::Client client(
            [&](const QString &message) {
                messages.append(message);
            },
            QStringLiteral("127.0.0.1:%1")
                .arg(peer.server.serverPort()),
            QStringLiteral("test-token"));

        const auto results = runCalls(client, {1}, 3000);
        QCOMPARE(peer.requests, 1);
        QCOMPARE(peer.rejectedRequests, 1);
        QVERIFY(!results[0].ok);
        QVERIFY(results[0].empty);
        QVERIFY(results[0].elapsed < 3000);
        const auto failureMessages = messages.snapshot();
        QCOMPARE(failureMessages.size(), 1);
        QVERIFY(failureMessages.first().contains(
            QStringLiteral("RPC Start failed:")));
        QVERIFY(!failureMessages.first().contains(
            QStringLiteral("timed out")));
    }

    void coreNotRunningReturnsImmediately() {
        NekoGui::dataStore->core_running = false;

        Messages messages;
        NekoGui_rpc::Client client(
            [&](const QString &message) {
                messages.append(message);
            },
            QStringLiteral("127.0.0.1:1"),
            QStringLiteral("test-token"));

        const auto results = runCalls(client, {1}, 1000);
        QVERIFY(!results[0].ok);
        QVERIFY(results[0].empty);
        QVERIFY(results[0].elapsed < 1000);
        const auto failureMessages = messages.snapshot();
        QCOMPARE(failureMessages.size(), 1);
        QVERIFY(failureMessages.first().contains(
            QStringLiteral("NetworkError code: -1919")));
    }
};

QTEST_MAIN(RpcTimeoutTest)
#include "tst_rpc_timeouts.moc"
