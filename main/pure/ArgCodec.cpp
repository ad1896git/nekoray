#include "ArgCodec.hpp"

namespace NkrInput {

QString encodeArgs(const QStringList &args) {
    QStringList encoded;
    for (const QString &arg : args) {
        bool quote = arg.isEmpty();
        for (QChar c : arg) {
            if (c.isSpace() || c == QLatin1Char('"') ||
                c == QLatin1Char('\'') || c == QLatin1Char('\\')) {
                quote = true;
                break;
            }
        }

        if (!quote) {
            encoded.append(arg);
            continue;
        }

        QString item;
        item += QLatin1Char('"');
        for (QChar c : arg) {
            if (c == QLatin1Char('\\') || c == QLatin1Char('"')) {
                item += QLatin1Char('\\');
            }
            item += c;
        }
        item += QLatin1Char('"');
        encoded.append(item);
    }
    return encoded.join(QLatin1Char(' '));
}

bool decodeArgs(const QString &text, QStringList &out) {
    QStringList parsed;
    QString current;
    QChar quote;
    bool started = false;

    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);

        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            } else if (quote == QLatin1Char('"') &&
                       c == QLatin1Char('\\') &&
                       i + 1 < text.size() &&
                       (text.at(i + 1) == QLatin1Char('\\') ||
                        text.at(i + 1) == QLatin1Char('"'))) {
                current += text.at(++i);
            } else {
                current += c;
            }
            continue;
        }

        if (c.isSpace()) {
            if (started) {
                parsed.append(current);
                current.clear();
                started = false;
            }
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            quote = c;
            started = true;
        } else {
            current += c;
            started = true;
        }
    }

    if (!quote.isNull()) {
        return false;
    }
    if (started) {
        parsed.append(current);
    }
    out = parsed;
    return true;
}

} // namespace NkrInput
