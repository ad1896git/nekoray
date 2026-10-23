#pragma once

#include <QString>

namespace NkrInput {

// Call after every other template replacement.
QString applyVpnAuth(QString config, bool enabled,
                     const QString &username, const QString &password);

} // namespace NkrInput
