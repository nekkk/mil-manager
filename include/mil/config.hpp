#pragma once

#include <string>

#include "mil/models.hpp"

namespace mil {

constexpr const char* kDefaultCatalogUrl = "https://nekkk.github.io/mil-manager-delivery/index.json";
constexpr const char* kConfigRootDir = "sdmc:/switch/mil_manager";
constexpr const char* kSettingsPath = "sdmc:/switch/mil_manager/settings.ini";
constexpr const char* kReceiptsDir = "sdmc:/switch/mil_manager/cache/receipts";
constexpr const char* kCacheDir = "sdmc:/switch/mil_manager/cache";
constexpr const char* kSaveOpsDir = "sdmc:/switch/mil_manager/cache/save-ops";
constexpr const char* kCatalogCachePath = "sdmc:/switch/mil_manager/cache/index.json";
constexpr const char* kCheatsSummaryCachePath = "sdmc:/switch/mil_manager/cache/cheats-summary.json";
constexpr const char* kCheatsIndexCachePath = "sdmc:/switch/mil_manager/cache/cheats-index.json";
constexpr const char* kSavesIndexCachePath = "sdmc:/switch/mil_manager/cache/saves-index.json";
constexpr const char* kSwitchLocalIndexPath = "sdmc:/switch/mil_manager/cache/index.json";
constexpr const char* kSwitchLocalCheatsSummaryPath = "sdmc:/switch/mil_manager/cache/cheats-summary.json";
constexpr const char* kSwitchLocalCheatsIndexPath = "sdmc:/switch/mil_manager/cache/cheats-index.json";
constexpr const char* kSwitchLocalSavesIndexPath = "sdmc:/switch/mil_manager/cache/saves-index.json";
constexpr const char* kInstalledTitlesCachePath = "sdmc:/switch/mil_manager/cache/installed-titles-cache.json";

AppConfig LoadAppConfig(std::string& note);
bool SaveAppConfig(const AppConfig& config, std::string& error);
const char* LanguageModeLabel(LanguageMode mode);
const char* ThemeModeLabel(ThemeMode mode);
const char* InstalledTitleScanModeLabel(InstalledTitleScanMode mode);

}  // namespace mil
