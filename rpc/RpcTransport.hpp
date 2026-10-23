#pragma once

#include "W04Support.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>

#include <climits>
#include <memory>

namespace NkrW04 {

struct WireReply {
    QNetworkReply::NetworkError error = QNetworkReply::UnknownNetworkError;
    QByteArray body;
    QByteArray grpcStatus;
    QByteArray grpcMessage;
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

                const auto finished = [reply, state] {
                    if (reply->property("nkrFinished").toBool()) return;
                    reply->setProperty("nkrFinished", true);
                    WireReply result;
                    result.error = reply->property("nkrTimedOut").toBool()
                                       ? QNetworkReply::TimeoutError
                                       : reply->error();
                    result.body = reply->readAll();
                    result.grpcStatus = reply->rawHeader("grpc-status");
                    result.grpcMessage = reply->rawHeader("grpc-message");
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
