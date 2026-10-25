#pragma once

namespace NekoGui {

template<typename Items, typename LookupProfile, typename RefreshProfile>
void SaveExistingTrafficProfiles(const Items &items,
                                 LookupProfile lookupProfile,
                                 RefreshProfile refreshProfile) {
    for (const auto &item: items) {
        auto profile = lookupProfile(item->id);
        if (!profile) {
            continue;
        }
        profile->Save();
        refreshProfile(item->id);
    }
}

} // namespace NekoGui
