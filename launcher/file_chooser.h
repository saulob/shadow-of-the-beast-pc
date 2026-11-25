#pragma once

#include <cstdlib>

namespace Sotb {

inline constexpr char kNoFileChooser[] = "No file chooser (zenity or kdialog) is installed.";

// trg::BrowseForFile cannot tell a missing chooser from a cancelled one.
inline bool FileChooserAvailable() {
#ifdef _WIN32
    return true;
#else
    return std::system("command -v zenity >/dev/null 2>&1 || command -v kdialog >/dev/null 2>&1") == 0;
#endif
}

} // namespace Sotb
