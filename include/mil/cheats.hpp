#pragma once

#include <string>

#include "mil/models.hpp"

namespace mil {

bool LoadCheatsIndexFromJsonString(const std::string& json, CheatsIndex& index, std::string& error);
bool LoadCheatsIndexFromFile(const std::string& path, CheatsIndex& index, std::string& error);

}  // namespace mil
