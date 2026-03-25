#pragma once

#include <string>

#include "mil/models.hpp"

namespace mil {

bool LoadSavesIndexFromJsonString(const std::string& json, SavesIndex& index, std::string& error);
bool LoadSavesIndexFromFile(const std::string& path, SavesIndex& index, std::string& error);

}  // namespace mil
