// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <functional>
#include <string>

// renders a save directory into a tile directory
using RenderFn = std::function<void(const std::filesystem::path &save, const std::filesystem::path &out)>;

// renders every backup zip in `dir` that isn't in the tile db yet, then watches `dir` for new ones
// each backup becomes one tile-history snapshot, timestamped with the backup's time
void backup_loop(const std::filesystem::path &dir, const std::string &db_path, RenderFn render);
