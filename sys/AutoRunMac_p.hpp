#pragma once

#include <QtGlobal>

#ifdef Q_OS_MACOS

#include <QByteArray>
#include <QString>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>

#include <functional>

namespace NkrAutoRunMac {

enum class Operation {
    Query,
    Enable,
    Disable
};

// All create/copy/resolve/insert callbacks return owned references.
// Snapshot elements are borrowed.
// Every owned non-null reference is released through release().
struct Api {
    std::function<CFURLRef(const QByteArray &)> createUrl;
    std::function<LSSharedFileListRef()> createList;
    std::function<CFArrayRef(LSSharedFileListRef)> copySnapshot;
    std::function<CFURLRef(LSSharedFileListItemRef)> resolve;
    std::function<LSSharedFileListItemRef(LSSharedFileListRef, CFURLRef)> insert;
    std::function<bool(LSSharedFileListRef, LSSharedFileListItemRef)> remove;
    std::function<void(CFTypeRef)> release;
};

QString bundlePath(const QString &applicationDirectory);
Api nativeApi();

// Query: true means a matching item was found.
// Enable/Disable: true means the requested operation completed.
// A false query also covers unavailable/unsupported state; diagnostics distinguish it.
bool execute(const QString &applicationDirectory, Operation operation, const Api &api);

} // namespace NkrAutoRunMac

#endif
