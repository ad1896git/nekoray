#pragma once

// UI-thread policy shared by delayed operation dialogs and warning helpers.
// A refused shutdown restores dialogs; successful shutdown keeps them disabled
// through aboutToQuit and dispatcher teardown.
namespace ShutdownDialogs {
inline bool suppressed = false;
}
