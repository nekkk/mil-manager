#pragma once

#include <string>

#include "mil/models.hpp"

namespace mil {

bool LoadCatalogFromJsonString(const std::string& json, CatalogIndex& catalog, std::string& error);
bool LoadCatalogFromFile(const std::string& path, CatalogIndex& catalog, std::string& error);

}  // namespace mil
