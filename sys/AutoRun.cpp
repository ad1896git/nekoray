#include "AutoRun.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>

#include "3rdparty/fix_old_qt.h"

#ifdef Q_OS_LINUX
#include "main/NekoGui.hpp"
#endif

#ifdef Q_OS_MACOS
#include "AutoRunMac_p.hpp"

#include <QDebug>
#include <QFile>
#endif

#ifdef Q_OS_WIN

#include <QSettings>

QString Windows_GenAutoRunString() {
    auto appPath = QApplication::applicationFilePath();
    appPath = "\"" + QDir::toNativeSeparators(appPath) + "\"";
    appPath += " -tray";
    return appPath;
}

void AutoRun_SetEnabled(bool enable) {
    // 以程序名称作为注册表中的键
    // 根据键获取对应的值（程序路径）
    auto appPath = QApplication::applicationFilePath();
    QFileInfo fInfo(appPath);
    QString name = fInfo.baseName();

    QSettings settings("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);

    if (enable) {
        settings.setValue(name, Windows_GenAutoRunString());
    } else {
        settings.remove(name);
    }
}

bool AutoRun_IsEnabled() {
    QSettings settings("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);

    // 以程序名称作为注册表中的键
    // 根据键获取对应的值（程序路径）
    auto appPath = QApplication::applicationFilePath();
    QFileInfo fInfo(appPath);
    QString name = fInfo.baseName();

    return settings.value(name).toString() == Windows_GenAutoRunString();
}

#endif

#ifdef Q_OS_MACOS

namespace NkrAutoRunMac {
namespace {

template<typename T>
class OwnedCF {
public:
    OwnedCF(T value, const Api &api) : value_(value), api_(api) {}

    ~OwnedCF() {
        if (value_) {
            api_.release(value_);
        }
    }

    OwnedCF(const OwnedCF &) = delete;
    OwnedCF &operator=(const OwnedCF &) = delete;

    T get() const {
        return value_;
    }

private:
    T value_;
    const Api &api_;
};

} // namespace

QString bundlePath(const QString &applicationDirectory) {
    if (!QDir::isAbsolutePath(applicationDirectory)) {
        return {};
    }

    QString path = QDir::cleanPath(applicationDirectory);
    const QString suffix = QStringLiteral("/Contents/MacOS");
    if (!path.endsWith(suffix)) {
        return {};
    }

    path.chop(suffix.size());
    if (!path.endsWith(QStringLiteral(".app"))) {
        return {};
    }

    return path;
}

Api nativeApi() {
    Api api;

    api.createUrl = [](const QByteArray &path) {
        return CFURLCreateFromFileSystemRepresentation(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8 *>(path.constData()),
            static_cast<CFIndex>(path.size()),
            true);
    };

    // W23a retains the legacy login-item backend. Its deprecation warnings
    // remain visible; adopting SMAppService requires a separate bundle design.
    api.createList = [] {
        return LSSharedFileListCreate(
            kCFAllocatorDefault, kLSSharedFileListSessionLoginItems, nullptr);
    };

    api.copySnapshot = [](LSSharedFileListRef list) {
        return LSSharedFileListCopySnapshot(list, nullptr);
    };

    api.resolve = [](LSSharedFileListItemRef item) {
        return LSSharedFileListItemCopyResolvedURL(
            item,
            kLSSharedFileListNoUserInteraction |
                kLSSharedFileListDoNotMountVolumes,
            nullptr);
    };

    api.insert = [](LSSharedFileListRef list, CFURLRef url) {
        return LSSharedFileListInsertItemURL(
            list, kLSSharedFileListItemLast, nullptr, nullptr,
            url, nullptr, nullptr);
    };

    api.remove = [](LSSharedFileListRef list, LSSharedFileListItemRef item) {
        return LSSharedFileListItemRemove(list, item) == noErr;
    };

    api.release = [](CFTypeRef value) {
        CFRelease(value);
    };

    return api;
}

bool execute(const QString &applicationDirectory,
             Operation operation,
             const Api &api) {
    const QString path = bundlePath(applicationDirectory);
    if (path.isEmpty()) {
        qWarning("macOS autorun: unsupported executable layout; "
                 "expected .app/Contents/MacOS");
        return false;
    }

    OwnedCF<CFURLRef> target(api.createUrl(QFile::encodeName(path)), api);
    if (!target.get()) {
        qWarning("macOS autorun: cannot create application URL");
        return false;
    }

    OwnedCF<LSSharedFileListRef> list(api.createList(), api);
    if (!list.get()) {
        qWarning("macOS autorun: cannot open login-item list");
        return false;
    }

    OwnedCF<CFArrayRef> snapshot(api.copySnapshot(list.get()), api);
    if (!snapshot.get()) {
        qWarning("macOS autorun: cannot read login-item snapshot");
        return false;
    }

    bool found = false;
    bool complete = true;

    const CFIndex count = CFArrayGetCount(snapshot.get());
    for (CFIndex i = 0; i < count; ++i) {
        const void *value = CFArrayGetValueAtIndex(snapshot.get(), i);
        if (!value) {
            qWarning("macOS autorun: null login item");
            complete = false;
            continue;
        }

        auto item = static_cast<LSSharedFileListItemRef>(
            const_cast<void *>(value));
        OwnedCF<CFURLRef> itemUrl(api.resolve(item), api);
        if (!itemUrl.get()) {
            qWarning("macOS autorun: cannot resolve login item");
            complete = false;
            continue;
        }

        if (!CFEqual(itemUrl.get(), target.get())) {
            continue;
        }

        found = true;
        if (operation == Operation::Disable && !api.remove(list.get(), item)) {
            qWarning("macOS autorun: cannot remove login item");
            complete = false;
        }
    }

    if (operation == Operation::Query) {
        return found;
    }

    if (operation == Operation::Disable) {
        return complete;
    }

    if (found) {
        return true;
    }

    if (!complete) {
        qWarning("macOS autorun: cannot establish absence; enable aborted");
        return false;
    }

    OwnedCF<LSSharedFileListItemRef> inserted(
        api.insert(list.get(), target.get()), api);
    if (!inserted.get()) {
        qWarning("macOS autorun: cannot insert login item");
        return false;
    }

    return true;
}

} // namespace NkrAutoRunMac

void AutoRun_SetEnabled(bool enable) {
    const auto api = NkrAutoRunMac::nativeApi();
    NkrAutoRunMac::execute(
        QCoreApplication::applicationDirPath(),
        enable ? NkrAutoRunMac::Operation::Enable
               : NkrAutoRunMac::Operation::Disable,
        api);
}

bool AutoRun_IsEnabled() {
    const auto api = NkrAutoRunMac::nativeApi();
    return NkrAutoRunMac::execute(
        QCoreApplication::applicationDirPath(),
        NkrAutoRunMac::Operation::Query,
        api);
}

#endif

#ifdef Q_OS_LINUX

#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QTextStream>

#define NEWLINE "\n"

//  launchatlogin.cpp
//  ShadowClash
//
//  Created by TheWanderingCoel on 2018/6/12.
//  Copyright © 2019 Coel Wu. All rights reserved.
//
QString getUserAutostartDir_private() {
    QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    config += QLatin1String("/autostart/");
    return config;
}

void AutoRun_SetEnabled(bool enable) {
    // From https://github.com/nextcloud/desktop/blob/master/src/common/utility_unix.cpp
    QString appName = QCoreApplication::applicationName();
    QString userAutoStartPath = getUserAutostartDir_private();
    QString desktopFileLocation = userAutoStartPath + appName + QLatin1String(".desktop");
    QStringList appCmdList;

    // nekoray: launcher
    if (qEnvironmentVariable("NKR_FROM_LAUNCHER") == "1") {
        appCmdList << QApplication::applicationDirPath() + "/launcher"
                   << "--";
    } else {
        if (QProcessEnvironment::systemEnvironment().contains("APPIMAGE")) {
            appCmdList << QProcessEnvironment::systemEnvironment().value("APPIMAGE");
        } else {
            appCmdList << QApplication::applicationFilePath();
        }
    }

    appCmdList << "-tray";

    if (NekoGui::dataStore->flag_use_appdata) {
        appCmdList << "-appdata";
    }

    if (enable) {
        if (!QDir().exists(userAutoStartPath) && !QDir().mkpath(userAutoStartPath)) {
            // qCWarning(lcUtility) << "Could not create autostart folder"
            // << userAutoStartPath;
            return;
        }

        QFile iniFile(desktopFileLocation);

        if (!iniFile.open(QIODevice::WriteOnly)) {
            // qCWarning(lcUtility) << "Could not write auto start entry" <<
            // desktopFileLocation;
            return;
        }

        QTextStream ts(&iniFile);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        ts.setCodec("UTF-8");
#endif
        ts << QLatin1String("[Desktop Entry]") << NEWLINE
           << QLatin1String("Name=") << appName << NEWLINE
           << QLatin1String("Exec=") << appCmdList.join(" ") << NEWLINE
           << QLatin1String("Terminal=") << "false" << NEWLINE
           << QLatin1String("Categories=") << "Network" << NEWLINE
           << QLatin1String("Type=") << "Application" << NEWLINE
           << QLatin1String("StartupNotify=") << "false" << NEWLINE
           << QLatin1String("X-GNOME-Autostart-enabled=") << "true" << NEWLINE;
        ts.flush();
        iniFile.close();
    } else {
        QFile::remove(desktopFileLocation);
    }
}

bool AutoRun_IsEnabled() {
    QString appName = QCoreApplication::applicationName();
    QString desktopFileLocation = getUserAutostartDir_private() + appName + QLatin1String(".desktop");
    return QFile::exists(desktopFileLocation);
}

#endif
