#pragma once

#include <QByteArray>
#include <QtEndian>
#include <QtGlobal>

#include <limits>
#include <utility>

namespace NkrGrpcFrame {

inline constexpr int HeaderSize = 5;
inline constexpr quint64 MaxPayloadSize =
    static_cast<quint64>((std::numeric_limits<int>::max)()) - HeaderSize;

static_assert(
    MaxPayloadSize <=
        static_cast<quint64>((std::numeric_limits<quint32>::max)()),
    "The codec payload limit must fit the wire length field");

enum class Error {
    None,
    TooLarge,
    MissingStatus,
    InvalidStatus,
    NonzeroStatus,
    ShortFrame,
    UnsupportedCompression,
    LengthMismatch
};

struct Result {
    Error error;
    QByteArray bytes;
};

inline bool payloadSizeSupported(quint64 size)
{
    return size <= MaxPayloadSize;
}

inline const char *errorText(Error error)
{
    switch (error) {
    case Error::None:
        return "none";
    case Error::TooLarge:
        return "payload exceeds codec size limit";
    case Error::MissingStatus:
        return "missing grpc-status";
    case Error::InvalidStatus:
        return "invalid grpc-status";
    case Error::NonzeroStatus:
        return "nonzero grpc-status";
    case Error::ShortFrame:
        return "frame shorter than five bytes";
    case Error::UnsupportedCompression:
        return "unsupported compression flag";
    case Error::LengthMismatch:
        return "declared length differs from body length";
    }
    return "unknown codec error";
}

inline Result encode(const QByteArray &payload)
{
    const auto size = static_cast<quint64>(payload.size());
    if (!payloadSizeSupported(size))
        return {Error::TooLarge, {}};

    QByteArray frame(HeaderSize, '\0');
    qToBigEndian<quint32>(
        static_cast<quint32>(size), frame.data() + 1);
    frame += payload;
    return {Error::None, std::move(frame)};
}

inline Result decode(const QByteArray &frame,
                     const QByteArray &grpcStatus)
{
    if (grpcStatus.isEmpty())
        return {Error::MissingStatus, {}};

    bool nonzero = false;
    for (char c : grpcStatus) {
        if (c < '0' || c > '9')
            return {Error::InvalidStatus, {}};
        nonzero = nonzero || c != '0';
    }
    if (nonzero)
        return {Error::NonzeroStatus, {}};

    const auto size = static_cast<quint64>(frame.size());
    if (size < HeaderSize)
        return {Error::ShortFrame, {}};

    if (frame.at(0) != '\0')
        return {Error::UnsupportedCompression, {}};

    const quint32 length =
        qFromBigEndian<quint32>(frame.constData() + 1);

    if (!payloadSizeSupported(length))
        return {Error::TooLarge, {}};

    if (static_cast<quint64>(length) != size - HeaderSize)
        return {Error::LengthMismatch, {}};

    return {Error::None, frame.mid(HeaderSize)};
}

} // namespace NkrGrpcFrame
