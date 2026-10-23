#pragma once

#include <QByteArray>
#include <QFile>
#include <QMap>
#include <QString>
#include <functional>
#include <stdexcept>

namespace Renumber {
using Files = QMap<QString, QByteArray>;
struct Hooks {
    std::function<void(const QString &)> point;
    // True reports failure without performing the operation.
    std::function<bool(const QString &)> fail;
};
class Error : public std::runtime_error {
public:
    explicit Error(const QString &message) : std::runtime_error(message.toStdString()) {}
};
void Require(bool condition, const QString &message);
QByteArray Read(const QString &path);
bool Exists(const QString &path);
bool Pending();
Files Scan();
class Lease {
public:
    Lease() = default;
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    bool Begin(bool reorder);
    void Ready();
    bool Exclusive() const { return exclusive_; }
private:
    void Lock(int byte, bool exclusive, bool unlock = false);
    QFile file_;
    bool exclusive_ = false;
    bool gate_ = false;
};
void Start(Lease &lease, const Files &oldFiles, const Files &newFiles, const Hooks &hooks = {});
void Recover(Lease &lease, const Hooks &hooks = {});
} // namespace Renumber

namespace NekoGui {
Renumber::Files BuildRenumbered(const Renumber::Files &oldFiles);
void PrepareConfiguration(bool reorder, Renumber::Lease &lease, const Renumber::Hooks &hooks = {});
bool ReloadConfiguration();
} // namespace NekoGui
