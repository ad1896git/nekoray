#pragma once

#include "main/NekoGui.hpp"
#include "ProxyEntity.hpp"

namespace NekoGui {
    class Group : public JsonStore {
    public:
        int id = -1;
        bool archive = false;
        bool skip_auto_update = false;
        QString name = "";
        QString url = "";
        QString info = "";
        qint64 sub_last_update = 0;
        int front_proxy_id = -1;

        // list ui
        bool manually_column_width = false;
        QList<int> column_width;
        QList<int> order;
        // Missing in legacy files: an empty order retains legacy fallback.
        bool order_explicit = false;

        // Persisted conservatively across an unfinished update/cleanup.
        bool recoveryRequired = false;

        [[nodiscard]] bool HasExplicitOrder() const {
            return order_explicit || !order.isEmpty();
        }

        Group();

        QJsonObject ToJson(const QStringList &without = {}) override;
        bool Validate(const QJsonObject &object) const override;

        // 按 id 顺序
        [[nodiscard]] QList<std::shared_ptr<ProxyEntity>> Profiles() const;

        // 按 显示 顺序
        [[nodiscard]] QList<std::shared_ptr<ProxyEntity>> ProfilesWithOrder() const;
    };
} // namespace NekoGui
