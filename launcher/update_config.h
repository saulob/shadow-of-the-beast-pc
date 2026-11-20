#pragma once

#include <filesystem>

#include "updater/update_controller.h"

namespace Sotb {

trg::update::UpdateConfig MakeUpdateConfig(const std::filesystem::path& install_dir);

} // namespace Sotb
