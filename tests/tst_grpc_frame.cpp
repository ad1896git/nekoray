#include <QtTest/QTest>

#include "rpc/GrpcFrame.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

class GrpcFrameTest : public QObject
{
    Q_OBJECT

private slots:
    void wireVectors_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<QByteArray>("wire");

        QTest::newRow("empty")
            << QByteArray()
            << QByteArray::fromHex("0000000000");

        QTest::newRow("one-byte")
            << QByteArray("a")
            << QByteArray::fromHex("000000000161");

        QTest::newRow("binary")
            << QByteArray::fromHex("007f80ff")
            << QByteArray::fromHex("0000000004007f80ff");

        const QByteArray payload256(256, 'x');
        QTest::newRow("length-256")
            << payload256
            << (QByteArray::fromHex("0000000100") + payload256);

        const QByteArray payload65536(65536, '\0');
        QTest::newRow("length-65536")
            << payload65536
            << (QByteArray::fromHex("0000010000") + payload65536);
    }

    void wireVectors()
    {
        QFETCH(QByteArray, payload);
        QFETCH(QByteArray, wire);

        const auto encoded = NkrGrpcFrame::encode(payload);
        QVERIFY(encoded.error == NkrGrpcFrame::Error::None);
        QCOMPARE(encoded.bytes, wire);

        // Decode the independent wire vector, not just encoder output.
        const auto decoded = NkrGrpcFrame::decode(wire, "0");
        QVERIFY(decoded.error == NkrGrpcFrame::Error::None);
        QCOMPARE(decoded.bytes, payload);
    }

    void sizeBoundary()
    {
        QCOMPARE(
            NkrGrpcFrame::MaxPayloadSize,
            static_cast<quint64>((std::numeric_limits<int>::max)()) - 5);

        QVERIFY(NkrGrpcFrame::payloadSizeSupported(0));
        QVERIFY(NkrGrpcFrame::payloadSizeSupported(
            NkrGrpcFrame::MaxPayloadSize - 1));
        QVERIFY(NkrGrpcFrame::payloadSizeSupported(
            NkrGrpcFrame::MaxPayloadSize));
        QVERIFY(!NkrGrpcFrame::payloadSizeSupported(
            NkrGrpcFrame::MaxPayloadSize + 1));
        QVERIFY(!NkrGrpcFrame::payloadSizeSupported(
            static_cast<quint64>(
                (std::numeric_limits<quint32>::max)())));
        QVERIFY(!NkrGrpcFrame::payloadSizeSupported(
            (std::numeric_limits<quint64>::max)()));
    }

    void status_data()
    {
        using Error = NkrGrpcFrame::Error;
        QTest::addColumn<QByteArray>("status");
        QTest::addColumn<int>("expected");

        QTest::newRow("success")
            << QByteArray("0") << int(Error::None);
        QTest::newRow("leading-zeroes")
            << QByteArray("000") << int(Error::None);
        QTest::newRow("missing")
            << QByteArray() << int(Error::MissingStatus);
        QTest::newRow("cancelled")
            << QByteArray("1") << int(Error::NonzeroStatus);
        QTest::newRow("unauthenticated")
            << QByteArray("16") << int(Error::NonzeroStatus);
        QTest::newRow("unknown-nonzero-code")
            << QByteArray("17") << int(Error::NonzeroStatus);
        QTest::newRow("nonzero-with-leading-zeroes")
            << QByteArray("0007") << int(Error::NonzeroStatus);
        QTest::newRow("integer-overflow")
            << QByteArray("999999999999999999999999999999")
            << int(Error::NonzeroStatus);
        QTest::newRow("word")
            << QByteArray("OK") << int(Error::InvalidStatus);
        QTest::newRow("plus-sign")
            << QByteArray("+0") << int(Error::InvalidStatus);
        QTest::newRow("minus-sign")
            << QByteArray("-0") << int(Error::InvalidStatus);
        QTest::newRow("leading-space")
            << QByteArray(" 0") << int(Error::InvalidStatus);
        QTest::newRow("trailing-space")
            << QByteArray("0 ") << int(Error::InvalidStatus);
        QTest::newRow("trailing-newline")
            << QByteArray("0\n") << int(Error::InvalidStatus);
        QTest::newRow("decimal")
            << QByteArray("0.0") << int(Error::InvalidStatus);
        QTest::newRow("multiple-values")
            << QByteArray("0,0") << int(Error::InvalidStatus);
        QTest::newRow("embedded-nul")
            << QByteArray("0\0", 2) << int(Error::InvalidStatus);
        QTest::newRow("non-ascii")
            << QByteArray::fromHex("ff") << int(Error::InvalidStatus);
        QTest::newRow("malformed-nonzero")
            << QByteArray("7x") << int(Error::InvalidStatus);
    }

    void status()
    {
        QFETCH(QByteArray, status);
        QFETCH(int, expected);

        const auto result = NkrGrpcFrame::decode(
            QByteArray::fromHex("0000000003616263"), status);

        QCOMPARE(int(result.error), expected);
        if (result.error == NkrGrpcFrame::Error::None)
            QCOMPARE(result.bytes, QByteArray("abc"));
        else
            QVERIFY(result.bytes.isEmpty());
    }

    void invalidFrames_data()
    {
        using Error = NkrGrpcFrame::Error;
        QTest::addColumn<QByteArray>("wire");
        QTest::addColumn<int>("expected");

        for (int length = 0; length < 5; ++length) {
            const auto name =
                QByteArray("short-") + QByteArray::number(length);
            QTest::newRow(name.constData())
                << QByteArray(length, '\0')
                << int(Error::ShortFrame);
        }

        QTest::newRow("declared-one-without-payload")
            << QByteArray::fromHex("0000000001")
            << int(Error::LengthMismatch);
        QTest::newRow("declared-zero-with-payload")
            << QByteArray::fromHex("000000000061")
            << int(Error::LengthMismatch);
        QTest::newRow("truncated-payload")
            << QByteArray::fromHex("000000000261")
            << int(Error::LengthMismatch);
        QTest::newRow("trailing-junk")
            << QByteArray::fromHex("000000000161ff")
            << int(Error::LengthMismatch);
        QTest::newRow("two-frames")
            << QByteArray::fromHex("00000000000000000000")
            << int(Error::LengthMismatch);
        QTest::newRow("little-endian-length")
            << QByteArray::fromHex("000100000061")
            << int(Error::LengthMismatch);

        QTest::newRow("compressed")
            << QByteArray::fromHex("0100000000")
            << int(Error::UnsupportedCompression);
        QTest::newRow("reserved-flag")
            << QByteArray::fromHex("0200000000")
            << int(Error::UnsupportedCompression);
        QTest::newRow("high-bit-flag")
            << QByteArray::fromHex("8000000000")
            << int(Error::UnsupportedCompression);
        QTest::newRow("all-flag-bits")
            << QByteArray::fromHex("ff00000000")
            << int(Error::UnsupportedCompression);

        // Supported platforms have 32-bit int. These headers exercise
        // boundary validation without allocating multi-gigabyte buffers.
        QTest::newRow("maximum-length-without-payload")
            << QByteArray::fromHex("007ffffffa")
            << int(Error::LengthMismatch);
        QTest::newRow("maximum-plus-one")
            << QByteArray::fromHex("007ffffffb")
            << int(Error::TooLarge);
        QTest::newRow("signed-high-bit")
            << QByteArray::fromHex("0080000000")
            << int(Error::TooLarge);
        QTest::newRow("uint32-maximum")
            << QByteArray::fromHex("00ffffffff")
            << int(Error::TooLarge);
    }

    void invalidFrames()
    {
        QFETCH(QByteArray, wire);
        QFETCH(int, expected);

        const auto result = NkrGrpcFrame::decode(wire, "0");
        QCOMPARE(int(result.error), expected);
        QVERIFY(result.bytes.isEmpty());
    }

    void trailersOnly()
    {
        const auto failed = NkrGrpcFrame::decode({}, "7");
        QVERIFY(failed.error == NkrGrpcFrame::Error::NonzeroStatus);
        QVERIFY(failed.bytes.isEmpty());

        const auto missing = NkrGrpcFrame::decode({}, {});
        QVERIFY(missing.error == NkrGrpcFrame::Error::MissingStatus);
        QVERIFY(missing.bytes.isEmpty());

        const auto successfulWithoutFrame =
            NkrGrpcFrame::decode({}, "0");
        QVERIFY(successfulWithoutFrame.error ==
                NkrGrpcFrame::Error::ShortFrame);
        QVERIFY(successfulWithoutFrame.bytes.isEmpty());
    }

    void unalignedLengthReads()
    {
        const auto wire =
            QByteArray::fromHex("0000000004007f80ff");
        const auto payload = QByteArray::fromHex("007f80ff");

        alignas(quint32) char storage[32] = {};
        int unaligned = 0;

        for (int offset = 0; offset < int(alignof(quint32)); ++offset) {
            std::memcpy(
                storage + offset, wire.constData(),
                static_cast<std::size_t>(wire.size()));

            const auto view = QByteArray::fromRawData(
                storage + offset, static_cast<int>(wire.size()));
            QVERIFY(view.constData() == storage + offset);

            const auto address = reinterpret_cast<std::uintptr_t>(
                view.constData() + 1);
            if (address % alignof(quint32) != 0)
                ++unaligned;

            const auto decoded = NkrGrpcFrame::decode(view, "0");
            QVERIFY(decoded.error == NkrGrpcFrame::Error::None);
            QCOMPARE(decoded.bytes, payload);
            QCOMPARE(view, wire);
        }

        QCOMPARE(unaligned, int(alignof(quint32)) - 1);
    }

    void encoderLengthWrite()
    {
        const auto encoded =
            NkrGrpcFrame::encode(QByteArray(256, 'x'));
        QVERIFY(encoded.error == NkrGrpcFrame::Error::None);
        QCOMPARE(encoded.bytes.left(5),
                 QByteArray::fromHex("0000000100"));

        // QByteArray's allocation is aligned; the wire length begins
        // one byte later. UBSan must permit this byte-oriented write.
        const auto address = reinterpret_cast<std::uintptr_t>(
            encoded.bytes.constData() + 1);
        QVERIFY(address % alignof(quint32) != 0);
    }
};

QTEST_APPLESS_MAIN(GrpcFrameTest)
#include "tst_grpc_frame.moc"
