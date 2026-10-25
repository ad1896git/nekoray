#include "main/UiSnapshot.hpp"
#include "HTTPRequestHelper.hpp"

#include <QByteArray>
#include <QNetworkProxy>
#include <QEventLoop>
#include <QMetaEnum>
#include <QTimer>

#include "main/NekoGui.hpp"

namespace NekoGui_network {

    NekoHTTPOptions NetworkRequestHelper::SnapshotOptions() {
        NekoGui_concurrency::RequireUiThread();
        NekoHTTPOptions options;
        options.userAgent = NekoGui::dataStore->GetUserAgent();
        options.insecure = NekoGui::dataStore->sub_insecure;
        if (NekoGui::dataStore->sub_use_proxy) {
            options.proxy.setType(QNetworkProxy::HttpProxy);
            options.proxy.setHostName(QStringLiteral("127.0.0.1"));
            options.proxy.setPort(NekoGui::dataStore->inbound_socks_port);
            if (NekoGui::dataStore->inbound_auth->NeedAuth()) {
                options.proxy.setUser(NekoGui::dataStore->inbound_auth->username);
                options.proxy.setPassword(NekoGui::dataStore->inbound_auth->password);
            }
            if (NekoGui::dataStore->started_id < 0)
                options.error = QObject::tr("Request with proxy but no profile started.");
        }
        return options;
    }
    NekoHTTPResponse NetworkRequestHelper::HttpGet(const QUrl &url) {
        const auto options = NekoGui_concurrency::ReadUi<NekoHTTPOptions>([] { return SnapshotOptions(); });
        if (!options) return NekoHTTPResponse{QObject::tr("Subscription request cancelled.")};
        return HttpGet(url, *options);
    }
    NekoHTTPResponse NetworkRequestHelper::HttpGet(const QUrl &url, const NekoHTTPOptions &options) {
        QNetworkRequest request;
        QNetworkAccessManager accessManager;
        request.setUrl(url);
        if (!options.error.isEmpty()) return NekoHTTPResponse{options.error};
        accessManager.setProxy(options.proxy);
        // Set attribute
#if (QT_VERSION >= QT_VERSION_CHECK(5, 9, 0))
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
        request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, options.userAgent);
        if (options.insecure) {
            QSslConfiguration c;
            c.setPeerVerifyMode(QSslSocket::PeerVerifyMode::VerifyNone);
            request.setSslConfiguration(c);
        }
        //
        auto _reply = accessManager.get(request);
        connect(_reply, &QNetworkReply::sslErrors, _reply, [insecure = options.insecure](const QList<QSslError> &errors) {
            QStringList error_str;
            for (const auto &err: errors) {
                error_str << err.errorString();
            }
            MW_show_log(QStringLiteral("SSL Errors: %1 %2").arg(error_str.join(","), insecure ? "(Ignored)" : ""));
        });
        // Wait for response
        auto abortTimer = new QTimer;
        abortTimer->setSingleShot(true);
        abortTimer->setInterval(10000);
        QObject::connect(abortTimer, &QTimer::timeout, _reply, &QNetworkReply::abort);
        abortTimer->start();
        {
            QEventLoop loop;
            QObject::connect(_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
        }
        if (abortTimer != nullptr) {
            abortTimer->stop();
            abortTimer->deleteLater();
        }
        //
        auto result = NekoHTTPResponse{_reply->error() == QNetworkReply::NetworkError::NoError ? "" : _reply->errorString(),
                                       _reply->readAll(), _reply->rawHeaderPairs()};
        _reply->deleteLater();
        return result;
    }

    QString NetworkRequestHelper::GetHeader(const QList<QPair<QByteArray, QByteArray>> &header, const QString &name) {
        for (const auto &p: header) {
            if (QString(p.first).toLower() == name.toLower()) return p.second;
        }
        return "";
    }

} // namespace NekoGui_network
