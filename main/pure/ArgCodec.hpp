#pragma once

#include <QString>
#include <QStringList>

namespace NkrInput {

QString encodeArgs(const QStringList &args);

// On failure, out is unchanged.
bool decodeArgs(const QString &text, QStringList &out);

} // namespace NkrInput
