#include "db/Database.hpp"
#include "db/Renumber.hpp"
#include <QSet>
#include <QRegularExpression>
#include "NekoGui.hpp"
#include "fmt/Preset.hpp"

#include <QFile>
#include <QDir>
#include <QApplication>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QDateTime>
#include <QUuid>

#include <cmath>
#include <limits>

#ifdef Q_OS_WIN
#include "sys/windows/guihelper.h"
#else
#ifdef Q_OS_LINUX
#include <sys/linux/LinuxCap.h>
#endif
#include <unistd.h>
#endif

namespace NekoGui_ConfigItem {

    // 添加关联
    void JsonStore::_add(configItem *item) {
        _map.insert(item->name, std::shared_ptr<configItem>(item));
    }

    QString JsonStore::_name(void *p) {
        for (const auto &_item: _map) {
            if (_item->ptr == p) return _item->name;
        }
        return {};
    }

    std::shared_ptr<configItem> JsonStore::_get(const QString &name) {
        // 直接 [] 会设置一个 nullptr ，所以先判断是否存在
        if (_map.contains(name)) {
            return _map[name];
        }
        return nullptr;
    }

    void JsonStore::_setValue(const QString &name, void *p) {
        auto item = _get(name);
        if (item == nullptr) return;

        switch (item->type) {
            case itemType::string:
                *(QString *) item->ptr = *(QString *) p;
                break;
            case itemType::boolean:
                *(bool *) item->ptr = *(bool *) p;
                break;
            case itemType::integer:
                *(int *) item->ptr = *(int *) p;
                break;
            case itemType::integer64:
                *(long long *) item->ptr = *(long long *) p;
                break;
            // others...
            case stringList:
            case integerList:
            case jsonStore:
                break;
        }
    }

    QJsonObject JsonStore::ToJson(const QStringList &without) {
        QJsonObject object;
        for (const auto &_item: _map) {
            auto item = _item.get();
            if (without.contains(item->name)) continue;
            switch (item->type) {
                case itemType::string:
                    // Allow Empty
                    if (!((QString *) item->ptr)->isEmpty()) {
                        object.insert(item->name, *(QString *) item->ptr);
                    }
                    break;
                case itemType::integer:
                    object.insert(item->name, *(int *) item->ptr);
                    break;
                case itemType::integer64:
                    object.insert(item->name, *(long long *) item->ptr);
                    break;
                case itemType::boolean:
                    object.insert(item->name, *(bool *) item->ptr);
                    break;
                case itemType::stringList:
                    object.insert(item->name, QList2QJsonArray<QString>(*(QList<QString> *) item->ptr));
                    break;
                case itemType::integerList:
                    object.insert(item->name, QList2QJsonArray<int>(*(QList<int> *) item->ptr));
                    break;
                case itemType::jsonStore:
                    // _add 时应关联对应 JsonStore 的指针
                    object.insert(item->name, ((JsonStore *) item->ptr)->ToJson());
                    break;
            }
        }
        return object;
    }

    QByteArray JsonStore::ToJsonBytes() {
        QJsonDocument document;
        document.setObject(ToJson());
        return document.toJson(save_control_compact ? QJsonDocument::Compact : QJsonDocument::Indented);
    }

    void JsonStore::FromJson(QJsonObject object) {
        for (const auto &key: object.keys()) {
            if (_map.count(key) == 0) {
                continue;
            }

            auto value = object[key];
            auto item = _map[key].get();

            if (item == nullptr)
                continue; // 故意忽略

            // 根据类型修改ptr的内容
            switch (item->type) {
                case itemType::string:
                    if (value.type() != QJsonValue::String) {
                        continue;
                    }
                    *(QString *) item->ptr = value.toString();
                    break;
                case itemType::integer:
                    if (value.type() != QJsonValue::Double) {
                        continue;
                    }
                    *(int *) item->ptr = value.toInt();
                    break;
                case itemType::integer64:
                    if (value.type() != QJsonValue::Double) {
                        continue;
                    }
                    *(long long *) item->ptr = value.toDouble();
                    break;
                case itemType::boolean:
                    if (value.type() != QJsonValue::Bool) {
                        continue;
                    }
                    *(bool *) item->ptr = value.toBool();
                    break;
                case itemType::stringList:
                    if (value.type() != QJsonValue::Array) {
                        continue;
                    }
                    *(QList<QString> *) item->ptr = QJsonArray2QListString(value.toArray());
                    break;
                case itemType::integerList:
                    if (value.type() != QJsonValue::Array) {
                        continue;
                    }
                    *(QList<int> *) item->ptr = QJsonArray2QListInt(value.toArray());
                    break;
                case itemType::jsonStore:
                    if (value.type() != QJsonValue::Object) {
                        continue;
                    }
                    ((JsonStore *) item->ptr)->FromJson(value.toObject());
                    break;
            }
        }

        if (callback_after_load != nullptr) callback_after_load();
    }

    namespace {

        bool JsonInteger(const QJsonValue &value, double lower, double upperExclusive) {
            if (!value.isDouble()) return false;
            const double number = value.toDouble();
            return std::isfinite(number) && std::trunc(number) == number &&
                   number >= lower && number < upperExclusive;
        }

        bool JsonInt(const QJsonValue &value) {
            return JsonInteger(value, double(std::numeric_limits<int>::min()),
                               double(std::numeric_limits<int>::max()) + 1.0);
        }

        bool IsolateCorruptFile(const QString &path) {
            const QFileInfo source(path);
            const QString directory = source.absolutePath() + "/corrupt";
            if (!QDir().mkpath(directory)) {
                qWarning() << "Cannot create corrupt-file directory" << directory;
                return false;
            }
            const QString suffix =
                QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzz") +
                "-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            const QString destination =
                directory + "/" + source.fileName() + "." + suffix + ".corrupt";
            QFile file(source.absoluteFilePath());
            if (!file.rename(destination)) {
                qWarning() << "Cannot isolate invalid config"
                           << source.absoluteFilePath() << file.errorString();
                return false;
            }
            qWarning() << "Isolated invalid config"
                       << source.absoluteFilePath() << "as" << destination;
            return true;
        }

    } // namespace

    bool JsonStore::Validate(const QJsonObject &object) const {
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const auto registered = _map.constFind(it.key());
            if (registered == _map.constEnd()) continue;
            const auto &item = registered.value();
            if (!item || !item->ptr) return false;
            const QJsonValue value = it.value();
            switch (item->type) {
                case itemType::string:
                    if (!value.isString()) return false;
                    break;
                case itemType::integer:
                    if (!JsonInt(value)) return false;
                    break;
                case itemType::integer64:
                    // Exclusive upper bound avoids conversion out of range at 2^63.
                    if (!JsonInteger(value, -9223372036854775808.0,
                                      9223372036854775808.0)) return false;
                    break;
                case itemType::boolean:
                    if (!value.isBool()) return false;
                    break;
                case itemType::stringList:
                    if (!value.isArray()) return false;
                    for (const auto element : value.toArray()) {
                        if (!element.isString()) return false;
                    }
                    break;
                case itemType::integerList:
                    if (!value.isArray()) return false;
                    for (const auto element : value.toArray()) {
                        if (!JsonInt(element)) return false;
                    }
                    break;
                case itemType::jsonStore:
                    if (!value.isObject() ||
                        !static_cast<JsonStore *>(item->ptr)->Validate(value.toObject())) {
                        return false;
                    }
                    break;
            }
        }
        return true;
    }

    bool JsonStore::FromJsonBytes(const QByteArray &data) {
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(data, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            qWarning() << "Invalid JSON object" << fn << error.errorString();
            return false;
        }
        const auto object = document.object();
        if (!Validate(object)) {
            qWarning() << "Invalid config schema" << fn;
            return false;
        }
        FromJson(object);
        return true;
    }

    qint64 JsonStore::WriteSaveFile(QSaveFile &file, const QByteArray &data) {
        return file.write(data);
    }

    bool JsonStore::CommitSaveFile(QSaveFile &file) {
        return file.commit();
    }

    SaveResult JsonStore::SaveChecked() {
        if (callback_before_save != nullptr) callback_before_save();
        if (save_control_no_save) return {false, false};
        const QString absolutePath = QFileInfo(fn).absoluteFilePath();
        if (fn.isEmpty() || absolutePath == blocked_save_path) {
            qWarning() << "Config save blocked" << fn;
            return {false, false};
        }
        const auto object = ToJson();
        if (!Validate(object)) {
            qWarning() << "Refusing to save invalid config" << fn;
            return {false, false};
        }
        const QByteArray content = QJsonDocument(object).toJson(
            save_control_compact ? QJsonDocument::Compact : QJsonDocument::Indented);
        const bool changed = last_save_content != content;
        QSaveFile file(fn);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly)) {
            qWarning() << "Cannot open config for saving" << fn << file.errorString();
            return {false, false};
        }
        if (WriteSaveFile(file, content) != content.size()) {
            qWarning() << "Incomplete config write" << fn << file.errorString();
            file.cancelWriting();
            return {false, false};
        }
        if (!CommitSaveFile(file)) {
            qWarning() << "Cannot commit config" << fn << file.errorString();
            file.cancelWriting();
            return {false, false};
        }
        last_save_content = content;
        return {true, changed};
    }

    bool JsonStore::Save() {
        return SaveChecked().changed;
    }

    bool JsonStore::Load() {
        isolated_path.clear();
        load_can_create = false;
        if (Renumber::Pending()) {
            qWarning() << "Configuration load blocked by unfinished renumbering";
            return false;
        }
        const QFileInfo info(fn);
        const QString absolutePath = info.absoluteFilePath();
        blocked_save_path = absolutePath;
        if (fn.isEmpty()) {
            qWarning() << "Cannot load config with an empty path";
            return false;
        }
        if (!info.exists() && !info.isSymLink()) {
            blocked_save_path.clear();
            load_can_create = true;
            if (load_control_must) qWarning() << "Required config does not exist" << fn;
            return false;
        }
        if (!info.isFile() || info.isSymLink()) {
            qWarning() << "Config path is not a regular non-symlink file" << fn;
            return false;
        }
        QFile file(fn);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "Cannot open config" << fn << file.errorString();
            return false;
        }
        const QByteArray content = file.readAll();
        const bool readOk = file.error() == QFileDevice::NoError;
        const QString readError = file.errorString();
        file.close();
        if (!readOk) {
            qWarning() << "Cannot read config" << fn << readError;
            return false;
        }
        if (!FromJsonBytes(content)) {
            QDir corruptDirectory(info.absolutePath() + "/corrupt");
            const auto existingFiles = corruptDirectory.entryList(QDir::Files);
            if (IsolateCorruptFile(fn)) {
                corruptDirectory.refresh();
                for (const auto &name : corruptDirectory.entryList(QDir::Files)) {
                    if (!existingFiles.contains(name) &&
                        name.startsWith(info.fileName() + ".") && name.endsWith(".corrupt")) {
                        isolated_path = corruptDirectory.absoluteFilePath(name);
                        break;
                    }
                }
                blocked_save_path.clear();
                load_can_create = true;
            }
            return false;
        }
        last_save_content = content;
        blocked_save_path.clear();
        return true;
    }

    bool JsonStore::LoadOrCreate() {
        if (Load()) return true;
        if (!load_can_create) return false;
        return SaveChecked().ok;
    }

} // namespace NekoGui_ConfigItem

namespace NekoGui {

    DataStore *dataStore = new DataStore();

    // datastore

    DataStore::DataStore() : JsonStore() {
        _add(new configItem("extraCore", dynamic_cast<JsonStore *>(extraCore), itemType::jsonStore));
        _add(new configItem("inbound_auth", dynamic_cast<JsonStore *>(inbound_auth), itemType::jsonStore));

        _add(new configItem("user_agent2", &user_agent, itemType::string));
        _add(new configItem("test_url", &test_latency_url, itemType::string));
        _add(new configItem("test_url_dl", &test_download_url, itemType::string));
        _add(new configItem("test_dl_timeout", &test_download_timeout, itemType::integer));
        _add(new configItem("current_group", &current_group, itemType::integer));
        _add(new configItem("inbound_address", &inbound_address, itemType::string));
        _add(new configItem("inbound_socks_port", &inbound_socks_port, itemType::integer));
        _add(new configItem("log_level", &log_level, itemType::string));
        _add(new configItem("mux_protocol", &mux_protocol, itemType::string));
        _add(new configItem("mux_concurrency", &mux_concurrency, itemType::integer));
        _add(new configItem("mux_padding", &mux_padding, itemType::boolean));
        _add(new configItem("mux_default_on", &mux_default_on, itemType::boolean));
        _add(new configItem("traffic_loop_interval", &traffic_loop_interval, itemType::integer));
        _add(new configItem("test_concurrent", &test_concurrent, itemType::integer));
        _add(new configItem("theme", &theme, itemType::string));
        _add(new configItem("custom_inbound", &custom_inbound, itemType::string));
        _add(new configItem("custom_route", &custom_route_global, itemType::string));
        _add(new configItem("sub_use_proxy", &sub_use_proxy, itemType::boolean));
        _add(new configItem("remember_id", &remember_id, itemType::integer));
        _add(new configItem("remember_enable", &remember_enable, itemType::boolean));
        _add(new configItem("language", &language, itemType::integer));
        _add(new configItem("spmode2", &remember_spmode, itemType::stringList));
        _add(new configItem("skip_cert", &skip_cert, itemType::boolean));
        _add(new configItem("hk_mw", &hotkey_mainwindow, itemType::string));
        _add(new configItem("hk_group", &hotkey_group, itemType::string));
        _add(new configItem("hk_route", &hotkey_route, itemType::string));
        _add(new configItem("hk_spmenu", &hotkey_system_proxy_menu, itemType::string));
        _add(new configItem("fakedns", &fake_dns, itemType::boolean));
        _add(new configItem("active_routing", &active_routing, itemType::string));
        _add(new configItem("mw_size", &mw_size, itemType::string));
        _add(new configItem("conn_stat", &connection_statistics, itemType::boolean));
        _add(new configItem("vpn_impl", &vpn_implementation, itemType::integer));
        _add(new configItem("vpn_mtu", &vpn_mtu, itemType::integer));
        _add(new configItem("vpn_ipv6", &vpn_ipv6, itemType::boolean));
        _add(new configItem("vpn_hide_console", &vpn_hide_console, itemType::boolean));
        _add(new configItem("vpn_strict_route", &vpn_strict_route, itemType::boolean));
        _add(new configItem("vpn_bypass_process", &vpn_rule_process, itemType::string));
        _add(new configItem("vpn_bypass_cidr", &vpn_rule_cidr, itemType::string));
        _add(new configItem("vpn_rule_white", &vpn_rule_white, itemType::boolean));
        _add(new configItem("check_include_pre", &check_include_pre, itemType::boolean));
        _add(new configItem("sp_format", &system_proxy_format, itemType::string));
        _add(new configItem("sub_clear", &sub_clear, itemType::boolean));
        _add(new configItem("sub_insecure", &sub_insecure, itemType::boolean));
        _add(new configItem("sub_auto_update", &sub_auto_update, itemType::integer));
        _add(new configItem("log_ignore", &log_ignore, itemType::stringList));
        _add(new configItem("start_minimal", &start_minimal, itemType::boolean));
        _add(new configItem("max_log_line", &max_log_line, itemType::integer));
        _add(new configItem("splitter_state", &splitter_state, itemType::string));
        _add(new configItem("utlsFingerprint", &utlsFingerprint, itemType::string));
        _add(new configItem("core_box_clash_api", &core_box_clash_api, itemType::integer));
        _add(new configItem("core_box_clash_api_secret", &core_box_clash_api_secret, itemType::string));
        _add(new configItem("core_box_underlying_dns", &core_box_underlying_dns, itemType::string));
        _add(new configItem("vpn_internal_tun", &vpn_internal_tun, itemType::boolean));
    }

    void DataStore::UpdateStartedId(int id) {
        started_id = id;
        if (remember_enable) {
            remember_id = id;
            Save();
        } else if (remember_id >= 0) {
            remember_id = -1919;
            Save();
        }
    }

    QString DataStore::GetUserAgent(bool isDefault) const {
        if (user_agent.isEmpty()) {
            isDefault = true;
        }
        if (isDefault) {
            QString version = SubStrBefore(NKR_VERSION, "-");
            if (!version.contains(".")) version = "2.0";
            return "NekoBox/PC/" + version + " (Prefer ClashMeta Format)";
        }
        return user_agent;
    }

    // preset routing
    Routing::Routing(int preset) : JsonStore() {
        if (preset == 1) {
            direct_ip =
                "geoip:cn\n"
                "geoip:private";
            direct_domain = "geosite:cn";
            proxy_ip = "";
            proxy_domain = "";
            block_ip = "";
            block_domain =
                "geosite:category-ads-all\n"
                "domain:appcenter.ms\n"
                "domain:firebase.io\n"
                "domain:crashlytics.com\n";
        }
        if (!Preset::SingBox::DomainStrategy.contains(domain_strategy)) domain_strategy = "";
        if (!Preset::SingBox::DomainStrategy.contains(outbound_domain_strategy)) outbound_domain_strategy = "";
        _add(new configItem("direct_ip", &this->direct_ip, itemType::string));
        _add(new configItem("direct_domain", &this->direct_domain, itemType::string));
        _add(new configItem("proxy_ip", &this->proxy_ip, itemType::string));
        _add(new configItem("proxy_domain", &this->proxy_domain, itemType::string));
        _add(new configItem("block_ip", &this->block_ip, itemType::string));
        _add(new configItem("block_domain", &this->block_domain, itemType::string));
        _add(new configItem("def_outbound", &this->def_outbound, itemType::string));
        _add(new configItem("custom", &this->custom, itemType::string));
        //
        _add(new configItem("remote_dns", &this->remote_dns, itemType::string));
        _add(new configItem("remote_dns_strategy", &this->remote_dns_strategy, itemType::string));
        _add(new configItem("direct_dns", &this->direct_dns, itemType::string));
        _add(new configItem("direct_dns_strategy", &this->direct_dns_strategy, itemType::string));
        _add(new configItem("domain_strategy", &this->domain_strategy, itemType::string));
        _add(new configItem("outbound_domain_strategy", &this->outbound_domain_strategy, itemType::string));
        _add(new configItem("dns_routing", &this->dns_routing, itemType::boolean));
        _add(new configItem("sniffing_mode", &this->sniffing_mode, itemType::integer));
        _add(new configItem("use_dns_object", &this->use_dns_object, itemType::boolean));
        _add(new configItem("dns_object", &this->dns_object, itemType::string));
        _add(new configItem("dns_final_out", &this->dns_final_out, itemType::string));
    }

    QString Routing::DisplayRouting() const {
        return QStringLiteral("[Proxy] %1\n[Proxy] %2\n[Direct] %3\n[Direct] %4\n[Block] %5\n[Block] %6\n[Default Outbound] %7\n[DNS] %8")
            .arg(SplitLinesSkipSharp(proxy_domain).join(","), 10)
            .arg(SplitLinesSkipSharp(proxy_ip).join(","), 10)
            .arg(SplitLinesSkipSharp(direct_domain).join(","), 10)
            .arg(SplitLinesSkipSharp(direct_ip).join(","), 10)
            .arg(SplitLinesSkipSharp(block_domain).join(","), 10)
            .arg(SplitLinesSkipSharp(block_ip).join(","), 10)
            .arg(def_outbound)
            .arg(use_dns_object ? "DNS Object" : "Simple DNS");
    }

    QStringList Routing::List() {
        QDir dr(ROUTES_PREFIX);
        return dr.entryList(QDir::Files);
    }

    bool Routing::SetToActive(const QString &name) {
        NekoGui::dataStore->routing = std::make_unique<Routing>();
        NekoGui::dataStore->routing->load_control_must = true;
        NekoGui::dataStore->routing->fn = ROUTES_PREFIX + name;
        auto ok = NekoGui::dataStore->routing->Load();
        if (ok) {
            NekoGui::dataStore->active_routing = name;
            NekoGui::dataStore->Save();
        }
        return ok;
    }

    // NO default extra core

    ExtraCore::ExtraCore() : JsonStore() {
        _add(new configItem("core_map", &this->core_map, itemType::string));
    }

    QString ExtraCore::Get(const QString &id) const {
        auto obj = QString2QJsonObject(core_map);
        for (const auto &c: obj.keys()) {
            if (c == id) return obj[id].toString();
        }
        return "";
    }

    void ExtraCore::Set(const QString &id, const QString &path) {
        auto obj = QString2QJsonObject(core_map);
        obj[id] = path;
        core_map = QJsonObject2QString(obj, true);
    }

    void ExtraCore::Delete(const QString &id) {
        auto obj = QString2QJsonObject(core_map);
        obj.remove(id);
        core_map = QJsonObject2QString(obj, true);
    }

    InboundAuthorization::InboundAuthorization() : JsonStore() {
        _add(new configItem("user", &this->username, itemType::string));
        _add(new configItem("pass", &this->password, itemType::string));
    }

    bool InboundAuthorization::NeedAuth() const {
        return !username.trimmed().isEmpty() && !password.trimmed().isEmpty();
    }

    // System Utils

    QString FindCoreAsset(const QString &name) {
        QStringList search{};
        search << QApplication::applicationDirPath();
        search << "/usr/share/sing-geoip";
        search << "/usr/share/sing-geosite";
        search << "/usr/share/sing-box";
        search << "/usr/lib/nekobox";
        search << "/usr/share/nekobox";
        for (const auto &dir: search) {
            if (dir.isEmpty()) continue;
            QFileInfo asset(dir + "/" + name);
            if (asset.exists()) {
                return asset.absoluteFilePath();
            }
        }
        return {};
    }

    QString FindNekoBoxCoreRealPath() {
        auto fn = QApplication::applicationDirPath() + "/nekobox_core";
        auto fi = QFileInfo(fn);
        if (fi.isSymLink()) return fi.symLinkTarget();
        return fn;
    }

    short isAdminCache = -1;

    // IsAdmin 主要判断：有无权限启动 Tun
    bool IsAdmin() {
        if (isAdminCache >= 0) return isAdminCache;

        bool admin = false;
#ifdef Q_OS_WIN
        admin = Windows_IsInAdmin();
#else
#ifdef Q_OS_LINUX
        admin |= Linux_GetCapString(FindNekoBoxCoreRealPath()).contains("cap_net_admin");
#endif
        admin |= geteuid() == 0;
#endif

        isAdminCache = admin;
        return admin;
    };

} // namespace NekoGui

namespace NekoGui {
namespace {
QJsonObject RenumberObject(const QByteArray &bytes, const QString &path) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(bytes, &error);
    Renumber::Require(error.error == QJsonParseError::NoError && document.isObject(), "Invalid JSON: " + path);
    return document.object();
}
QByteArray RenumberJson(const QJsonObject &object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
int RenumberReference(int oldId, const QMap<int, int> &mapping, const QString &description,
                      const QSet<int> &sentinels = {}) {
    if (sentinels.contains(oldId)) return oldId;
    Renumber::Require(mapping.contains(oldId), "Dangling " + description + ": " + QString::number(oldId));
    return mapping.value(oldId);
}
QJsonArray RenumberReferences(const QJsonArray &values, const QMap<int, int> &mapping, const QString &description) {
    QJsonArray result;
    for (const auto &value : values) result.append(RenumberReference(value.toInt(), mapping, description));
    return result;
}
int RenumberNumericFile(const QString &path, const QString &root) {
    if (!path.startsWith(root + "/")) return -1;
    const auto name = path.mid(root.size() + 1);
    if (name.contains('/') || !name.endsWith(".json", Qt::CaseInsensitive)) return -1;
    const auto stem = name.left(name.size() - 5);
    bool ok = false;
    const int id = stem.toInt(&ok);
    if (!ok) {
        const QRegularExpression numeric("^[+-]?[0-9]+$");
        Renumber::Require(!numeric.match(stem).hasMatch(), "Out-of-range numeric filename: " + path);
        return -1;
    }
    Renumber::Require(id >= 0 && name == QString::number(id) + ".json", "Noncanonical numeric filename: " + path);
    return id;
}
void RenumberValidateProfile(const QJsonObject &object, const QString &path) {
    ProxyEntity metadata(nullptr, QString());
    metadata.fn = path;
    Renumber::Require(metadata.Validate(object), "Invalid profile: " + path);
}
void RenumberValidateGroup(const QJsonObject &object, const QString &path) {
    Group group;
    group.fn = path;
    Renumber::Require(group.Validate(object), "Invalid group: " + path);
}
} // namespace
Renumber::Files BuildRenumbered(const Renumber::Files &oldFiles) {
    using Renumber::Require;
    QMap<int, QJsonObject> profiles;
    QMap<int, QJsonObject> groups;
    for (auto it = oldFiles.constBegin(); it != oldFiles.constEnd(); ++it) {
        const int pid = RenumberNumericFile(it.key(), "profiles");
        const int gid = RenumberNumericFile(it.key(), "groups");
        if (pid >= 0) {
            const auto object = RenumberObject(it.value(), it.key());
            RenumberValidateProfile(object, it.key());
            Require(!profiles.contains(pid), "Duplicate profile ID");
            profiles.insert(pid, object);
        } else if (gid >= 0) {
            const auto object = RenumberObject(it.value(), it.key());
            RenumberValidateGroup(object, it.key());
            Require(!groups.contains(gid), "Duplicate group ID");
            groups.insert(gid, object);
        }
    }
    Require(!groups.isEmpty(), "Initialize at least one group before requesting renumbering");
    Require(oldFiles.contains("groups/nekobox.json"), "Initialize settings before requesting renumbering");
    auto settings = RenumberObject(oldFiles.value("groups/nekobox.json"), "groups/nekobox.json");
    DataStore settingsValidator;
    Require(settingsValidator.Validate(settings), "Invalid settings");
    QJsonObject manager;
    if (oldFiles.contains("groups/pm.json")) manager = RenumberObject(oldFiles.value("groups/pm.json"), "groups/pm.json");
    QList<int> unusedTabs;
    JsonStore managerValidator;
    managerValidator._add(new configItem("groups", &unusedTabs, itemType::integerList));
    Require(managerValidator.Validate(manager), "Invalid manager");
    QList<int> tabOrder;
    for (const auto &value : manager.value("groups").toArray()) {
        const int id = value.toInt();
        Require(groups.contains(id), "Dangling group-tab reference");
        if (!tabOrder.contains(id)) tabOrder.append(id);
    }
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it)
        if (!tabOrder.contains(it.key())) tabOrder.append(it.key());
    for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it)
        Require(groups.contains(it.value().value("gid").toInt()), "Profile belongs to a missing group");
    QMap<int, int> groupMap;
    QMap<int, int> profileMap;
    QMap<int, QList<int>> visible;
    int nextGroup = 0;
    int nextProfile = 0;
    for (int gid : tabOrder) {
        groupMap.insert(gid, nextGroup++);
        const auto group = groups.value(gid);
        const auto order = group.value("order").toArray();
        const bool explicitOrder = group.value("order_explicit").toBool(false) || !order.isEmpty();
        if (explicitOrder) {
            for (const auto &value : order) {
                const int pid = value.toInt();
                Require(profiles.contains(pid) && profiles.value(pid).value("gid").toInt() == gid,
                        "Dangling or cross-group order reference");
                if (!visible[gid].contains(pid)) visible[gid].append(pid);
            }
        } else {
            for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it)
                if (it.value().value("gid").toInt() == gid) visible[gid].append(it.key());
        }
        // Number visible profiles first, then every hidden member of this group.
        for (int pid : visible.value(gid)) profileMap.insert(pid, nextProfile++);
        for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it)
            if (it.value().value("gid").toInt() == gid && !profileMap.contains(it.key()))
                profileMap.insert(it.key(), nextProfile++);
    }
    Require(profileMap.size() == profiles.size() && groupMap.size() == groups.size(), "Incomplete identity mapping");
    auto result = oldFiles;
    // Remove all source names before inserting destinations, including overlapping IDs.
    for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it)
        result.remove(QString("profiles/%1.json").arg(it.key()));
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it)
        result.remove(QString("groups/%1.json").arg(it.key()));
    for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it) {
        auto object = it.value();
        const int newId = profileMap.value(it.key());
        object["id"] = newId;
        object["gid"] = groupMap.value(object.value("gid").toInt());
        if (object.value("type").toString() == "chain") {
            auto bean = object.value("bean").toObject();
            bean["list"] = RenumberReferences(bean.value("list").toArray(), profileMap, "chain member");
            object["bean"] = bean;
        }
        // Traffic counters and unknown fields remain attached to the logical profile.
        const auto path = QString("profiles/%1.json").arg(newId);
        Require(!result.contains(path), "Destination profile collision");
        RenumberValidateProfile(object, path);
        result.insert(path, RenumberJson(object));
    }
    for (int oldId : tabOrder) {
        auto object = groups.value(oldId);
        const int newId = groupMap.value(oldId);
        object["id"] = newId;
        object["front_proxy_id"] = RenumberReference(object.value("front_proxy_id").toInt(-1), profileMap,
                                                    "front proxy", QSet<int>{-1});
        QJsonArray order;
        for (int pid : visible.value(oldId)) order.append(profileMap.value(pid));
        object["order"] = order;
        object["order_explicit"] = true;
        // Preserve recovery_required, including explicitly empty visibility.
        const auto path = QString("groups/%1.json").arg(newId);
        Require(!result.contains(path), "Destination group collision");
        RenumberValidateGroup(object, path);
        result.insert(path, RenumberJson(object));
    }
    QJsonArray tabs;
    for (int gid : tabOrder) tabs.append(groupMap.value(gid));
    manager["groups"] = tabs;
    settings["current_group"] = RenumberReference(settings.value("current_group").toInt(0), groupMap,
                                                  "current group", QSet<int>{-1});
    settings["remember_id"] = RenumberReference(settings.value("remember_id").toInt(-1919), profileMap,
                                                "remembered profile", QSet<int>{-1, -1919});
    Require(managerValidator.Validate(manager) && settingsValidator.Validate(settings), "Invalid remapped configuration");
    result.insert("groups/pm.json", RenumberJson(manager));
    result.insert("groups/nekobox.json", RenumberJson(settings));
    return result;
}
void PrepareConfiguration(bool reorder, Renumber::Lease &lease, const Renumber::Hooks &hooks) {
    if (lease.Begin(reorder)) {
        // Recover once, even when the original command line still requests reorder.
        if (Renumber::Pending()) Renumber::Recover(lease, hooks);
        else if (reorder) {
            const auto oldFiles = Renumber::Scan();
            Renumber::Start(lease, oldFiles, BuildRenumbered(oldFiles), hooks);
        }
    }
    Renumber::Require(!Renumber::Pending(), "Recovery did not finish");
    lease.Ready();
    if (hooks.point) hooks.point("startup:ready");
}
bool ReloadConfiguration() {
    if (Renumber::Pending()) return false;
    if (!dataStore->Load()) return false;
    return profileManager->LoadManager();
}
} // namespace NekoGui
