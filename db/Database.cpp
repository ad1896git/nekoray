#include "Database.hpp"

#include "fmt/includes.h"

#include <QFile>
#include <QDir>
#include <QColor>
#include <QFileInfo>
#include <QJsonObject>

#include <limits>

namespace NekoGui {

    namespace {
        bool PathOccupied(const QString &path) {
            const QFileInfo info(path);
            return info.exists() || info.isSymLink();
        }

        bool StoredId(const QJsonObject &object, const QString &key) {
            const auto value = object.value(key);
            if (!value.isDouble()) return false;
            const int id = value.toInt(-1);
            return id >= 0 && value.toDouble() == double(id);
        }

        bool IdMatchesFile(const QJsonObject &object, const QString &path) {
            if (path.isEmpty()) return true;
            bool ok = false;
            const int fileId = QFileInfo(path).completeBaseName().toInt(&ok);
            return ok && fileId >= 0 && object.value("id").toInt(-1) == fileId;
        }

        int AvailableId(int first, const QString &directory) {
            if (first < 0) return -1;
            for (int id = first; ; ++id) {
                if (!PathOccupied(QStringLiteral("%1/%2.json").arg(directory).arg(id))) {
                    return id;
                }
                if (id == std::numeric_limits<int>::max()) return -1;
            }
        }
    } // namespace


    ProfileManager *profileManager = new ProfileManager();

    ProfileManager::ProfileManager() : JsonStore("groups/pm.json") {
        _add(new configItem("groups", &groupsTabOrder, itemType::integerList));
    }

    QList<int> filterIntJsonFile(const QString &path) {
        QList<int> result;
        QDir dr(path);
        auto entryList = dr.entryList(QDir::Files);
        for (auto e: entryList) {
            e = e.toLower();
            if (!e.endsWith(".json", Qt::CaseInsensitive)) continue;
            e = e.remove(".json", Qt::CaseInsensitive);
            bool ok;
            auto id = e.toInt(&ok);
            if (ok) {
                result << id;
            }
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    bool ProfileManager::LoadManager() {
        const auto previousProfiles = profiles;
        const auto previousGroups = groups;
        const auto previousProfileIds = profilesIdOrder;
        const auto previousGroupIds = groupsIdOrder;
        const auto previousTabs = groupsTabOrder;
        const auto previousCache = last_save_content;
        const auto restore = [&] {
            profiles = previousProfiles;
            groups = previousGroups;
            profilesIdOrder = previousProfileIds;
            groupsIdOrder = previousGroupIds;
            groupsTabOrder = previousTabs;
            last_save_content = previousCache;
        };
        groupsTabOrder.clear();
        const bool managerLoaded = JsonStore::Load();
        if (!managerLoaded && PathOccupied(fn)) {
            restore();
            return false;
        }
        const auto profileFiles = filterIntJsonFile("profiles");
        const auto groupFiles = filterIntJsonFile("groups");
        profiles.clear();
        groups.clear();
        profilesIdOrder.clear();
        groupsIdOrder.clear();
        for (const int id : profileFiles) {
            auto ent = LoadProxyEntity(QStringLiteral("profiles/%1.json").arg(id));
            if (!ent) continue;
            profiles.emplace(id, ent);
            profilesIdOrder.append(id);
        }
        auto loadedOrder = groupsTabOrder;
        groupsTabOrder.clear();
        for (const int id : groupFiles) {
            auto ent = LoadGroup(QStringLiteral("groups/%1.json").arg(id));
            if (!ent) continue;
            groups.emplace(id, ent);
            groupsIdOrder.append(id);
            if (!loadedOrder.contains(id)) loadedOrder.append(id);
        }
        for (const int id : loadedOrder) {
            if (groups.count(id) && !groupsTabOrder.contains(id)) groupsTabOrder.append(id);
        }
        if (groups.empty()) {
            auto defaultGroup = NewGroup();
            defaultGroup->name = QObject::tr("Default");
            if (!AddGroup(defaultGroup)) {
                qWarning() << "Cannot persist the default group";
                restore();
                return false;
            }
        }
        //
        if (dataStore->flag_reorder) {
            {
                // remove all (contains orphan)
                for (const auto &profile: profiles) {
                    QFile::remove(profile.second->fn);
                }
            }
            std::map<int, int> gidOld2New;
            {
                int i = 0;
                int ii = 0;
                QList<int> newProfilesIdOrder;
                std::map<int, std::shared_ptr<ProxyEntity>> newProfiles;
                for (auto gid: groupsTabOrder) {
                    auto group = GetGroup(gid);
                    gidOld2New[gid] = ii++;
                    for (auto const &profile: group->ProfilesWithOrder()) {
                        auto oldId = profile->id;
                        auto newId = i++;
                        profile->id = newId;
                        profile->gid = gidOld2New[gid];
                        profile->fn = QStringLiteral("profiles/%1.json").arg(newId);
                        profile->Save();
                        newProfiles[newId] = profile;
                        newProfilesIdOrder << newId;
                    }
                    group->order = {};
                    group->Save();
                }
                profiles = newProfiles;
                profilesIdOrder = newProfilesIdOrder;
            }
            {
                QList<int> newGroupsIdOrder;
                std::map<int, std::shared_ptr<Group>> newGroups;
                for (auto oldGid: groupsTabOrder) {
                    auto newId = gidOld2New[oldGid];
                    auto group = groups[oldGid];
                    QFile::remove(group->fn);
                    group->id = newId;
                    group->fn = QStringLiteral("groups/%1.json").arg(newId);
                    group->Save();
                    newGroups[newId] = group;
                    newGroupsIdOrder << newId;
                }
                groups = newGroups;
                groupsIdOrder = newGroupsIdOrder;
                groupsTabOrder = newGroupsIdOrder;
            }
            MessageBoxInfo(software_name, "Profiles and groups reorder complete.");
        }
        return true;
    }

    void ProfileManager::SaveManager() {
        JsonStore::Save();
    }

    std::shared_ptr<ProxyEntity> ProfileManager::LoadProxyEntity(const QString &jsonPath) {
        ProxyEntity metadata(nullptr, QString());
        metadata.fn = jsonPath;
        // Validate metadata and the selected bean before applying anything.
        if (!metadata.Load()) return nullptr;
        auto ent = NewProxyEntity(metadata.type);
        ent->fn = jsonPath;
        ent->load_control_must = true;
        // Apply the exact validated bytes without a second filesystem read.
        if (!ent->FromJsonBytes(metadata.last_save_content)) return nullptr;
        ent->last_save_content = metadata.last_save_content;
        return ent;
    }

    //  新建的不给 fn 和 id

    std::shared_ptr<ProxyEntity> ProfileManager::NewProxyEntity(const QString &type) {
        NekoGui_fmt::AbstractBean *bean;

        if (type == "socks") {
            bean = new NekoGui_fmt::SocksHttpBean(NekoGui_fmt::SocksHttpBean::type_Socks5);
        } else if (type == "http") {
            bean = new NekoGui_fmt::SocksHttpBean(NekoGui_fmt::SocksHttpBean::type_HTTP);
        } else if (type == "shadowsocks") {
            bean = new NekoGui_fmt::ShadowSocksBean();
        } else if (type == "chain") {
            bean = new NekoGui_fmt::ChainBean();
        } else if (type == "vmess") {
            bean = new NekoGui_fmt::VMessBean();
        } else if (type == "trojan") {
            bean = new NekoGui_fmt::TrojanVLESSBean(NekoGui_fmt::TrojanVLESSBean::proxy_Trojan);
        } else if (type == "vless") {
            bean = new NekoGui_fmt::TrojanVLESSBean(NekoGui_fmt::TrojanVLESSBean::proxy_VLESS);
        } else if (type == "naive") {
            bean = new NekoGui_fmt::NaiveBean();
        } else if (type == "hysteria2") {
            bean = new NekoGui_fmt::QUICBean(NekoGui_fmt::QUICBean::proxy_Hysteria2);
        } else if (type == "tuic") {
            bean = new NekoGui_fmt::QUICBean(NekoGui_fmt::QUICBean::proxy_TUIC);
        } else if (type == "custom") {
            bean = new NekoGui_fmt::CustomBean();
        } else {
            bean = new NekoGui_fmt::AbstractBean(-114514);
        }

        auto ent = std::make_shared<ProxyEntity>(bean, type);
        return ent;
    }

    std::shared_ptr<Group> ProfileManager::NewGroup() {
        auto ent = std::make_shared<Group>();
        return ent;
    }

    // ProxyEntity

    ProxyEntity::ProxyEntity(NekoGui_fmt::AbstractBean *bean, const QString &type_) {
        if (type_ != nullptr) this->type = type_;

        _add(new configItem("type", &type, itemType::string));
        _add(new configItem("id", &id, itemType::integer));
        _add(new configItem("gid", &gid, itemType::integer));
        _add(new configItem("yc", &latency, itemType::integer));
        _add(new configItem("report", &full_test_report, itemType::string));

        // 可以不关联 bean，只加载 ProxyEntity 的信息
        if (bean != nullptr) {
            this->bean = std::shared_ptr<NekoGui_fmt::AbstractBean>(bean);
            // 有虚函数就要在这里 dynamic_cast
            _add(new configItem("bean", dynamic_cast<JsonStore *>(bean), itemType::jsonStore));
            _add(new configItem("traffic", dynamic_cast<JsonStore *>(traffic_data.get()), itemType::jsonStore));
        }
    };

    bool ProxyEntity::Validate(const QJsonObject &object) const {
        if (!StoredId(object, "id") || !StoredId(object, "gid") ||
            !IdMatchesFile(object, fn) || !object.value("type").isString() ||
            !object.value("bean").isObject()) return false;
        const QString storedType = object.value("type").toString();
        const auto prototype = ProfileManager::NewProxyEntity(storedType);
        if (!prototype || !prototype->bean || prototype->bean->version == -114514) {
            return false;
        }
        if (bean && storedType != type) return false;
        const auto beanObject = object.value("bean").toObject();
        if (!beanObject.value("_v").isDouble() ||
            beanObject.value("_v").toInt(-114514) == -114514) return false;
        // Qualified call validates the selected bean without recursive dispatch here.
        return prototype->JsonStore::Validate(object);
    }

    QString ProxyEntity::DisplayLatency() const {
        if (latency < 0) {
            return QObject::tr("Unavailable");
        } else if (latency > 0) {
            return UNICODE_LRO + QStringLiteral("%1 ms").arg(latency);
        } else {
            return "";
        }
    }

    QColor ProxyEntity::DisplayLatencyColor() const {
        if (latency < 0) {
            return Qt::red;
        } else if (latency > 0) {
            auto greenMs = dataStore->test_latency_url.startsWith("https://") ? 200 : 100;
            if (latency < greenMs) {
                return Qt::darkGreen;
            } else {
                return Qt::darkYellow;
            }
        } else {
            return {};
        }
    }

    // Profile

    int ProfileManager::NewProfileID() const {
        int first = 0;
        if (!profiles.empty()) {
            const int highest = profiles.rbegin()->first;
            if (highest == std::numeric_limits<int>::max()) return -1;
            first = highest + 1;
        }
        return AvailableId(first, "profiles");
    }

    bool ProfileManager::AddProfile(const std::shared_ptr<ProxyEntity> &ent, int gid) {
        if (!ent || ent->id >= 0 || !ent->bean) return false;
        const int newId = NewProfileID();
        if (newId < 0) return false;
        const int previousId = ent->id;
        const int previousGid = ent->gid;
        const QString previousPath = ent->fn;
        ent->gid = gid < 0 ? dataStore->current_group : gid;
        ent->id = newId;
        ent->fn = QStringLiteral("profiles/%1.json").arg(newId);
        if (!ent->SaveChecked().ok) {
            ent->id = previousId;
            ent->gid = previousGid;
            ent->fn = previousPath;
            return false;
        }
        profiles.emplace(newId, ent);
        profilesIdOrder.append(newId);
        return true;
    }

    bool ProfileManager::DeleteProfile(int id) {
        if (id < 0 || dataStore->started_id == id) return false;
        const auto found = profiles.find(id);
        if (found == profiles.end()) return false;
        QFile file(found->second->fn);
        if (!file.remove()) {
            qWarning() << "Cannot delete profile"
                       << id << file.fileName() << file.errorString();
            return false;
        }
        profiles.erase(found);
        profilesIdOrder.removeAll(id);
        return true;
    }

    void ProfileManager::MoveProfile(const std::shared_ptr<ProxyEntity> &ent, int gid) {
        if (gid == ent->gid || gid < 0) return;
        auto oldGroup = GetGroup(ent->gid);
        if (oldGroup != nullptr && !oldGroup->order.isEmpty()) {
            oldGroup->order.removeAll(ent->id);
            oldGroup->Save();
        }
        auto newGroup = GetGroup(gid);
        if (newGroup != nullptr && !newGroup->order.isEmpty()) {
            newGroup->order.push_back(ent->id);
            newGroup->Save();
        }
        ent->gid = gid;
        ent->Save();
    }

    std::shared_ptr<ProxyEntity> ProfileManager::GetProfile(int id) {
        return profiles.count(id) ? profiles[id] : nullptr;
    }

    // Group

    Group::Group() {
        _add(new configItem("id", &id, itemType::integer));
        _add(new configItem("front_proxy_id", &front_proxy_id, itemType::integer));
        _add(new configItem("archive", &archive, itemType::boolean));
        _add(new configItem("skip_auto_update", &skip_auto_update, itemType::boolean));
        _add(new configItem("name", &name, itemType::string));
        _add(new configItem("order", &order, itemType::integerList));
        _add(new configItem("url", &url, itemType::string));
        _add(new configItem("info", &info, itemType::string));
        _add(new configItem("lastup", &sub_last_update, itemType::integer64));
        _add(new configItem("manually_column_width", &manually_column_width, itemType::boolean));
        _add(new configItem("column_width", &column_width, itemType::integerList));
    }

    QJsonObject Group::ToJson(const QStringList &without) {
        auto object = JsonStore::ToJson(without);
        if (!without.contains("name")) object.insert("name", name);
        return object;
    }

    bool Group::Validate(const QJsonObject &object) const {
        return JsonStore::Validate(object) && StoredId(object, "id") &&
               IdMatchesFile(object, fn) && object.value("name").isString();
    }

    std::shared_ptr<Group> ProfileManager::LoadGroup(const QString &jsonPath) {
        auto ent = std::make_shared<Group>();
        ent->fn = jsonPath;
        if (!ent->Load()) return nullptr;
        return ent;
    }

    int ProfileManager::NewGroupID() const {
        int first = 0;
        if (!groups.empty()) {
            const int highest = groups.rbegin()->first;
            if (highest == std::numeric_limits<int>::max()) return -1;
            first = highest + 1;
        }
        return AvailableId(first, "groups");
    }

    bool ProfileManager::AddGroup(const std::shared_ptr<Group> &ent) {
        if (!ent || ent->id >= 0) return false;
        const int newId = NewGroupID();
        if (newId < 0) return false;
        const int previousId = ent->id;
        const QString previousPath = ent->fn;
        ent->id = newId;
        ent->fn = QStringLiteral("groups/%1.json").arg(newId);
        if (!ent->SaveChecked().ok) {
            ent->id = previousId;
            ent->fn = previousPath;
            return false;
        }
        groups.emplace(newId, ent);
        groupsIdOrder.append(newId);
        groupsTabOrder.append(newId);
        return true;
    }

    void ProfileManager::DeleteGroup(int gid) {
        if (groups.size() <= 1) return;
        QList<int> toDelete;
        for (const auto &[id, profile]: profiles) {
            if (profile->gid == gid) toDelete += id; // map访问中，不能操作
        }
        bool childrenDeleted = true;
        for (const auto &id : toDelete) {
            if (!DeleteProfile(id)) childrenDeleted = false;
        }
        if (!childrenDeleted) {
            qWarning() << "Keeping group because a profile could not be deleted" << gid;
            return;
        }
        groups.erase(gid);
        groupsIdOrder.removeAll(gid);
        groupsTabOrder.removeAll(gid);
        QFile(QStringLiteral("groups/%1.json").arg(gid)).remove();
    }

    std::shared_ptr<Group> ProfileManager::GetGroup(int id) {
        return groups.count(id) ? groups[id] : nullptr;
    }

    std::shared_ptr<Group> ProfileManager::CurrentGroup() {
        return GetGroup(dataStore->current_group);
    }

    QList<std::shared_ptr<ProxyEntity>> Group::Profiles() const {
        QList<std::shared_ptr<ProxyEntity>> ret;
        for (const auto &[_, profile]: profileManager->profiles) {
            if (id == profile->gid) ret += profile;
        }
        return ret;
    }

    QList<std::shared_ptr<ProxyEntity>> Group::ProfilesWithOrder() const {
        if (order.isEmpty()) {
            return Profiles();
        } else {
            QList<std::shared_ptr<ProxyEntity>> ret;
            for (auto _id: order) {
                auto ent = profileManager->GetProfile(_id);
                if (ent != nullptr) ret += ent;
            }
            return ret;
        }
    }

} // namespace NekoGui
