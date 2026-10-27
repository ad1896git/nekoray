#pragma once

#include "W04Support.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QHttpHeaders>
#endif

#include <climits>
#include <memory>

namespace NkrW04 {

struct WireReply {
    QNetworkReply::NetworkError error = QNetworkReply::UnknownNetworkError;
    QByteArray body;
    QByteArray grpcStatus;
    QByteArray grpcMessage;
};

// Qt 6 can append an entire cumulative HTTP/2 header snapshot on each
// metaDataChanged notification. Read each appended batch separately instead
// of rawHeader(), which joins those repeated snapshots with commas. Qt 5
// replaces fields in place; a changed prefix then selects the full snapshot.
class ReplyMetadata {
public:
    using Headers = QList<QNetworkReply::RawHeaderPair>;

    void update(const QNetworkReply &reply) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        // rawHeaderPairs() also joins duplicate fields on these versions.
        update(reply.headers().toListOfPairs());
#else
        update(reply.rawHeaderPairs());
#endif
    }

    void update(const Headers &headers) {
        bool appended = headers.size() >= previous_.size();
        for (decltype(headers.size()) i = 0; appended && i < previous_.size(); ++i)
            appended = headers.at(i) == previous_.at(i);

        const auto first = appended ? previous_.size() : 0;
        QByteArray status;
        QByteArray message;
        bool hasStatus = false;
        bool hasMessage = false;
        for (auto i = first; i < headers.size(); ++i) {
            const auto &header = headers.at(i);
            if (header.first.toLower() == "grpc-status") {
                // Preserve duplicate fields within a batch as invalid status.
                if (hasStatus) status += ", ";
                status += header.second;
                hasStatus = true;
            } else if (header.first.toLower() == "grpc-message") {
                if (hasMessage) message += ", ";
                message += header.second;
                hasMessage = true;
            }
        }
        if (hasStatus || !appended) grpcStatus = status;
        if (hasMessage || !appended) grpcMessage = message;
        previous_ = headers;
    }

    QByteArray grpcStatus;
    QByteArray grpcMessage;

private:
    Headers previous_;
};

class RpcTransport {
public:
    RpcTransport() {
        Q_ASSERT(QCoreApplication::instance());

        thread_ = new QThread;
        manager_ = new QNetworkAccessManager;

        manager_->moveToThread(thread_);

        // The QThread QObject must not depend on a caller worker's event loop.
        thread_->moveToThread(QCoreApplication::instance()->thread());

        QObject::connect(thread_, &QThread::finished,
                         manager_, &QObject::deleteLater);
        QObject::connect(thread_, &QThread::finished,
                         thread_, &QObject::deleteLater);
        thread_->start();
    }

    RpcTransport(const RpcTransport &) = delete;
    RpcTransport &operator=(const RpcTransport &) = delete;

    ~RpcTransport() {
        // No wait(): a timed-out Test must not block again in its destructor.
        // finished connections retain responsibility for QObject cleanup.
        thread_->quit();
    }

    WireReply send(const QNetworkRequest &request, const QByteArray &body,
                   qint64 timeoutMs) {
        struct State {
            Completion<WireReply> completion;
            QElapsedTimer elapsed;
            qint64 timeout = 0;

            qint64 remaining() const {
                return qMax<qint64>(0, timeout - elapsed.elapsed());
            }
        };

        const auto state = std::make_shared<State>();
        state->timeout = timeoutMs;
        state->elapsed.start();

        auto *manager = manager_;
        const bool posted = QMetaObject::invokeMethod(
            manager,
            [manager, request, body, state] {
                if (state->completion.abandoned()) return;

                if (state->timeout > 0 && state->remaining() == 0) {
                    WireReply result;
                    result.error = QNetworkReply::TimeoutError;
                    state->completion.complete(result);
                    return;
                }

                auto *reply = manager->post(request, body);
                const auto metadata = std::make_shared<ReplyMetadata>();
                QObject::connect(reply, &QNetworkReply::metaDataChanged, reply,
                                 [reply, metadata] {
                    metadata->update(*reply);
                });

                if (state->timeout > 0) {
                    auto *timer = new QTimer(reply);
                    timer->setSingleShot(true);
                    QObject::connect(timer, &QTimer::timeout, reply,
                                     [timer, reply, state] {
                        const qint64 remaining = state->remaining();
                        if (remaining > 0) {
                            timer->start(static_cast<int>(
                                qMin<qint64>(remaining, INT_MAX)));
                        } else {
                            reply->setProperty("nkrTimedOut", true);
                            reply->abort();
                        }
                    });
                    timer->start(static_cast<int>(
                        qMax<qint64>(1, qMin<qint64>(
                            state->remaining(), INT_MAX))));
                }

                const auto finished = [reply, state, metadata] {
                    if (reply->property("nkrFinished").toBool()) return;
                    reply->setProperty("nkrFinished", true);
                    WireReply result;
                    result.error = reply->property("nkrTimedOut").toBool()
                                       ? QNetworkReply::TimeoutError
                                       : reply->error();
                    // abort() closes the QIODevice before emitting finished().
                    // Error bodies are unused; do not read a closed reply.
                    if (reply->isOpen()) result.body = reply->readAll();
                    metadata->update(*reply);
                    result.grpcStatus = metadata->grpcStatus;
                    result.grpcMessage = metadata->grpcMessage;
                    state->completion.complete(std::move(result));
                    reply->deleteLater();
                };

                QObject::connect(reply, &QNetworkReply::finished,
                                 reply, finished);

                // Covers a reply already completed before connecting.
                if (reply->isFinished()) finished();
            },
            Qt::QueuedConnection);

        if (!posted) {
            WireReply result;
            result.error = QNetworkReply::UnknownNetworkError;
            state->completion.complete(result);
        }

        WireReply result;
        const qint64 waitMs = timeoutMs > 0
                                 ? qMax<qint64>(1, state->remaining()) : 0;
        if (!state->completion.wait(waitMs, result)) {
            result.error = QNetworkReply::TimeoutError;
        }
        return result;
    }

private:
    QThread *thread_ = nullptr;
    QNetworkAccessManager *manager_ = nullptr;
};

} // namespace NkrW04
