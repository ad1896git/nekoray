#pragma once

#include <QtGlobal>

#ifdef Q_OS_MACOS
void InstallMacQuitHandler();
#else
inline void InstallMacQuitHandler() {}
#endif
