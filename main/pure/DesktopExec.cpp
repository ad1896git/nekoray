#include "DesktopExec.hpp"

namespace NkrInput {

QString desktopExecArg(const QString &arg) {
    const QString reserved = QStringLiteral(" \t\n\r\"'\\><~|&;$*?#()`");
    bool quote = arg.isEmpty();
    for (QChar c : arg) {
        if (reserved.contains(c)) {
            quote = true;
            break;
        }
    }

    QString execLayer;
    for (QChar c : arg) {
        if (c == QLatin1Char('"') || c == QLatin1Char('`') ||
            c == QLatin1Char('$') || c == QLatin1Char('\\')) {
            execLayer += QLatin1Char('\\');
            execLayer += c;
        } else if (c == QLatin1Char('%')) {
            execLayer += QStringLiteral("%%");
        } else {
            execLayer += c;
        }
    }
    if (quote) {
        execLayer = QLatin1Char('"') + execLayer + QLatin1Char('"');
    }

    QString valueLayer;
    for (QChar c : execLayer) {
        if (c == QLatin1Char('\\')) {
            valueLayer += QStringLiteral("\\\\");
        } else if (c == QLatin1Char('\n')) {
            valueLayer += QStringLiteral("\\n");
        } else if (c == QLatin1Char('\t')) {
            valueLayer += QStringLiteral("\\t");
        } else if (c == QLatin1Char('\r')) {
            valueLayer += QStringLiteral("\\r");
        } else {
            valueLayer += c;
        }
    }
    return valueLayer;
}

QString desktopExec(const QStringList &args) {
    QStringList encoded;
    for (const QString &arg : args) {
        encoded.append(desktopExecArg(arg));
    }
    return encoded.join(QLatin1Char(' '));
}

} // namespace NkrInput
