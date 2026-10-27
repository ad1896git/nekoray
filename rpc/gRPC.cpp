#include "gRPC.h"

#include <utility>
#include <QStringList>

#ifndef NKR_NO_GRPC

#include "main/NekoGui.hpp"

#include "RpcTransport.hpp"

#include "GrpcFrame.hpp"

namespace QtGrpc {

class Http2GrpcChannelPrivate {
public:
    Http2GrpcChannelPrivate(const QString &url,
                            const QString &auth,
                            const QString &service)
        : urlBase_("http://" + url),
          serviceName_(service),
          auth_(auth.toLatin1()) {}

    QNetworkReply::NetworkError Call(
        const QString &methodName,
        const google::protobuf::Message &req,
        google::protobuf::Message *rsp,
        qint64 timeout_ms) {
        if (timeout_ms <= 0) {
            return QNetworkReply::ProtocolInvalidOperationError;
        }

        if (!NekoGui::dataStore->core_running) {
            return QNetworkReply::NetworkError(-1919);
        }

        std::string serialized;
        if (!req.SerializeToString(&serialized)) {
            return QNetworkReply::NetworkError(-114514);
        }
        if (!NkrGrpcFrame::payloadSizeSupported(
                static_cast<quint64>(serialized.size()))) {
            MW_show_log(
                QStringLiteral("gRPC request payload exceeds codec size limit"));
            return QNetworkReply::ProtocolInvalidOperationError;
        }
        const auto args = QByteArray::fromStdString(serialized);

        QNetworkRequest request(
            QUrl(urlBase_ + "/" + serviceName_ + "/" + methodName));
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
        request.setAttribute(QNetworkRequest::Http2DirectAttribute, true);
#endif
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QLatin1String("application/grpc"));
        request.setRawHeader("Cache-Control", "no-store");
        request.setRawHeader("grpc-accept-encoding", "identity");
        request.setRawHeader("accept-encoding", "identity,gzip");
        request.setRawHeader("te", "trailers");
        request.setRawHeader("nekoray_auth", auth_);

        const auto encoded = NkrGrpcFrame::encode(args);
        if (encoded.error != NkrGrpcFrame::Error::None) {
            MW_show_log(
                QStringLiteral("gRPC request rejected: %1")
                    .arg(QString::fromLatin1(
                        NkrGrpcFrame::errorText(encoded.error))));
            return QNetworkReply::ProtocolInvalidOperationError;
        }

        const auto result =
            transport_.send(request, encoded.bytes, timeout_ms);
        if (result.error != QNetworkReply::NoError) return result.error;

        const auto decoded =
            NkrGrpcFrame::decode(result.body, result.grpcStatus);
        if (decoded.error != NkrGrpcFrame::Error::None) {
            MW_show_log(
                QStringLiteral(
                    "gRPC response rejected: %1; "
                    "grpc-status: %2; error msg: %3")
                    .arg(QString::fromLatin1(
                        NkrGrpcFrame::errorText(decoded.error)))
                    .arg(QString::fromLatin1(result.grpcStatus))
                    .arg(QString::fromLatin1(result.grpcMessage)));
            return QNetworkReply::ProtocolUnknownError;
        }

        if (!rsp->ParseFromArray(
                decoded.bytes.constData(),
                static_cast<int>(decoded.bytes.size()))) {
            return QNetworkReply::NetworkError(-114514);
        }
        return QNetworkReply::NoError;
    }

private:
    QString urlBase_;
    QString serviceName_;
    QByteArray auth_;
    NkrW04::RpcTransport transport_;
};

} // namespace QtGrpc

namespace NekoGui_rpc {

namespace {

constexpr qint64 ExitTimeoutMs = 500;
constexpr qint64 ControlTimeoutMs = 10000;
constexpr qint64 PollTimeoutMs = 500;

QString rpcFailure(const QString &operation,
                   QNetworkReply::NetworkError status,
                   qint64 timeoutMs) {
    if (status == QNetworkReply::TimeoutError) {
        QString message =
            QStringLiteral(
                "RPC %1 timed out after %2 ms "
                "(QNetworkReply::TimeoutError).")
                .arg(operation)
                .arg(timeoutMs);
        if (operation == QStringLiteral("Start") ||
            operation == QStringLiteral("Stop")) {
            message += QStringLiteral(
                " Core state is unknown; restart the core before retrying.");
        }
        return message;
    }

    return QStringLiteral(
               "RPC %1 failed: QNetworkReply::NetworkError code: %2")
        .arg(operation)
        .arg(static_cast<int>(status));
}

} // namespace

Client::Client(std::function<void(const QString &)> onError,
               const QString &target,
               const QString &token) {
    this->make_grpc_channel = [=]() {
        return std::make_unique<QtGrpc::Http2GrpcChannelPrivate>(
            target, token, "libcore.LibcoreService");
    };
    this->default_grpc_channel = make_grpc_channel();
    this->onError = std::move(onError);
}

Client::~Client() = default;

void Client::Exit() {
    libcore::EmptyReq request;
    libcore::EmptyResp reply;
    const auto status = default_grpc_channel->Call(
        "Exit", request, &reply, ExitTimeoutMs);

    // Normal core exit can close the connection before a response arrives.
    if (status == QNetworkReply::TimeoutError) {
        onError(rpcFailure("Exit", status, ExitTimeoutMs));
    }
}

QString Client::Start(bool *rpcOK,
                      const libcore::LoadConfigReq &request) {
    libcore::ErrorResp reply;
    const auto status = default_grpc_channel->Call(
        "Start", request, &reply, ControlTimeoutMs);

    *rpcOK = status == QNetworkReply::NoError;
    if (!*rpcOK) {
        onError(rpcFailure("Start", status, ControlTimeoutMs));
        return {};
    }
    return QString::fromStdString(reply.error());
}

QString Client::Stop(bool *rpcOK) {
    libcore::EmptyReq request;
    libcore::ErrorResp reply;
    const auto status = default_grpc_channel->Call(
        "Stop", request, &reply, ControlTimeoutMs);

    *rpcOK = status == QNetworkReply::NoError;
    if (!*rpcOK) {
        onError(rpcFailure("Stop", status, ControlTimeoutMs));
        return {};
    }
    return QString::fromStdString(reply.error());
}

long long Client::QueryStats(const std::string &tag,
                            const std::string &direct) {
    libcore::QueryStatsReq request;
    request.set_tag(tag);
    request.set_direct(direct);

    libcore::QueryStatsResp reply;
    const auto status = default_grpc_channel->Call(
        "QueryStats", request, &reply, PollTimeoutMs);

    if (status == QNetworkReply::NoError) {
        return reply.traffic();
    }
    if (status == QNetworkReply::TimeoutError) {
        onError(rpcFailure("QueryStats", status, PollTimeoutMs));
    }
    return 0;
}

std::string Client::ListConnections() {
    libcore::EmptyReq request;
    libcore::ListConnectionsResp reply;
    const auto status = default_grpc_channel->Call(
        "ListConnections", request, &reply, PollTimeoutMs);

    if (status == QNetworkReply::NoError) {
        return reply.nekoray_connections_json();
    }
    if (status == QNetworkReply::TimeoutError) {
        onError(rpcFailure("ListConnections", status, PollTimeoutMs));
    }
    return {};
}

libcore::TestResp Client::Test(bool *rpcOK,
                              const libcore::TestReq &request) {
    libcore::TestResp reply;
    const qint64 timeoutMs = NkrW04::testTimeoutMs(
        request.timeout(),
        request.mode() == libcore::FullTest,
        request.full_speed(),
        request.full_speed_timeout());

    const auto status = make_grpc_channel()->Call(
        "Test", request, &reply, timeoutMs);

    *rpcOK = status == QNetworkReply::NoError;
    if (!*rpcOK) {
        onError(rpcFailure("Test", status, timeoutMs));
    }
    return reply;
}

} // namespace NekoGui_rpc

#endif
