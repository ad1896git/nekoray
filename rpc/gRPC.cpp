#include "gRPC.h"

#include <utility>
#include <QStringList>

#ifndef NKR_NO_GRPC

#include "main/NekoGui.hpp"

#include "RpcTransport.hpp"

#include <QtEndian>

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
        qint64 timeout_ms = 0) {

        if (!NekoGui::dataStore->core_running) {
            return QNetworkReply::NetworkError(-1919);
        }

        std::string serialized;
        if (!req.SerializeToString(&serialized)) {
            return QNetworkReply::NetworkError(-114514);
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
        request.setRawHeader("grpc-accept-encoding", "identity,deflate,gzip");
        request.setRawHeader("accept-encoding", "identity,gzip");
        request.setRawHeader("te", "trailers");
        request.setRawHeader("nekoray_auth", auth_);

        // Preserve the existing framing implementation for W10 / PR-12.
        QByteArray message(5, '\0');
        *reinterpret_cast<int *>(message.data() + 1) =
            qToBigEndian(static_cast<int>(args.size()));
        message += args;

        const auto result = transport_.send(request, message, timeout_ms);
        if (result.error != QNetworkReply::NoError) return result.error;

        const int grpcStatus = result.grpcStatus.toInt();
        if (grpcStatus != 0) {
            MW_show_log(
                QStringLiteral("grpc-status error code: %1, error msg: %2")
                    .arg(grpcStatus)
                    .arg(QString::fromLatin1(result.grpcMessage)));
            return QNetworkReply::ProtocolUnknownError;
        }

        const QByteArray response = result.body.mid(5);
        if (!rsp->ParseFromArray(response.constData(), response.size())) {
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

    Client::Client(std::function<void(const QString &)> onError, const QString &target, const QString &token) {
        this->make_grpc_channel = [=]() { return std::make_unique<QtGrpc::Http2GrpcChannelPrivate>(target, token, "libcore.LibcoreService"); };
        this->default_grpc_channel = make_grpc_channel();
        this->onError = std::move(onError);
    }

#define NOT_OK      \
    *rpcOK = false; \
    onError(QStringLiteral("QNetworkReply::NetworkError code: %1\n").arg(status));

    void Client::Exit() {
        libcore::EmptyReq request;
        libcore::EmptyResp reply;
        default_grpc_channel->Call("Exit", request, &reply, 500);
    }

    QString Client::Start(bool *rpcOK, const libcore::LoadConfigReq &request) {
        libcore::ErrorResp reply;
        auto status = default_grpc_channel->Call("Start", request, &reply);

        if (status == QNetworkReply::NoError) {
            *rpcOK = true;
            return {reply.error().c_str()};
        } else {
            NOT_OK
            return "";
        }
    }

    QString Client::Stop(bool *rpcOK) {
        libcore::EmptyReq request;
        libcore::ErrorResp reply;
        auto status = default_grpc_channel->Call("Stop", request, &reply);

        if (status == QNetworkReply::NoError) {
            *rpcOK = true;
            return {reply.error().c_str()};
        } else {
            NOT_OK
            return "";
        }
    }

    long long Client::QueryStats(const std::string &tag, const std::string &direct) {
        libcore::QueryStatsReq request;
        request.set_tag(tag);
        request.set_direct(direct);

        libcore::QueryStatsResp reply;
        auto status = default_grpc_channel->Call("QueryStats", request, &reply, 500);

        if (status == QNetworkReply::NoError) {
            return reply.traffic();
        } else {
            return 0;
        }
    }

    std::string Client::ListConnections() {
        libcore::EmptyReq request;
        libcore::ListConnectionsResp reply;
        auto status = default_grpc_channel->Call("ListConnections", request, &reply, 500);

        if (status == QNetworkReply::NoError) {
            return reply.nekoray_connections_json();
        } else {
            return "";
        }
    }

    //

    libcore::TestResp Client::Test(bool *rpcOK,
                                  const libcore::TestReq &request) {
        libcore::TestResp reply;
        const qint64 timeoutMs = NkrW04::testTimeoutMs(
            request.timeout(),
            request.mode() == libcore::FullTest,
            request.full_speed(),
            request.full_speed_timeout());

        auto status = make_grpc_channel()->Call(
            "Test", request, &reply, timeoutMs);

        if (status == QNetworkReply::NoError) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return reply;
        }
    }

    libcore::UpdateResp Client::Update(bool *rpcOK, const libcore::UpdateReq &request) {
        libcore::UpdateResp reply;
        auto status = default_grpc_channel->Call("Update", request, &reply);

        if (status == QNetworkReply::NoError) {
            *rpcOK = true;
            return reply;
        } else {
            NOT_OK
            return reply;
        }
    }
} // namespace NekoGui_rpc

#endif
