#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QNetworkProxy>
#include <functional>

namespace NekoGui_network {
    struct NekoHTTPResponse {
        QString error;
        QByteArray data;
        QList<QPair<QByteArray, QByteArray>> header;
    };

    struct NekoHTTPOptions {
        QNetworkProxy proxy;
        QString userAgent;
        QString error;
        bool insecure = false;
    };
    class NetworkRequestHelper : QObject {
        Q_OBJECT

        explicit NetworkRequestHelper(QObject *parent) : QObject(parent){};

        ~NetworkRequestHelper() override = default;
        ;

    public:
        static NekoHTTPOptions SnapshotOptions();
        static NekoHTTPResponse HttpGet(const QUrl &url);
        static NekoHTTPResponse HttpGet(const QUrl &url, const NekoHTTPOptions &options);

        static QString GetHeader(const QList<QPair<QByteArray, QByteArray>> &header, const QString &name);
    };
} // namespace NekoGui_network

using namespace NekoGui_network;
