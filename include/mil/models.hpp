#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace mil {

enum class ContentSection {
    Translations,
    ModsTools,
    Cheats,
    SaveGames,
    About,
};

enum class LanguageMode {
    PtBr,
    EnUs,
};

enum class ThemeMode {
    Light,
    Dark,
};

enum class InstalledTitleScanMode {
    Auto,
    Full,
    CatalogProbe,
    Disabled,
};

struct CompatibilityRule {
    std::string minGameVersion;
    std::string maxGameVersion;
    std::vector<std::string> exactGameVersions;
};

struct CatalogVariant {
    std::string id;
    std::string label;
    std::string installTarget;
    std::string assetId;
    std::string assetType;
    std::string contentHash;
    std::string relativePath;
    std::string downloadUrl;
    std::uint64_t size = 0;
    std::string packageVersion;
    std::string contentRevision;
    CompatibilityRule compatibility;
};

struct CatalogEntry {
    std::string id;
    std::string titleId;
    std::string name;
    std::string installTarget;
    std::string intro;
    std::string introPtBr;
    std::string introEnUs;
    std::string summary;
    std::string summaryPtBr;
    std::string summaryEnUs;
    std::string author;
    std::string packageVersion;
    std::string contentRevision;
    std::string language;
    std::vector<std::string> contentTypes;
    std::string assetId;
    std::string assetType;
    std::string contentHash;
    std::string relativePath;
    std::string downloadUrl;
    std::uint64_t size = 0;
    std::string detailsUrl;
    std::string coverUrl;
    std::string iconUrl;
    std::string thumbnailUrl;
    std::vector<std::string> tags;
    CompatibilityRule compatibility;
    std::vector<CatalogVariant> variants;
    ContentSection section = ContentSection::Translations;
    bool featured = false;
};

struct CatalogIndex {
    std::string catalogName;
    std::string catalogRevision;
    std::string channel;
    std::string schemaVersion;
    std::string generatedAt;
    std::string deliveryBaseUrl;
    std::string thumbPackRevision;
    std::string thumbPackAssetId;
    std::string thumbPackAssetType;
    std::string thumbPackRelativePath;
    std::string thumbPackUrl;
    std::string thumbPackSha256;
    std::uint64_t thumbPackSize = 0;
    std::vector<CatalogEntry> entries;
};

struct CheatEntryRecord {
    std::string id;
    std::string title;
    std::string primarySource;
    std::vector<std::string> sources;
    std::string assetId;
    std::string assetType;
    std::vector<std::string> categories;
    std::string contentHash;
    int cheatCount = 0;
    int lineCount = 0;
    std::string relativePath;
    std::string downloadUrl;
    std::vector<std::string> originUrls;
    int priorityRank = 0;
};

struct CheatBuildRecord {
    std::string buildId;
    std::vector<std::string> categories;
    std::string primarySource;
    std::vector<std::string> sources;
    std::string assetId;
    std::string assetType;
    std::string contentHash;
    int cheatCount = 0;
    int lineCount = 0;
    std::string relativePath;
    std::string downloadUrl;
    std::uint64_t size = 0;
    int priorityRank = 0;
    std::vector<CheatEntryRecord> entries;
};

struct CheatTitleRecord {
    std::string titleId;
    std::string name;
    std::vector<CheatBuildRecord> builds;
};

struct CheatsIndex {
    std::string schemaVersion;
    std::string generatedAt;
    std::string generator;
    std::string catalogRevision;
    std::string deliveryBaseUrl;
    std::string cheatsPackRevision;
    std::string cheatsPackAssetId;
    std::string cheatsPackAssetType;
    std::string cheatsPackRelativePath;
    std::string cheatsPackUrl;
    std::string cheatsPackSha256;
    std::uint64_t cheatsPackSize = 0;
    std::vector<std::string> watchedTitleIds;
    std::vector<CheatTitleRecord> titles;
};

struct SaveVariantRecord {
    std::string id;
    std::string label;
    std::string category;
    std::string saveKind;
    std::string layoutType;
    std::string platform;
    std::string author;
    std::string language;
    std::string updatedAt;
    std::string assetId;
    std::string assetType;
    std::string relativePath;
    std::string downloadUrl;
    std::string contentHash;
    std::string sha256;
    std::uint64_t size = 0;
    std::vector<std::string> origins;
};

struct SaveTitleRecord {
    std::string titleId;
    std::string name;
    std::vector<std::string> categories;
    std::vector<SaveVariantRecord> variants;
};

struct SavesIndex {
    std::string schemaVersion;
    std::string generatedAt;
    std::string generator;
    std::string catalogRevision;
    std::string deliveryBaseUrl;
    std::vector<SaveTitleRecord> titles;
};

struct InstalledTitle {
    std::uint64_t applicationId = 0;
    std::string titleIdHex;
    std::string baseTitleIdHex;
    std::string buildIdHex;
    std::string name;
    std::string publisher;
    std::string displayVersion;
    std::string localIconPath;
    std::string sourcePath;
    std::string basePath;
    std::string updatePath;
    std::vector<std::string> dlcPaths;
    std::string fileType;
    std::string emulatorName;
    std::string source;
    std::string lastPlayedUtc;
    std::string playTime;
    bool metadataAvailable = false;
    bool favorite = false;
};

struct InstallReceipt {
    std::string packageId;
    std::string packageVersion;
    std::string titleId;
    std::string installRoot;
    std::string sourceUrl;
    std::string installedAt;
    std::string gameVersion;
    std::string installType;
    std::string backupPath;
    std::string saveKind;
    std::string saveUserId;
    std::string variantId;
    std::vector<std::string> files;
};

struct AppConfig {
    LanguageMode language = LanguageMode::PtBr;
    ThemeMode theme = ThemeMode::Light;
    InstalledTitleScanMode scanMode = InstalledTitleScanMode::Auto;
    std::vector<std::string> catalogUrls;
};

inline std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline std::string Trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

inline std::string FormatTitleId(std::uint64_t value) {
    std::ostringstream stream;
    stream.setf(std::ios::hex, std::ios::basefield);
    stream.setf(std::ios::uppercase);
    stream.width(16);
    stream.fill('0');
    stream << value;
    return stream.str();
}

inline ContentSection ParseSection(const std::string& rawValue) {
    const std::string value = ToLowerAscii(rawValue);
    if (value == "translations" || value == "translation" || value == "dubs" || value == "dub" ||
        value == "traducao" || value == "traducoes") {
        return ContentSection::Translations;
    }
    if (value == "mods" || value == "mod" || value == "tools" || value == "mods-tools") {
        return ContentSection::ModsTools;
    }
    if (value == "cheats" || value == "cheat") {
        return ContentSection::Cheats;
    }
    if (value == "savegames" || value == "save" || value == "saves") {
        return ContentSection::SaveGames;
    }
    return ContentSection::About;
}

inline const char* SectionLabel(ContentSection section) {
    switch (section) {
        case ContentSection::Translations:
            return "Traduções & Dublagens";
        case ContentSection::ModsTools:
            return "Mods";
        case ContentSection::Cheats:
            return "Cheats";
        case ContentSection::SaveGames:
            return "Jogos Salvos";
        case ContentSection::About:
        default:
            return "Sobre a M.I.L.";
    }
}

inline std::vector<int> ParseVersionTokens(const std::string& version) {
    std::vector<int> tokens;
    std::string current;
    for (char ch : version) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            current.push_back(ch);
        } else if (!current.empty()) {
            tokens.push_back(std::stoi(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::stoi(current));
    }
    return tokens;
}

inline int CompareGameVersion(const std::string& left, const std::string& right) {
    const auto leftTokens = ParseVersionTokens(left);
    const auto rightTokens = ParseVersionTokens(right);
    const std::size_t count = std::max(leftTokens.size(), rightTokens.size());
    for (std::size_t index = 0; index < count; ++index) {
        const int lhs = index < leftTokens.size() ? leftTokens[index] : 0;
        const int rhs = index < rightTokens.size() ? rightTokens[index] : 0;
        if (lhs < rhs) {
            return -1;
        }
        if (lhs > rhs) {
            return 1;
        }
    }
    return 0;
}

inline bool MatchesCompatibility(const CompatibilityRule& rule, const std::string& gameVersion) {
    if (gameVersion.empty()) {
        return true;
    }
    if (!rule.exactGameVersions.empty()) {
        return std::find(rule.exactGameVersions.begin(), rule.exactGameVersions.end(), gameVersion) != rule.exactGameVersions.end();
    }
    if (!rule.minGameVersion.empty() && CompareGameVersion(gameVersion, rule.minGameVersion) < 0) {
        return false;
    }
    if (!rule.maxGameVersion.empty() && CompareGameVersion(gameVersion, rule.maxGameVersion) > 0) {
        return false;
    }
    return true;
}

}  // namespace mil
