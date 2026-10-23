#include <QtTest>
#include <string>
#include "go/grpc_server/gen/libcore.pb.h"

class UpdateResponseTest : public QObject {
    Q_OBJECT
private slots:
    void emptyValuesMeanNoUpdate() {
        libcore::UpdateResp response;
        QVERIFY(response.download_url().empty());
        response.set_download_url("");
        QVERIFY(response.download_url().empty());
    }
    void inspectingUrlPreservesResponse() {
        libcore::UpdateResp response;
        response.set_download_url("https://example.invalid/update.zip");
        response.set_assets_name("update.zip");
        response.set_release_url("https://example.invalid/release");
        response.set_release_note("notes");
        const std::string before = response.SerializeAsString();
        for (int i = 0; i < 1000; ++i) QVERIFY(!response.download_url().empty());
        QVERIFY(response.SerializeAsString() == before);
        QVERIFY(response.download_url() == "https://example.invalid/update.zip");
        const libcore::UpdateResp copy = response;
        QVERIFY(copy.download_url() == response.download_url());
    }
};
QTEST_GUILESS_MAIN(UpdateResponseTest)
#include "tst_w22.moc"
