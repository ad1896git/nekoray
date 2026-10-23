#include <QCoreApplication>
#include <QObject>
#include <QtTest/QTest>

class SmokeTest : public QObject
{
    Q_OBJECT

private slots:
    void smoke()
    {
        QVERIFY(QCoreApplication::instance() != nullptr);
    }
};

QTEST_MAIN(SmokeTest)

#include "smoke_test.moc"
