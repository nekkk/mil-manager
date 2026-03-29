#include "mil/app.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <switch.h>
#include <mbedtls/sha256.h>

#include "mil/catalog.hpp"
#include "mil/cheats.hpp"
#include "mil/config.hpp"
#include "mil/graphics.hpp"
#include "mil/http.hpp"
#include "mil/installer.hpp"
#include "mil/platform.hpp"
#include "mil/savegames.hpp"
#include "picojson.h"

namespace mil {

namespace {

std::size_t Utf8GlyphCountLocal(const std::string& text) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if ((lead & 0x80u) == 0) {
            ++index;
        } else if ((lead & 0xE0u) == 0xC0u && index + 1 < text.size()) {
            index += 2;
        } else if ((lead & 0xF0u) == 0xE0u && index + 2 < text.size()) {
            index += 3;
        } else if ((lead & 0xF8u) == 0xF0u && index + 3 < text.size()) {
            index += 4;
        } else {
            ++index;
        }
        ++count;
    }
    return count;
}

std::size_t Utf8ByteOffsetForGlyphLocal(const std::string& text, std::size_t glyphIndex) {
    std::size_t currentGlyph = 0;
    std::size_t index = 0;
    while (index < text.size() && currentGlyph < glyphIndex) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if ((lead & 0x80u) == 0) {
            ++index;
        } else if ((lead & 0xE0u) == 0xC0u && index + 1 < text.size()) {
            index += 2;
        } else if ((lead & 0xF0u) == 0xE0u && index + 2 < text.size()) {
            index += 3;
        } else if ((lead & 0xF8u) == 0xF0u && index + 3 < text.size()) {
            index += 4;
        } else {
            ++index;
        }
        ++currentGlyph;
    }
    return index;
}

bool EnsureDirectory(const std::string& path) {
    if (path.empty()) {
        return false;
    }

    std::string current;
    for (std::size_t index = 0; index < path.size(); ++index) {
        current.push_back(path[index]);
        if (path[index] == '/' || path[index] == ':') {
            continue;
        }
        const bool isLast = index + 1 == path.size();
        const bool nextIsSeparator = !isLast && path[index + 1] == '/';
        if (!isLast && !nextIsSeparator) {
            continue;
        }
        mkdir(current.c_str(), 0777);
    }
    return true;
}

bool FileExists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool DirectoryExists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string ReadTextFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return {};
    }
    std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    while (!data.empty() && (data.back() == '\n' || data.back() == '\r')) {
        data.pop_back();
    }
    return data;
}

bool WriteTextFile(const std::string& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return output.good();
}

std::string SanitizePathComponent(std::string value) {
    if (value.empty()) {
        return "default";
    }
    for (char& ch : value) {
        const bool allowed = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                             ch == '-' || ch == '_' || ch == '.';
        if (!allowed) {
            ch = '_';
        }
    }
    return value;
}

int ColorLuma(std::uint32_t color) {
    const int red = static_cast<int>((color >> 0) & 0xFFu);
    const int green = static_cast<int>((color >> 8) & 0xFFu);
    const int blue = static_cast<int>((color >> 16) & 0xFFu);
    return (red * 299 + green * 587 + blue * 114) / 1000;
}

bool FileHasContent(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0;
}

std::string ComputeFileSha256(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return {};
    }

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    mbedtls_sha256_starts_ret(&context, 0);

    char buffer[8192];
    while (input.good()) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize readBytes = input.gcount();
        if (readBytes > 0) {
            mbedtls_sha256_update_ret(&context,
                                      reinterpret_cast<const unsigned char*>(buffer),
                                      static_cast<std::size_t>(readBytes));
        }
    }

    unsigned char digest[32] = {};
    mbedtls_sha256_finish_ret(&context, digest);
    mbedtls_sha256_free(&context);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (unsigned char byte : digest) {
        hex.push_back(kHex[(byte >> 4) & 0x0F]);
        hex.push_back(kHex[byte & 0x0F]);
    }
    return std::string("sha256:") + hex;
}

struct PackManifestInfo {
    std::string revision;
    std::string packUrl;
    std::string packSha256;
};

std::string JsonStringField(const picojson::object& object, const char* key) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->second.is<std::string>()) {
        return {};
    }
    return iterator->second.get<std::string>();
}

bool ParsePackManifestJson(const std::string& json, PackManifestInfo& manifest) {
    picojson::value root;
    const std::string parseError = picojson::parse(root, json);
    if (!parseError.empty() || !root.is<picojson::object>()) {
        return false;
    }
    const picojson::object& object = root.get<picojson::object>();
    manifest.revision = JsonStringField(object, "revision");
    manifest.packUrl = JsonStringField(object, "packUrl");
    manifest.packSha256 = JsonStringField(object, "packSha256");
    return true;
}

bool LoadPackManifestFromFile(const std::string& path, PackManifestInfo& manifest) {
    const std::string json = ReadTextFile(path);
    return !json.empty() && ParsePackManifestJson(json, manifest);
}

void FlattenLanguageJsonValue(const picojson::value& value,
                              const std::string& prefix,
                              std::unordered_map<std::string, std::string>& output) {
    if (value.is<std::string>()) {
        output[prefix] = value.get<std::string>();
        return;
    }
    if (!value.is<picojson::object>()) {
        return;
    }

    const auto& object = value.get<picojson::object>();
    for (const auto& pair : object) {
        const std::string childKey = prefix.empty() ? pair.first : prefix + "." + pair.first;
        FlattenLanguageJsonValue(pair.second, childKey, output);
    }
}

bool LoadLanguageStringsFromFile(const std::string& path,
                                 std::unordered_map<std::string, std::string>& output,
                                 std::string& error) {
    output.clear();
    std::string json = ReadTextFile(path);
    if (json.empty()) {
        error = "Language file not found.";
        return false;
    }
    if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xEF &&
        static_cast<unsigned char>(json[1]) == 0xBB &&
        static_cast<unsigned char>(json[2]) == 0xBF) {
        json.erase(0, 3);
    }

    picojson::value root;
    const std::string parseError = picojson::parse(root, json);
    if (!parseError.empty() || !root.is<picojson::object>()) {
        error = "Invalid language JSON.";
        return false;
    }

    FlattenLanguageJsonValue(root, "", output);
    return true;
}

std::string LanguageJsonPath(LanguageMode mode) {
    switch (mode) {
        case LanguageMode::EnUs:
            return "romfs:/lang/en-US.json";
        case LanguageMode::PtBr:
        default:
            return "romfs:/lang/pt-BR.json";
    }
}

bool DesiredPackAlreadyPresent(const std::string& localManifestPath,
                               const std::string& desiredRevision,
                               const std::string& desiredPackSha256) {
    PackManifestInfo localManifest;
    if (!LoadPackManifestFromFile(localManifestPath, localManifest)) {
        return false;
    }
    if (!desiredPackSha256.empty()) {
        return localManifest.packSha256 == desiredPackSha256;
    }
    if (!desiredRevision.empty()) {
        return localManifest.revision == desiredRevision;
    }
    return false;
}

bool IsUsableThumbnailCacheFile(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (FileHasContent(path)) {
        return true;
    }
    if (FileExists(path)) {
        remove(path.c_str());
    }
    return false;
}

bool ContainsString(const std::vector<std::string>& values, const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

void AppendUniqueString(std::vector<std::string>& values, const std::string& value) {
    if (!ContainsString(values, value)) {
        values.push_back(value);
    }
}

enum class TouchTargetKind {
    None,
    Section,
    Entry,
    ActionButton,
    SearchButton,
    SortButton,
    RefreshButton,
    LanguageButton,
    ThemeButton,
    ConfirmYesButton,
    ConfirmNoButton,
    VariantOption,
    VariantConfirmButton,
    VariantCancelButton,
    CheatBuildOption,
    CheatBuildConfirmButton,
    CheatBuildCancelButton,
};

enum class SortMode {
    Recommended,
    Recent,
    Name,
};

struct TouchTarget {
    TouchTargetKind kind = TouchTargetKind::None;
    int index = -1;
};

struct AppState {
    AppConfig config;
    CatalogIndex catalog;
    std::unordered_map<std::string, std::string> catalogSearchIndex;
    CheatsIndex cheatsIndex;
    std::vector<CatalogEntry> derivedCheatEntries;
    std::unordered_map<std::string, std::string> derivedCheatSearchIndex;
    SavesIndex savesIndex;
    std::vector<CatalogEntry> derivedSaveEntries;
    std::unordered_map<std::string, std::string> derivedSaveSearchIndex;
    std::vector<InstalledTitle> installedTitles;
    std::unordered_map<std::string, std::size_t> installedTitleIndexById;
    std::vector<InstallReceipt> receipts;
    ContentSection section = ContentSection::Translations;
    std::size_t selection = 0;
    std::string activeCatalogSource;
    std::string activeCheatsSource;
    std::string activeSavesSource;
    bool cheatsIndexFiltered = false;
    bool savesIndexLoaded = false;
    std::string statusLine = "Carregando...";
    std::string searchQuery;
    std::string platformNote;
    enum class FocusPane {
        Sections,
        Catalog,
    } focus = FocusPane::Sections;
    SortMode sortMode = SortMode::Recommended;
    bool touchActive = false;
    int lastTouchX = 0;
    int lastTouchY = 0;
    bool exitRequested = false;
    bool exitRequestInFlight = false;
    TouchTarget activeTouchTarget;
    struct ThumbnailFailure {
        std::string id;
        std::uint64_t retryFrame = 0;
    };
    std::vector<ThumbnailFailure> thumbnailFailures;
    std::uint64_t frameCounter = 0;
    std::mutex thumbnailMutex;
    std::thread thumbnailWorker;
    bool thumbnailWorkerEnabled = false;
    bool thumbnailWorkerStop = false;
    bool thumbnailWorkerBusy = false;
    std::string pendingThumbnailId;
    std::string pendingThumbnailUrl;
    std::string pendingThumbnailFallbackUrl;
    std::string pendingThumbnailPath;
    ContentSection thumbnailSelectionSection = ContentSection::Translations;
    std::size_t thumbnailSelectionAnchor = 0;
    std::uint64_t thumbnailSelectionStableSince = 0;
    std::uint64_t nextThumbnailPrefetchFrame = 0;
    bool installConfirmVisible = false;
    std::string installConfirmEntryId;
    std::string installConfirmTitle;
    std::string installConfirmMessage;
    bool variantSelectVisible = false;
    std::string variantSelectEntryId;
    std::string variantSelectTitle;
    std::string variantSelectMessage;
    std::vector<std::string> variantSelectIds;
    std::size_t variantSelectSelection = 0;
    bool cheatBuildSelectVisible = false;
    std::string cheatBuildEntryId;
    std::string cheatBuildTitleId;
    std::string cheatBuildTitle;
    std::string cheatBuildMessage;
    std::vector<std::string> cheatBuildIds;
    std::size_t cheatBuildSelection = 0;
    bool progressVisible = false;
    std::string progressTitle;
    std::string progressDetail;
    int progressPercent = 0;
    std::unordered_map<std::string, std::string> languageStrings;
    bool languageStringsLoaded = false;
    std::vector<const CatalogEntry*> visibleEntriesCache;
    bool visibleEntriesDirty = true;
    PlatformSession* activeSession = nullptr;
};

struct ThemePalette {
    std::uint32_t windowTop;
    std::uint32_t windowBottom;
    std::uint32_t windowBase;
    std::uint32_t sidebarFill;
    std::uint32_t sidebarBorder;
    std::uint32_t sidebarBorderFocused;
    std::uint32_t sidebarHeaderFill;
    std::uint32_t headerText;
    std::uint32_t headerMetaText;
    std::uint32_t sidebarItemFill;
    std::uint32_t sidebarItemBorder;
    std::uint32_t sidebarItemFillSelected;
    std::uint32_t sidebarItemBorderSelected;
    std::uint32_t sidebarItemText;
    std::uint32_t sidebarItemTextSelected;
    std::uint32_t actionFill;
    std::uint32_t actionBorder;
    std::uint32_t actionBorderActive;
    std::uint32_t actionBadgeOuter;
    std::uint32_t actionBadgeOuterActive;
    std::uint32_t actionBadgeInner;
    std::uint32_t actionBadgeText;
    std::uint32_t actionText;
    std::uint32_t statusFill;
    std::uint32_t statusBorder;
    std::uint32_t statusText;
    std::uint32_t contentPanelFill;
    std::uint32_t contentPanelBorder;
    std::uint32_t contentPanelBorderFocused;
    std::uint32_t contentHeaderFill;
    std::uint32_t emptyFill;
    std::uint32_t emptyBorder;
    std::uint32_t entryFill;
    std::uint32_t entryFillSelected;
    std::uint32_t entryBorder;
    std::uint32_t entryBorderSelected;
    std::uint32_t entryAccent;
    std::uint32_t entryAccentSelected;
    std::uint32_t coverFill;
    std::uint32_t coverBorder;
    std::uint32_t detailsFill;
    std::uint32_t detailsBorder;
    std::uint32_t primaryText;
    std::uint32_t secondaryText;
    std::uint32_t mutedText;
    std::uint32_t accentText;
    std::uint32_t warningText;
    std::uint32_t chipSuggestedFill;
    std::uint32_t chipSuggestedText;
    std::uint32_t chipInstalledFill;
    std::uint32_t chipInstalledText;
    std::uint32_t chipNeutralFill;
    std::uint32_t chipNeutralText;
    std::uint32_t buttonPrimaryFill;
    std::uint32_t buttonPrimaryBorder;
    std::uint32_t buttonPrimaryBorderActive;
    std::uint32_t buttonSecondaryFill;
    std::uint32_t buttonSecondaryBorder;
    std::uint32_t buttonTextPrimary;
    std::uint32_t buttonTextSecondary;
    std::uint32_t buttonBadgeOuter;
    std::uint32_t buttonBadgeInnerPrimary;
    std::uint32_t buttonBadgeInnerSecondary;
    std::uint32_t buttonBadgeTextPrimary;
    std::uint32_t buttonBadgeTextSecondary;
    std::uint32_t footerFill;
    std::uint32_t footerBorder;
    std::uint32_t footerText;
};

bool EnsureCheatsIndexReady(AppState& state, bool forceRemoteRefresh = false, bool allowRyujinxRemote = false);
void RefreshDerivedCheatEntries(AppState& state);
void FilterCheatsIndexToDetectedTitles(AppState& state);
void PopulateDetectedCheatBuildIds(AppState& state, bool allowNetworkFallback = true);
bool EntryUsesCheatsIndex(const AppState& state, const CatalogEntry& entry);
bool EnsureSavesIndexReady(AppState& state, bool forceRemoteRefresh = false, bool allowRyujinxRemote = false);
void RefreshDerivedSaveEntries(AppState& state);
bool EntryUsesSavesIndex(const AppState& state, const CatalogEntry& entry);
std::string UiText(const AppState& state, const char* ptBr, const char* enUs);
std::string UiString(const AppState& state, const char* key, const char* ptBr, const char* enUs);
bool UseEnglish(const AppState& state);
const CheatTitleRecord* FindCheatTitleRecord(const CheatsIndex& index, const std::string& titleId);
const CheatBuildRecord* FindCheatBuildRecord(const CheatTitleRecord& title, const std::string& buildId);
const SaveTitleRecord* FindSaveTitleRecord(const SavesIndex& index, const std::string& titleId);
bool IsRyujinxGuestEnvironment();
std::string ResolveThumbPackUrl(const CatalogIndex& catalog, const std::string& catalogSource);
std::string ResolveCheatsPackUrl(const CheatsIndex& index, const std::string& cheatsIndexSource);
std::string ResolveCatalogEntryDownloadUrl(const AppState& state, const CatalogEntry& entry);
std::string ResolveCatalogVariantDownloadUrl(const AppState& state, const CatalogVariant& variant);
std::string ResolveCheatBuildDownloadUrl(const AppState& state, const CheatBuildRecord& build);
std::string ResolveCheatEntryDownloadUrl(const AppState& state, const CheatEntryRecord& entry);
std::string ResolveSaveVariantDownloadUrl(const AppState& state, const SaveVariantRecord& variant);
const std::vector<const CatalogEntry*>& BuildVisibleEntries(AppState& state);
void RenderUi(PlatformSession& session, const AppState& state, const std::vector<const CatalogEntry*>& items);

void PumpUi(AppState& state) {
    if (state.activeSession == nullptr) {
        return;
    }
    RenderUi(*state.activeSession, state, BuildVisibleEntries(state));
}

void SetProgress(AppState& state, const std::string& title, const std::string& detail, int percent) {
    state.progressVisible = true;
    state.progressTitle = title;
    state.progressDetail = detail;
    state.progressPercent = std::max(0, std::min(100, percent));
    PumpUi(state);
}

void ClearProgress(AppState& state) {
    state.progressVisible = false;
    state.progressTitle.clear();
    state.progressDetail.clear();
    state.progressPercent = 0;
}

struct UiDownloadProgressContext {
    AppState* state = nullptr;
    std::string title;
    std::string labelPt;
    std::string labelEn;
    int basePercent = 0;
    int spanPercent = 100;
    int lastPercent = -1;
    std::uint64_t lastBytes = 0;
    std::chrono::steady_clock::time_point lastUpdate = std::chrono::steady_clock::now();
};

std::string FormatByteCount(std::uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(bytes);
    std::size_t unitIndex = 0;
    while (value >= 1024.0 && unitIndex + 1 < (sizeof(kUnits) / sizeof(kUnits[0]))) {
        value /= 1024.0;
        ++unitIndex;
    }

    std::ostringstream stream;
    if (unitIndex == 0 || value >= 10.0) {
        stream << static_cast<int>(value + 0.5);
    } else {
        stream.setf(std::ios::fixed);
        stream.precision(1);
        stream << value;
    }
    stream << ' ' << kUnits[unitIndex];
    return stream.str();
}

void UpdateUiDownloadProgress(std::uint64_t downloadedBytes, std::uint64_t totalBytes, bool totalKnown, void* userData) {
    auto* context = reinterpret_cast<UiDownloadProgressContext*>(userData);
    if (context == nullptr || context->state == nullptr) {
        return;
    }

    AppState& state = *context->state;
    int percent = context->basePercent;
    if (totalKnown && totalBytes > 0) {
        const std::uint64_t clampedDownloaded = std::min(downloadedBytes, totalBytes);
        percent += static_cast<int>((clampedDownloaded * static_cast<std::uint64_t>(context->spanPercent)) / totalBytes);
    }
    percent = std::max(0, std::min(100, percent));

    const auto now = std::chrono::steady_clock::now();
    const bool finished = totalKnown && totalBytes > 0 && downloadedBytes >= totalBytes;
    const bool shouldRender = context->lastPercent < 0 ||
                              percent != context->lastPercent ||
                              downloadedBytes != context->lastBytes ||
                              finished ||
                              (now - context->lastUpdate) >= std::chrono::milliseconds(150);
    if (!shouldRender) {
        return;
    }

    context->lastPercent = percent;
    context->lastBytes = downloadedBytes;
    context->lastUpdate = now;

    std::string detail = UseEnglish(state) ? context->labelEn : context->labelPt;
    detail += ' ';
    detail += FormatByteCount(downloadedBytes);
    if (totalKnown && totalBytes > 0) {
        detail += " / ";
        detail += FormatByteCount(totalBytes);
    }

    SetProgress(state, context->title, detail, percent);
}

constexpr int kSidebarX = 24;
constexpr int kSidebarY = 28;
constexpr int kSidebarWidth = 270;
constexpr int kSidebarHeight = 664;
constexpr int kSidebarSectionX = kSidebarX + 16;
constexpr int kSidebarSectionY = kSidebarY + 126;
constexpr int kSidebarSectionWidth = kSidebarWidth - 32;
constexpr int kSidebarSectionHeight = 52;
constexpr int kSidebarSectionGap = 10;

constexpr int kEntryListX = 316;
constexpr int kEntryListY = 28;
constexpr int kEntryListWidth = 540;
constexpr int kEntryListHeight = 610;
constexpr int kEntryCardX = kEntryListX + 18;
constexpr int kEntryCardY = kEntryListY + 96;
constexpr int kEntryCardWidth = kEntryListWidth - 36;
constexpr int kEntryCardHeight = 118;
constexpr int kEntryCardGap = 8;
constexpr int kEntryListTopOffset = 96;
constexpr int kEntryListBottomPadding = 14;
constexpr const char* kThumbnailCacheDir = "sdmc:/switch/mil_manager/cache/images/";
constexpr const char* kThumbPackRootDir = "sdmc:/switch/mil_manager/cache/thumb-packs";
constexpr const char* kThumbPackRevisionPath = "sdmc:/switch/mil_manager/cache/thumb-pack-revision.txt";
constexpr const char* kThumbPackZipPath = "sdmc:/switch/mil_manager/cache/thumbs-pack.zip";
constexpr const char* kCheatPackRootDir = "sdmc:/switch/mil_manager/cache/cheat-packs";
constexpr const char* kCheatPackRevisionPath = "sdmc:/switch/mil_manager/cache/cheat-pack-revision.txt";
constexpr const char* kCheatPackZipPath = "sdmc:/switch/mil_manager/cache/cheats-pack.zip";
constexpr const char* kCheatsSummaryTempPath = "sdmc:/switch/mil_manager/cache/cheats-summary.json.tmp";
constexpr const char* kCheatsIndexTempPath = "sdmc:/switch/mil_manager/cache/cheats-index.json.tmp";
constexpr const char* kSavesIndexTempPath = "sdmc:/switch/mil_manager/cache/saves-index.json.tmp";

constexpr int kDetailsX = 878;
constexpr int kDetailsY = 28;
constexpr int kDetailsWidth = 378;
constexpr int kDetailsHeight = 610;
constexpr int kDetailsActionX = kDetailsX + 20;
constexpr int kDetailsActionY = kDetailsY + kDetailsHeight - 58;
constexpr int kDetailsActionWidth = kDetailsWidth - 40;
constexpr int kDetailsActionHeight = 46;

constexpr int kFooterY = 648;
constexpr int kFooterHeight = 44;
constexpr int kSidebarInfoX = kSidebarX + 16;
constexpr int kSidebarInfoWidth = kSidebarWidth - 32;
constexpr int kSidebarCardGap = 10;
constexpr int kSidebarCardWidth = (kSidebarInfoWidth - kSidebarCardGap) / 2;
constexpr int kSidebarActionCardHeight = 34;
constexpr int kSidebarStatusCardHeight = 24;
constexpr int kSidebarActionRowGap = 8;
constexpr int kSidebarStatusRowGap = 6;
constexpr int kSidebarActionCardY = kSidebarY + kSidebarHeight - 164;
constexpr int kSidebarStatusCardY = kSidebarActionCardY + (kSidebarActionCardHeight * 3) + (kSidebarActionRowGap * 2) + 10;
constexpr int kInstallButtonX = kSidebarInfoX;
constexpr int kExitButtonX = kSidebarInfoX + kSidebarCardWidth + kSidebarCardGap;
constexpr int kRefreshButtonX = kSidebarInfoX;
constexpr int kSortButtonX = kSidebarInfoX + kSidebarCardWidth + kSidebarCardGap;
constexpr int kLanguageButtonX = kSidebarInfoX;
constexpr int kThemeButtonX = kSidebarInfoX + kSidebarCardWidth + kSidebarCardGap;
constexpr int kInstallButtonY = kSidebarActionCardY;
constexpr int kExitButtonY = kSidebarActionCardY;
constexpr int kRefreshButtonY = kSidebarActionCardY + kSidebarActionCardHeight + kSidebarActionRowGap;
constexpr int kSortButtonY = kSidebarActionCardY + kSidebarActionCardHeight + kSidebarActionRowGap;
constexpr int kLanguageButtonY = kSidebarActionCardY + (kSidebarActionCardHeight + kSidebarActionRowGap) * 2;
constexpr int kThemeButtonY = kSidebarActionCardY + (kSidebarActionCardHeight + kSidebarActionRowGap) * 2;
constexpr int kInstallConfirmDialogWidth = 420;
constexpr int kInstallConfirmDialogHeight = 198;
constexpr int kVariantSelectDialogWidth = 460;
constexpr int kVariantSelectRowHeight = 34;
constexpr int kVariantSelectRowGap = 8;
constexpr int kVariantSelectButtonsHeight = 40;
constexpr int kCheatBuildDialogWidth = 460;
constexpr int kCheatBuildRowHeight = 34;
constexpr int kCheatBuildRowGap = 8;
constexpr int kCheatBuildButtonsHeight = 40;
constexpr int kCheatBuildDialogMaxVisibleRows = 5;

int InstallConfirmDialogX(int canvasWidth) {
    return (canvasWidth - kInstallConfirmDialogWidth) / 2;
}

int InstallConfirmDialogY(int canvasHeight) {
    return (canvasHeight - kInstallConfirmDialogHeight) / 2;
}

int InstallConfirmDialogX(const gfx::Canvas& canvas) {
    return InstallConfirmDialogX(canvas.width);
}

int InstallConfirmDialogY(const gfx::Canvas& canvas) {
    return InstallConfirmDialogY(canvas.height);
}

int VariantSelectDialogHeight(const AppState& state) {
    const int rowCount = std::max(1, static_cast<int>(state.variantSelectIds.size()));
    const int rowsHeight = rowCount * kVariantSelectRowHeight + std::max(0, rowCount - 1) * kVariantSelectRowGap;
    return 62 + std::max(18, gfx::MeasureWrappedTextHeight(state.variantSelectMessage, kVariantSelectDialogWidth - 36, 1, 3)) +
           12 + rowsHeight + 14 + kVariantSelectButtonsHeight + 16;
}

int VariantSelectDialogX(int canvasWidth) {
    return (canvasWidth - kVariantSelectDialogWidth) / 2;
}

int VariantSelectDialogY(int canvasHeight, const AppState& state) {
    return (canvasHeight - VariantSelectDialogHeight(state)) / 2;
}

int VariantSelectDialogX(const gfx::Canvas& canvas) {
    return VariantSelectDialogX(canvas.width);
}

int VariantSelectDialogY(const gfx::Canvas& canvas, const AppState& state) {
    return VariantSelectDialogY(canvas.height, state);
}

int VariantSelectDialogMessageHeight(const AppState& state) {
    return std::max(18, gfx::MeasureWrappedTextHeight(state.variantSelectMessage, kVariantSelectDialogWidth - 36, 1, 3));
}

int VariantSelectDialogRowStartOffset(const AppState& state) {
    return 62 + VariantSelectDialogMessageHeight(state) + 12;
}

int CheatBuildDialogMessageHeight(const AppState& state) {
    return std::max(18, gfx::MeasureWrappedTextHeight(state.cheatBuildMessage, kCheatBuildDialogWidth - 36, 1, 3));
}

int CheatBuildDialogRowStartOffset(const AppState& state) {
    return 62 + CheatBuildDialogMessageHeight(state) + 12;
}

int CheatBuildDialogRowsHeight(const AppState& state) {
    const int rowCount =
        std::max(1, std::min(kCheatBuildDialogMaxVisibleRows, static_cast<int>(state.cheatBuildIds.size())));
    return rowCount * kCheatBuildRowHeight + std::max(0, rowCount - 1) * kCheatBuildRowGap;
}

int CheatBuildDialogHeight(const AppState& state) {
    return CheatBuildDialogRowStartOffset(state) + CheatBuildDialogRowsHeight(state) + 14 + kCheatBuildButtonsHeight + 16;
}

int CheatBuildDialogX(int canvasWidth) {
    return (canvasWidth - kCheatBuildDialogWidth) / 2;
}

int CheatBuildDialogY(int canvasHeight, const AppState& state) {
    return (canvasHeight - CheatBuildDialogHeight(state)) / 2;
}

int CheatBuildDialogX(const gfx::Canvas& canvas) {
    return CheatBuildDialogX(canvas.width);
}

int CheatBuildDialogY(const gfx::Canvas& canvas, const AppState& state) {
    return CheatBuildDialogY(canvas.height, state);
}

std::pair<std::size_t, std::size_t> GetCheatBuildVisibleWindow(const AppState& state) {
    if (state.cheatBuildIds.empty()) {
        return {0, 0};
    }

    const std::size_t itemCount = state.cheatBuildIds.size();
    const std::size_t visibleCount = std::min<std::size_t>(itemCount, kCheatBuildDialogMaxVisibleRows);
    const std::size_t halfWindow = visibleCount / 2;
    std::size_t windowStart = state.cheatBuildSelection > halfWindow ? state.cheatBuildSelection - halfWindow : 0;
    if (windowStart + visibleCount > itemCount) {
        windowStart = itemCount > visibleCount ? itemCount - visibleCount : 0;
    }
    return {windowStart, std::min(itemCount, windowStart + visibleCount)};
}

const InstalledTitle* FindInstalledTitle(const std::vector<InstalledTitle>& titles, const std::string& titleId) {
    const std::string normalized = ToLowerAscii(titleId);
    for (const InstalledTitle& title : titles) {
        if (ToLowerAscii(title.titleIdHex) == normalized) {
            return &title;
        }
    }
    return nullptr;
}

InstalledTitle* FindInstalledTitleMutable(std::vector<InstalledTitle>& titles, const std::string& titleId) {
    const std::string normalized = ToLowerAscii(titleId);
    for (InstalledTitle& title : titles) {
        if (ToLowerAscii(title.titleIdHex) == normalized) {
            return &title;
        }
    }
    return nullptr;
}

const InstalledTitle* FindInstalledTitle(const AppState& state, const std::string& titleId) {
    const auto it = state.installedTitleIndexById.find(ToLowerAscii(titleId));
    if (it == state.installedTitleIndexById.end() || it->second >= state.installedTitles.size()) {
        return nullptr;
    }
    return &state.installedTitles[it->second];
}

const CatalogEntry* FindCheatCatalogOverride(const CatalogIndex& catalog, const std::string& titleId) {
    const std::string normalized = ToLowerAscii(titleId);
    for (const CatalogEntry& entry : catalog.entries) {
        if (entry.section == ContentSection::Cheats && ToLowerAscii(entry.titleId) == normalized) {
            return &entry;
        }
    }
    return nullptr;
}

const CatalogEntry* FindAnyCatalogEntryByTitleId(const CatalogIndex& catalog, const std::string& titleId) {
    const std::string normalized = ToLowerAscii(titleId);
    for (const CatalogEntry& entry : catalog.entries) {
        if (ToLowerAscii(entry.titleId) == normalized) {
            return &entry;
        }
    }
    return nullptr;
}

std::string NormalizeCheatDisplayName(std::string name) {
    const std::pair<const char*, const char*> replacements[] = {
        {"â„¢", "TM"},
        {"â€¢", "•"},
        {"â€“", "-"},
        {"â€”", "-"},
        {"â€˜", "'"},
        {"â€™", "'"},
        {"â€œ", "\""},
        {"â€\x9d", "\""},
        {"Ã¡", "á"},
        {"Ã¢", "â"},
        {"Ã£", "ã"},
        {"Ã¤", "ä"},
        {"Ã§", "ç"},
        {"Ã©", "é"},
        {"Ãª", "ê"},
        {"Ã­", "í"},
        {"Ã³", "ó"},
        {"Ã´", "ô"},
        {"Ãµ", "õ"},
        {"Ãº", "ú"},
        {"Ã‰", "É"},
        {"Ã“", "Ó"},
        {"Ãš", "Ú"},
    };
    for (const auto& replacement : replacements) {
        std::size_t pos = 0;
        while ((pos = name.find(replacement.first, pos)) != std::string::npos) {
            name.replace(pos, std::char_traits<char>::length(replacement.first), replacement.second);
            pos += std::char_traits<char>::length(replacement.second);
        }
    }
    const std::string suffix = " - Cheats";
    if (name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix) {
        name.erase(name.size() - suffix.size());
    }
    return name;
}

void CopyCheatVisualMetadata(CatalogEntry& target, const CatalogEntry& source) {
    if (target.name.empty()) {
        target.name = source.name;
    }
    if (target.iconUrl.empty()) {
        target.iconUrl = source.iconUrl;
    }
    if (target.coverUrl.empty()) {
        target.coverUrl = source.coverUrl;
    }
    if (target.thumbnailUrl.empty()) {
        target.thumbnailUrl = source.thumbnailUrl;
    }
    if (target.detailsUrl.empty()) {
        target.detailsUrl = source.detailsUrl;
    }
}

std::string NormalizeSearchQuery(std::string value) {
    return ToLowerAscii(Trim(value));
}

std::string BuildCatalogSearchText(const CatalogEntry& entry) {
    std::string text = ToLowerAscii(entry.name) + "\n" + ToLowerAscii(entry.titleId);
    for (const std::string& tag : entry.tags) {
        if (!tag.empty()) {
            text += "\n";
            text += ToLowerAscii(tag);
        }
    }
    for (const std::string& contentType : entry.contentTypes) {
        if (!contentType.empty()) {
            text += "\n";
            text += ToLowerAscii(contentType);
        }
    }
    return text;
}

void RefreshCatalogSearchIndex(AppState& state) {
    state.catalogSearchIndex.clear();
    state.catalogSearchIndex.reserve(state.catalog.entries.size());
    for (const CatalogEntry& entry : state.catalog.entries) {
        state.catalogSearchIndex[entry.id] = BuildCatalogSearchText(entry);
    }
}

void RefreshInstalledTitleIndex(AppState& state) {
    state.installedTitleIndexById.clear();
    state.installedTitleIndexById.reserve(state.installedTitles.size());
    for (std::size_t index = 0; index < state.installedTitles.size(); ++index) {
        state.installedTitleIndexById[ToLowerAscii(state.installedTitles[index].titleIdHex)] = index;
    }
}

void InvalidateVisibleEntries(AppState& state) {
    state.visibleEntriesDirty = true;
    state.visibleEntriesCache.clear();
}

std::string BuildCheatSearchText(const CatalogEntry& entry, const CheatTitleRecord* cheatTitle) {
    std::string text = ToLowerAscii(entry.name) + "\n" + ToLowerAscii(entry.titleId);
    if (cheatTitle != nullptr) {
        for (const CheatBuildRecord& build : cheatTitle->builds) {
            text += "\n";
            text += ToLowerAscii(build.buildId);
            for (const std::string& category : build.categories) {
                text += "\n";
                text += ToLowerAscii(category);
            }
            if (!build.primarySource.empty()) {
                text += "\n";
                text += ToLowerAscii(build.primarySource);
            }
        }
    }
    return text;
}

std::string BuildSaveSearchText(const CatalogEntry& entry, const SaveTitleRecord* saveTitle) {
    std::string text = ToLowerAscii(entry.name) + "\n" + ToLowerAscii(entry.titleId);
    if (saveTitle != nullptr) {
        for (const SaveVariantRecord& variant : saveTitle->variants) {
            if (!variant.label.empty()) {
                text += "\n";
                text += ToLowerAscii(variant.label);
            }
            if (!variant.category.empty()) {
                text += "\n";
                text += ToLowerAscii(variant.category);
            }
        }
    }
    return text;
}

const std::string* FindDerivedSearchText(const AppState& state, const CatalogEntry& entry) {
    const auto* index = &state.catalogSearchIndex;
    if (entry.section == ContentSection::Cheats) {
        index = &state.derivedCheatSearchIndex;
    } else if (entry.section == ContentSection::SaveGames) {
        index = &state.derivedSaveSearchIndex;
    }
    const auto it = index->find(entry.id);
    if (it == index->end()) {
        return nullptr;
    }
    return &it->second;
}

bool EntryMatchesSearch(const AppState& state, const CatalogEntry& entry) {
    const std::string query = NormalizeSearchQuery(state.searchQuery);
    if (query.empty()) {
        return true;
    }

    if (const std::string* indexed = FindDerivedSearchText(state, entry)) {
        return indexed->find(query) != std::string::npos;
    }

    const std::string name = ToLowerAscii(entry.name);
    const std::string titleId = ToLowerAscii(entry.titleId);
    if (name.find(query) != std::string::npos || titleId.find(query) != std::string::npos) {
        return true;
    }

    if (entry.section == ContentSection::Cheats) {
        const CheatTitleRecord* cheatTitle = FindCheatTitleRecord(state.cheatsIndex, entry.titleId);
        if (cheatTitle != nullptr) {
            for (const CheatBuildRecord& build : cheatTitle->builds) {
                if (ToLowerAscii(build.buildId).find(query) != std::string::npos) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool CheatEntryHasExactBuildMatch(const AppState& state, const CatalogEntry& entry, const InstalledTitle* installedTitle) {
    if (entry.section != ContentSection::Cheats || !EntryUsesCheatsIndex(state, entry) || installedTitle == nullptr ||
        installedTitle->buildIdHex.empty()) {
        return false;
    }

    const CheatTitleRecord* cheatTitle = FindCheatTitleRecord(state.cheatsIndex, entry.titleId);
    return cheatTitle != nullptr && FindCheatBuildRecord(*cheatTitle, installedTitle->buildIdHex) != nullptr;
}

bool EntryIsSuggested(const AppState& state, const CatalogEntry& entry) {
    const InstalledTitle* installedTitle = FindInstalledTitle(state, entry.titleId);
    if (entry.section == ContentSection::Cheats) {
        return CheatEntryHasExactBuildMatch(state, entry, installedTitle);
    }
    return installedTitle != nullptr;
}

bool ShouldShowCheatEntryByDefault(const AppState& state, const CatalogEntry& entry) {
    if (entry.section != ContentSection::Cheats) {
        return true;
    }
    return FindInstalledTitle(state, entry.titleId) != nullptr;
}

bool ShouldShowSaveEntryByDefault(const AppState& state, const CatalogEntry& entry) {
    if (entry.section != ContentSection::SaveGames) {
        return true;
    }
    return FindInstalledTitle(state, entry.titleId) != nullptr;
}

bool OpenSearchDialog(AppState& state) {
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0))) {
        state.statusLine = UiText(state, "Falha ao abrir pesquisa.", "Failed to open search.");
        return false;
    }

    swkbdConfigMakePresetDefault(&keyboard);
    std::string guideText = UiText(state, "Pesquisar por nome, ID ou build", "Search by name, ID or build");
    if (state.section == ContentSection::SaveGames) {
        guideText = UiText(state, u8"Pesquisar por nome ou ID do título", "Search by name or title ID");
    }
    swkbdConfigSetGuideText(&keyboard, guideText.c_str());
    swkbdConfigSetHeaderText(&keyboard, state.config.language == LanguageMode::EnUs ? "Search" : "Pesquisar");
    swkbdConfigSetInitialText(&keyboard, state.searchQuery.c_str());
    swkbdConfigSetStringLenMax(&keyboard, 64);

    char buffer[65] = {};
    const Result rc = swkbdShow(&keyboard, buffer, sizeof(buffer));
    swkbdClose(&keyboard);
    if (R_FAILED(rc)) {
        return false;
    }

    state.searchQuery = Trim(buffer);
    state.selection = 0;
    InvalidateVisibleEntries(state);
    if (state.section == ContentSection::Cheats) {
        EnsureCheatsIndexReady(state, false);
        if (NormalizeSearchQuery(state.searchQuery).empty() && !state.cheatsIndexFiltered && !state.cheatsIndex.titles.empty()) {
            FilterCheatsIndexToDetectedTitles(state);
            RefreshDerivedCheatEntries(state);
        }
    } else if (state.section == ContentSection::SaveGames) {
        EnsureSavesIndexReady(state, false);
    }
    if (state.searchQuery.empty()) {
        state.statusLine = UiText(state, "Pesquisa limpa.", "Search cleared.");
    } else {
        state.statusLine = std::string(UiText(state, "Pesquisa: ", "Search: ")) + state.searchQuery;
    }
    return true;
}

void RefreshDerivedCheatEntries(AppState& state) {
    state.derivedCheatEntries.clear();
    state.derivedCheatSearchIndex.clear();

    std::vector<std::string> seenTitleIds;
    for (const CheatTitleRecord& cheatTitle : state.cheatsIndex.titles) {
        CatalogEntry entry;
        const CatalogEntry* overrideEntry = FindCheatCatalogOverride(state.catalog, cheatTitle.titleId);
        const CatalogEntry* metadataEntry = FindAnyCatalogEntryByTitleId(state.catalog, cheatTitle.titleId);

        if (overrideEntry != nullptr) {
            entry = *overrideEntry;
        } else if (metadataEntry != nullptr) {
            CopyCheatVisualMetadata(entry, *metadataEntry);
        }

        entry.section = ContentSection::Cheats;
        entry.titleId = ToLowerAscii(cheatTitle.titleId);
        entry.id = !entry.id.empty() ? entry.id : ("cheats-" + ToLowerAscii(cheatTitle.titleId));

        const std::string resolvedName = NormalizeCheatDisplayName(cheatTitle.name);
        if (!resolvedName.empty()) {
            entry.name = resolvedName;
        } else if (entry.name.empty()) {
            entry.name = cheatTitle.titleId;
        }

        if (entry.summaryPtBr.empty()) {
            entry.summaryPtBr = "Cheats agregados automaticamente a partir de fontes online.";
        }
        if (entry.summaryEnUs.empty()) {
            entry.summaryEnUs = "Cheats aggregated automatically from online sources.";
        }
        entry.summary = entry.summaryPtBr;

        if (entry.introPtBr.empty()) {
            entry.introPtBr = "Instalação por build detectado do jogo ou escolha manual.";
        }
        if (entry.introEnUs.empty()) {
            entry.introEnUs = "Installation by detected game build or manual selection.";
        }
        entry.intro = entry.introPtBr;

        if (entry.author.empty()) {
            entry.author = "M.I.L.";
        }
        if (entry.packageVersion.empty()) {
            entry.packageVersion = state.cheatsIndex.catalogRevision;
        }
        if (entry.contentRevision.empty()) {
            entry.contentRevision = !state.cheatsIndex.catalogRevision.empty() ? state.cheatsIndex.catalogRevision
                                                                              : state.cheatsIndex.generatedAt;
        }
        if (entry.contentTypes.empty()) {
            entry.contentTypes = {"cheat"};
        }
        if (!ContainsString(entry.tags, "cheat")) {
            entry.tags.push_back("cheat");
        }

        state.derivedCheatEntries.push_back(entry);
        state.derivedCheatSearchIndex[entry.id] = BuildCheatSearchText(entry, &cheatTitle);
        AppendUniqueString(seenTitleIds, entry.titleId);
    }

    for (const CatalogEntry& entry : state.catalog.entries) {
        if (entry.section != ContentSection::Cheats) {
            continue;
        }
        if (ContainsString(seenTitleIds, ToLowerAscii(entry.titleId))) {
            continue;
        }
        state.derivedCheatEntries.push_back(entry);
        state.derivedCheatSearchIndex[entry.id] = BuildCheatSearchText(entry, nullptr);
    }
    InvalidateVisibleEntries(state);
}

void RefreshDerivedSaveEntries(AppState& state) {
    state.derivedSaveEntries.clear();
    state.derivedSaveSearchIndex.clear();

    std::vector<std::string> seenTitleIds;
    for (const SaveTitleRecord& saveTitle : state.savesIndex.titles) {
        CatalogEntry entry;
        const CatalogEntry* overrideEntry = nullptr;
        const CatalogEntry* metadataEntry = FindAnyCatalogEntryByTitleId(state.catalog, saveTitle.titleId);

        for (const CatalogEntry& candidate : state.catalog.entries) {
            if (candidate.section == ContentSection::SaveGames &&
                ToLowerAscii(candidate.titleId) == ToLowerAscii(saveTitle.titleId)) {
                overrideEntry = &candidate;
                break;
            }
        }

        if (overrideEntry != nullptr) {
            entry = *overrideEntry;
        } else if (metadataEntry != nullptr) {
            CopyCheatVisualMetadata(entry, *metadataEntry);
        }

        entry.section = ContentSection::SaveGames;
        entry.titleId = ToLowerAscii(saveTitle.titleId);
        entry.id = !entry.id.empty() ? entry.id : ("saves-" + ToLowerAscii(saveTitle.titleId));

        if (!saveTitle.name.empty()) {
            entry.name = saveTitle.name;
        } else if (entry.name.empty()) {
            entry.name = saveTitle.titleId;
        }

        if (entry.summaryPtBr.empty()) {
            entry.summaryPtBr = u8"Backups de save agregados automaticamente a partir de repositórios públicos.";
        }
        if (entry.summaryEnUs.empty()) {
            entry.summaryEnUs = "Save backups aggregated automatically from public repositories.";
        }
        entry.summary = entry.summaryPtBr;

        if (entry.introPtBr.empty()) {
            entry.introPtBr = u8"Backup automático do save atual e aplicação direta da variante selecionada.";
        }
        if (entry.introEnUs.empty()) {
            entry.introEnUs = "Automatic backup of the current save and direct application of the selected variant.";
        }
        entry.intro = entry.introPtBr;

        if (entry.author.empty()) {
            std::vector<std::string> origins;
            for (const SaveVariantRecord& variant : saveTitle.variants) {
                if (!variant.author.empty()) {
                    AppendUniqueString(origins, variant.author);
                }
                for (const std::string& origin : variant.origins) {
                    AppendUniqueString(origins, origin);
                }
            }
            if (origins.empty()) {
                entry.author = "M.I.L.";
            } else {
                entry.author.clear();
                for (std::size_t index = 0; index < origins.size(); ++index) {
                    if (index > 0) {
                        entry.author += ", ";
                    }
                    entry.author += origins[index];
                }
            }
        }

        if (entry.contentRevision.empty()) {
            for (const SaveVariantRecord& variant : saveTitle.variants) {
                if (!variant.updatedAt.empty() &&
                    (entry.contentRevision.empty() || variant.updatedAt > entry.contentRevision)) {
                    entry.contentRevision = variant.updatedAt;
                }
            }
        }

        if (entry.contentTypes.empty()) {
            entry.contentTypes = {"save"};
        }
        if (!ContainsString(entry.tags, "save")) {
            entry.tags.push_back("save");
        }

        entry.variants.clear();
        for (const SaveVariantRecord& variantRecord : saveTitle.variants) {
            CatalogVariant variant;
            variant.id = variantRecord.id;
            variant.label = variantRecord.label;
            variant.assetId = variantRecord.assetId;
            variant.assetType = variantRecord.assetType;
            variant.contentHash = !variantRecord.contentHash.empty() ? variantRecord.contentHash : variantRecord.sha256;
            variant.relativePath = variantRecord.relativePath;
            variant.downloadUrl = variantRecord.downloadUrl;
            variant.size = variantRecord.size;
            variant.contentRevision = variantRecord.updatedAt;
            entry.variants.push_back(std::move(variant));
        }

        state.derivedSaveEntries.push_back(std::move(entry));
        state.derivedSaveSearchIndex[state.derivedSaveEntries.back().id] =
            BuildSaveSearchText(state.derivedSaveEntries.back(), &saveTitle);
        AppendUniqueString(seenTitleIds, ToLowerAscii(saveTitle.titleId));
    }

    for (const CatalogEntry& entry : state.catalog.entries) {
        if (entry.section != ContentSection::SaveGames) {
            continue;
        }
        if (ContainsString(seenTitleIds, ToLowerAscii(entry.titleId))) {
            continue;
        }
        state.derivedSaveEntries.push_back(entry);
        state.derivedSaveSearchIndex[entry.id] = BuildSaveSearchText(entry, nullptr);
    }
    InvalidateVisibleEntries(state);
}

bool ShouldFilterCheatsIndexForCurrentView(const AppState& state) {
    return state.section == ContentSection::Cheats && NormalizeSearchQuery(state.searchQuery).empty();
}

void FilterCheatsIndexToDetectedTitles(AppState& state) {
    if (!ShouldFilterCheatsIndexForCurrentView(state) || state.cheatsIndex.titles.empty()) {
        state.cheatsIndexFiltered = false;
        InvalidateVisibleEntries(state);
        return;
    }

    std::vector<CheatTitleRecord> filteredTitles;
    filteredTitles.reserve(state.cheatsIndex.titles.size());
    for (const CheatTitleRecord& title : state.cheatsIndex.titles) {
        if (FindInstalledTitle(state, title.titleId) != nullptr) {
            filteredTitles.push_back(title);
        }
    }

    if (filteredTitles.size() < state.cheatsIndex.titles.size()) {
        state.cheatsIndex.titles = std::move(filteredTitles);
        state.cheatsIndexFiltered = true;
    } else {
        state.cheatsIndexFiltered = false;
    }
    InvalidateVisibleEntries(state);
}

void PopulateDetectedCheatBuildIds(AppState& state, bool allowNetworkFallback) {
    if (state.cheatsIndex.titles.empty() || state.installedTitles.empty()) {
        return;
    }

    for (InstalledTitle& title : state.installedTitles) {
        if (!title.buildIdHex.empty()) {
            continue;
        }
        if (FindCheatTitleRecord(state.cheatsIndex, title.titleIdHex) == nullptr) {
            continue;
        }

        std::string resolveNote;
        TryResolveInstalledTitleBuildId(title, allowNetworkFallback, resolveNote);
    }
}

bool EntryHasVariants(const CatalogEntry& entry);
std::vector<const CatalogVariant*> CollectAllVariants(const CatalogEntry& entry);
std::vector<const CatalogVariant*> FindCompatibleVariants(const CatalogEntry& entry, const InstalledTitle* installedTitle);
bool EntryMatchesInstalledVersion(const CatalogEntry& entry, const InstalledTitle* installedTitle);
bool EntryHasAnyCompatibilityInformation(const CatalogEntry& entry);
std::string VariantDisplayLabel(const CatalogVariant& variant);
std::string VariantListSummary(const CatalogEntry& entry);
const CatalogVariant* FindVariantById(const CatalogEntry& entry, const std::string& variantId);
CatalogEntry ResolveEntryForVariant(const CatalogEntry& entry, const CatalogVariant* variant);

bool ShouldShowProofCover(const CatalogEntry& entry) {
    const std::string normalizedTitleId = ToLowerAscii(entry.titleId);
    const std::string normalizedId = ToLowerAscii(entry.id);
    return normalizedTitleId == "0100b6e00a420000" || normalizedId == "dust-ptbr";
}

const std::vector<const CatalogEntry*>& BuildVisibleEntries(AppState& state) {
    if (!state.visibleEntriesDirty) {
        return state.visibleEntriesCache;
    }

    struct PreparedEntry {
        const CatalogEntry* entry = nullptr;
        std::string sortName;
        std::string recentKey;
        bool suggested = false;
        bool detected = false;
    };

    std::vector<PreparedEntry> prepared;
    const std::vector<CatalogEntry>* sourceEntries = &state.catalog.entries;
    if (state.section == ContentSection::Cheats) {
        sourceEntries = &state.derivedCheatEntries;
    } else if (state.section == ContentSection::SaveGames) {
        sourceEntries = &state.derivedSaveEntries;
    }

    const std::string query = NormalizeSearchQuery(state.searchQuery);
    for (const CatalogEntry& entry : *sourceEntries) {
        if (entry.section != state.section) {
            continue;
        }
        if (!EntryMatchesSearch(state, entry)) {
            continue;
        }
        if ((state.section == ContentSection::Cheats || state.section == ContentSection::SaveGames) && query.empty()) {
            const bool showByDefault = state.section == ContentSection::Cheats ? ShouldShowCheatEntryByDefault(state, entry)
                                                                              : ShouldShowSaveEntryByDefault(state, entry);
            if (!showByDefault) {
                continue;
            }
        }

        const InstalledTitle* installedTitle = FindInstalledTitle(state, entry.titleId);
        PreparedEntry item;
        item.entry = &entry;
        item.sortName = ToLowerAscii(entry.name);
        item.recentKey = entry.contentRevision;
        item.detected = installedTitle != nullptr;
        item.suggested = entry.section == ContentSection::Cheats ? CheatEntryHasExactBuildMatch(state, entry, installedTitle)
                                                                 : installedTitle != nullptr;
        prepared.push_back(std::move(item));
    }

    std::sort(prepared.begin(), prepared.end(), [&](const PreparedEntry& left, const PreparedEntry& right) {
        switch (state.sortMode) {
            case SortMode::Name:
                if (left.sortName != right.sortName) {
                    return left.sortName < right.sortName;
                }
                break;
            case SortMode::Recent:
                if (left.recentKey != right.recentKey) {
                    return left.recentKey > right.recentKey;
                }
                if (left.entry->featured != right.entry->featured) {
                    return left.entry->featured > right.entry->featured;
                }
                if (left.suggested != right.suggested) {
                    return left.suggested > right.suggested;
                }
                break;
            case SortMode::Recommended:
            default:
                if (left.suggested != right.suggested) {
                    return left.suggested > right.suggested;
                }
                if (left.detected != right.detected) {
                    return left.detected > right.detected;
                }
                if (left.entry->featured != right.entry->featured) {
                    return left.entry->featured > right.entry->featured;
                }
                break;
        }

        if (left.sortName != right.sortName) {
            return left.sortName < right.sortName;
        }
        return left.entry->id < right.entry->id;
    });

    state.visibleEntriesCache.clear();
    state.visibleEntriesCache.reserve(prepared.size());
    for (const PreparedEntry& item : prepared) {
        state.visibleEntriesCache.push_back(item.entry);
    }
    state.visibleEntriesDirty = false;
    return state.visibleEntriesCache;
}

std::string ThumbnailCachePathForEntry(const CatalogEntry& entry) {
    if (entry.id.empty()) {
        return {};
    }
    return std::string(kThumbnailCacheDir) + entry.id + ".img";
}

std::string ThumbPackDirectoryForCatalog(const CatalogIndex& catalog) {
    if (catalog.thumbPackRevision.empty()) {
        return {};
    }
    return std::string(kThumbPackRootDir) + "/" + SanitizePathComponent(catalog.thumbPackRevision);
}

std::string ThumbPackPathForEntry(const CatalogIndex& catalog, const CatalogEntry& entry) {
    if (entry.id.empty()) {
        return {};
    }
    const std::string directory = ThumbPackDirectoryForCatalog(catalog);
    if (directory.empty()) {
        return {};
    }
    return directory + "/" + entry.id + ".png";
}

bool EnsureThumbPackCache(const CatalogIndex& catalog,
                          const std::string& catalogSource,
                          std::string& error,
                          bool allowRemoteDownload = false,
                          UiDownloadProgressContext* progressContext = nullptr) {
    const std::string packUrl = ResolveThumbPackUrl(catalog, catalogSource);
    if (catalog.thumbPackRevision.empty() || packUrl.empty()) {
        return true;
    }

    EnsureDirectory(kCacheDir);
    EnsureDirectory(kThumbPackRootDir);

    const std::string revision = SanitizePathComponent(catalog.thumbPackRevision);
    const std::string packDir = std::string(kThumbPackRootDir) + "/" + revision;
    const std::string packManifestPath = packDir + "/manifest.json";
    const std::string currentRevision = ReadTextFile(kThumbPackRevisionPath);
    if (FileHasContent(packManifestPath) &&
        (!allowRemoteDownload || DesiredPackAlreadyPresent(packManifestPath, catalog.thumbPackRevision, catalog.thumbPackSha256))) {
        if (currentRevision != revision) {
            WriteTextFile(kThumbPackRevisionPath, revision + "\n");
        }
        return true;
    }

    if (!allowRemoteDownload) {
        error = "Thumb pack ausente no cache local.";
        return false;
    }

    HttpDownloadOptions options;
    options.connectTimeoutMs = 6000;
    options.requestTimeoutMs = 30000;
    options.retryCount = 2;
    options.probeDownloadInfo = false;
    options.allowResume = false;

    std::size_t downloadedBytes = 0;
    if (!HttpDownloadToFileWithOptions(packUrl, kThumbPackZipPath, options, &downloadedBytes, error) ||
        downloadedBytes == 0) {
        remove(kThumbPackZipPath);
        if (error.empty()) {
            error = "Thumb pack download failed.";
        }
        return false;
    }

    if (!catalog.thumbPackSha256.empty()) {
        const std::string downloadedHash = ComputeFileSha256(kThumbPackZipPath);
        if (downloadedHash.empty() || downloadedHash != catalog.thumbPackSha256) {
            remove(kThumbPackZipPath);
            error = "Thumb pack hash mismatch.";
            return false;
        }
    }

    std::vector<std::string> extractedFiles;
    if (!ExtractZipToDirectory(kThumbPackZipPath, packDir, &extractedFiles, error)) {
        remove(kThumbPackZipPath);
        return false;
    }

    remove(kThumbPackZipPath);
    if (!FileHasContent(packManifestPath)) {
        error = "Thumb pack manifest missing after extraction.";
        return false;
    }
    if (!WriteTextFile(kThumbPackRevisionPath, revision + "\n")) {
        error = "Failed to store thumb pack revision.";
        return false;
    }
    return true;
}

std::string CheatPackDirectoryForIndex(const CheatsIndex& index) {
    if (index.cheatsPackRevision.empty()) {
        return {};
    }
    return std::string(kCheatPackRootDir) + "/" + SanitizePathComponent(index.cheatsPackRevision);
}

std::string CheatPackPathForEntry(const CheatsIndex& index, const CheatEntryRecord& entry) {
    if (entry.relativePath.empty()) {
        return {};
    }
    const std::string directory = CheatPackDirectoryForIndex(index);
    if (directory.empty()) {
        return {};
    }
    return directory + "/" + entry.relativePath;
}

bool EnsureCheatPackCache(const CheatsIndex& index,
                          const std::string& cheatsIndexSource,
                          std::string& error,
                          bool allowRemoteDownload = false,
                          UiDownloadProgressContext* progressContext = nullptr) {
    const std::string packUrl = ResolveCheatsPackUrl(index, cheatsIndexSource);
    if (index.cheatsPackRevision.empty() || packUrl.empty()) {
        return true;
    }

    EnsureDirectory(kCacheDir);
    EnsureDirectory(kCheatPackRootDir);

    const std::string revision = SanitizePathComponent(index.cheatsPackRevision);
    const std::string packDir = std::string(kCheatPackRootDir) + "/" + revision;
    const std::string packManifestPath = packDir + "/manifest.json";
    const std::string currentRevision = ReadTextFile(kCheatPackRevisionPath);
    if (FileHasContent(packManifestPath) &&
        (!allowRemoteDownload || DesiredPackAlreadyPresent(packManifestPath, index.cheatsPackRevision, index.cheatsPackSha256))) {
        if (currentRevision != revision) {
            WriteTextFile(kCheatPackRevisionPath, revision + "\n");
        }
        return true;
    }

    if (!allowRemoteDownload) {
        error = "Cheat pack ausente no cache local.";
        return false;
    }

    HttpDownloadOptions options;
    options.connectTimeoutMs = 6000;
    options.requestTimeoutMs = 30000;
    options.retryCount = 2;
    options.probeDownloadInfo = false;
    options.allowResume = false;
    options.progressCallback = progressContext != nullptr ? UpdateUiDownloadProgress : nullptr;
    options.progressUserData = progressContext;
    options.progressCallback = progressContext != nullptr ? UpdateUiDownloadProgress : nullptr;
    options.progressUserData = progressContext;

    std::size_t downloadedBytes = 0;
    if (!HttpDownloadToFileWithOptions(packUrl, kCheatPackZipPath, options, &downloadedBytes, error) ||
        downloadedBytes == 0) {
        remove(kCheatPackZipPath);
        if (error.empty()) {
            error = "Cheat pack download failed.";
        }
        return false;
    }

    if (!index.cheatsPackSha256.empty()) {
        const std::string downloadedHash = ComputeFileSha256(kCheatPackZipPath);
        if (downloadedHash.empty() || downloadedHash != index.cheatsPackSha256) {
            remove(kCheatPackZipPath);
            error = "Cheat pack hash mismatch.";
            return false;
        }
    }

    std::vector<std::string> extractedFiles;
    if (!ExtractZipToDirectory(kCheatPackZipPath, packDir, &extractedFiles, error)) {
        remove(kCheatPackZipPath);
        return false;
    }

    remove(kCheatPackZipPath);
    if (!FileHasContent(packManifestPath)) {
        error = "Cheat pack manifest missing after extraction.";
        return false;
    }
    if (!WriteTextFile(kCheatPackRevisionPath, revision + "\n")) {
        error = "Failed to store cheat pack revision.";
        return false;
    }
    return true;
}

std::string CachedThumbnailPathForEntry(const AppState& state, const CatalogEntry& entry) {
    const std::string packPath = ThumbPackPathForEntry(state.catalog, entry);
    if (IsUsableThumbnailCacheFile(packPath)) {
        return packPath;
    }
    const std::string path = ThumbnailCachePathForEntry(entry);
    if (IsUsableThumbnailCacheFile(path)) {
        return path;
    }
    return {};
}

std::string PreferredThumbnailPathForEntry(const AppState& state, const CatalogEntry& entry) {
    const InstalledTitle* installedTitle = FindInstalledTitle(state, entry.titleId);
    if (installedTitle != nullptr && !installedTitle->localIconPath.empty() && FileExists(installedTitle->localIconPath)) {
        return installedTitle->localIconPath;
    }
    return CachedThumbnailPathForEntry(state, entry);
}

std::size_t GetVisibleEntryCountForPanelHeight(int panelHeight) {
    const int availableHeight = std::max(0, panelHeight - kEntryListTopOffset - kEntryListBottomPadding);
    const int perCard = kEntryCardHeight + kEntryCardGap;
    if (perCard <= 0) {
        return 1;
    }
    return static_cast<std::size_t>(std::max(1, (availableHeight + kEntryCardGap) / perCard));
}

std::pair<std::size_t, std::size_t> GetVisibleEntryWindow(std::size_t itemCount, std::size_t selection, int panelHeight) {
    if (itemCount == 0) {
        return {0, 0};
    }

    const std::size_t visibleCount = std::min(itemCount, GetVisibleEntryCountForPanelHeight(panelHeight));
    const std::size_t halfWindow = visibleCount / 2;
    std::size_t windowStart = selection > halfWindow ? selection - halfWindow : 0;
    if (windowStart + visibleCount > itemCount) {
        windowStart = itemCount > visibleCount ? itemCount - visibleCount : 0;
    }
    return {windowStart, std::min(itemCount, windowStart + visibleCount)};
}

bool HasThumbnailFailure(AppState& state, const std::string& entryId) {
    const std::lock_guard<std::mutex> lock(state.thumbnailMutex);
    for (const auto& failure : state.thumbnailFailures) {
        if (failure.id == entryId && state.frameCounter < failure.retryFrame) {
            return true;
        }
    }
    return false;
}

void ClearThumbnailFailures(AppState& state) {
    const std::lock_guard<std::mutex> lock(state.thumbnailMutex);
    state.thumbnailFailures.clear();
}

void NoteThumbnailFailure(AppState& state, const std::string& entryId) {
    const std::uint64_t retryDelay = IsEmulatorEnvironment() ? 900 : 180;
    const std::lock_guard<std::mutex> lock(state.thumbnailMutex);
    for (auto& failure : state.thumbnailFailures) {
        if (failure.id == entryId) {
            failure.retryFrame = state.frameCounter + retryDelay;
            return;
        }
    }
    state.thumbnailFailures.push_back({entryId, state.frameCounter + retryDelay});
}

void ThumbnailWorkerMain(AppState* state) {
    HttpDownloadOptions options;
    options.connectTimeoutMs = 5000;
    options.requestTimeoutMs = 20000;
    options.retryCount = 2;
    options.probeDownloadInfo = false;
    options.allowResume = false;

    while (true) {
        std::string entryId;
        std::string url;
        std::string fallbackUrl;
        std::string cachePath;

        {
            const std::lock_guard<std::mutex> lock(state->thumbnailMutex);
            if (state->thumbnailWorkerStop) {
                break;
            }
            if (!state->pendingThumbnailUrl.empty()) {
                entryId = std::move(state->pendingThumbnailId);
                url = std::move(state->pendingThumbnailUrl);
                fallbackUrl = std::move(state->pendingThumbnailFallbackUrl);
                cachePath = std::move(state->pendingThumbnailPath);
                state->pendingThumbnailId.clear();
                state->pendingThumbnailUrl.clear();
                state->pendingThumbnailFallbackUrl.clear();
                state->pendingThumbnailPath.clear();
                state->thumbnailWorkerBusy = true;
            }
        }

        if (url.empty()) {
            svcSleepThread(50'000'000);
            continue;
        }

        EnsureDirectory(kThumbnailCacheDir);
        std::size_t downloadedBytes = 0;
        std::string error;
        bool downloaded =
            HttpDownloadToFileWithOptions(url, cachePath, options, &downloadedBytes, error) && downloadedBytes > 0;
        if (!downloaded && !fallbackUrl.empty() && fallbackUrl != url) {
            remove(cachePath.c_str());
            downloadedBytes = 0;
            error.clear();
            downloaded =
                HttpDownloadToFileWithOptions(fallbackUrl, cachePath, options, &downloadedBytes, error) && downloadedBytes > 0;
        }
        if (!downloaded) {
            remove(cachePath.c_str());
            NoteThumbnailFailure(*state, entryId);
            const std::lock_guard<std::mutex> lock(state->thumbnailMutex);
            state->thumbnailWorkerBusy = false;
        } else {
            const std::lock_guard<std::mutex> lock(state->thumbnailMutex);
            state->thumbnailWorkerBusy = false;
        }
    }
}

void PrefetchVisibleThumbnail(AppState& state, const std::vector<const CatalogEntry*>& items) {
    if (items.empty()) {
        return;
    }

    if (!state.thumbnailWorkerEnabled) {
        return;
    }

    const bool emulator = GetRuntimeEnvironment() == RuntimeEnvironment::Emulator;
    if (emulator) {
        return;
    }
    const std::size_t clampedSelection = std::min(state.selection, items.size() - 1);
    if (state.thumbnailSelectionSection != state.section || state.thumbnailSelectionAnchor != clampedSelection) {
        state.thumbnailSelectionSection = state.section;
        state.thumbnailSelectionAnchor = clampedSelection;
        state.thumbnailSelectionStableSince = state.frameCounter;
        return;
    }

    const std::uint64_t settleFrames = emulator ? 18 : 10;
    if (state.frameCounter < state.thumbnailSelectionStableSince + settleFrames ||
        state.frameCounter < state.nextThumbnailPrefetchFrame) {
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(state.thumbnailMutex);
        if (state.thumbnailWorkerBusy || !state.pendingThumbnailUrl.empty()) {
            return;
        }
    }

    const auto window = GetVisibleEntryWindow(items.size(), clampedSelection, kEntryListHeight);
    const std::size_t windowStart = window.first;
    const std::size_t windowEnd = window.second;

    std::vector<const CatalogEntry*> candidates;
    candidates.push_back(items[clampedSelection]);
    for (std::size_t index = windowStart; index < windowEnd; ++index) {
        if (index == clampedSelection) {
            continue;
        }
        candidates.push_back(items[index]);
    }

    for (const CatalogEntry* candidate : candidates) {
        const CatalogEntry& entry = *candidate;
        const InstalledTitle* installedTitle = FindInstalledTitle(state, entry.titleId);
        if (installedTitle != nullptr && !installedTitle->localIconPath.empty() && FileExists(installedTitle->localIconPath)) {
            continue;
        }
        const std::string primaryUrl =
            !entry.thumbnailUrl.empty() ? entry.thumbnailUrl : (!entry.coverUrl.empty() ? entry.coverUrl : entry.iconUrl);
        if (primaryUrl.empty() || HasThumbnailFailure(state, entry.id)) {
            continue;
        }

        const std::string cachePath = ThumbnailCachePathForEntry(entry);
        if (cachePath.empty() || IsUsableThumbnailCacheFile(cachePath)) {
            continue;
        }

        const std::string fallbackUrl = !entry.coverUrl.empty() && entry.coverUrl != primaryUrl
                                            ? entry.coverUrl
                                            : (!entry.iconUrl.empty() && entry.iconUrl != primaryUrl ? entry.iconUrl
                                                                                                      : std::string());

        {
            const std::lock_guard<std::mutex> lock(state.thumbnailMutex);
            if (state.thumbnailWorkerBusy || !state.pendingThumbnailUrl.empty()) {
                return;
            }
            state.pendingThumbnailId = entry.id;
            state.pendingThumbnailUrl = primaryUrl;
            state.pendingThumbnailFallbackUrl = fallbackUrl;
            state.pendingThumbnailPath = cachePath;
        }
        state.nextThumbnailPrefetchFrame = state.frameCounter + (emulator ? 60 : 10);
        return;
    }
}

bool PointInRect(int x, int y, int rectX, int rectY, int rectWidth, int rectHeight) {
    return x >= rectX && y >= rectY && x < rectX + rectWidth && y < rectY + rectHeight;
}

bool CanUseExitControl() {
    return GetRuntimeEnvironment() == RuntimeEnvironment::Console;
}

bool IsHomebrewLoaderEnvironment() {
    const std::string loaderInfo = ToLowerAscii(GetLoaderInfoSummary());
    return loaderInfo.find("hbmenu") != std::string::npos ||
           loaderInfo.find("hbl") != std::string::npos ||
           loaderInfo.find("nx-hbloader") != std::string::npos;
}

TouchTarget HitTestTouchTarget(const AppState& state, const std::vector<const CatalogEntry*>& items, int touchX, int touchY) {
    if (state.installConfirmVisible) {
        const int dialogX = InstallConfirmDialogX(1280);
        const int dialogY = InstallConfirmDialogY(720);
        const int buttonWidth = (kInstallConfirmDialogWidth - 48) / 2;
        if (PointInRect(touchX, touchY, dialogX + 18, dialogY + kInstallConfirmDialogHeight - 58, buttonWidth, 40)) {
            return {TouchTargetKind::ConfirmYesButton, 0};
        }
        if (PointInRect(touchX,
                        touchY,
                        dialogX + kInstallConfirmDialogWidth / 2 + 6,
                        dialogY + kInstallConfirmDialogHeight - 58,
                        buttonWidth,
                        40)) {
            return {TouchTargetKind::ConfirmNoButton, 0};
        }
        return {};
    }

    if (state.variantSelectVisible) {
        const int dialogX = VariantSelectDialogX(1280);
        const int dialogY = VariantSelectDialogY(720, state);
        const int dialogHeight = VariantSelectDialogHeight(state);
        const int buttonWidth = (kVariantSelectDialogWidth - 48) / 2;
        const int buttonY = dialogY + dialogHeight - 56;
        const int rowStartY = dialogY + 86;
        for (std::size_t index = 0; index < state.variantSelectIds.size(); ++index) {
            const int rowY = rowStartY + static_cast<int>(index) * (kVariantSelectRowHeight + kVariantSelectRowGap);
            if (PointInRect(touchX, touchY, dialogX + 18, rowY, kVariantSelectDialogWidth - 36, kVariantSelectRowHeight)) {
                return {TouchTargetKind::VariantOption, static_cast<int>(index)};
            }
        }
        if (PointInRect(touchX, touchY, dialogX + 18, buttonY, buttonWidth, kVariantSelectButtonsHeight)) {
            return {TouchTargetKind::VariantConfirmButton, 0};
        }
        if (PointInRect(touchX,
                        touchY,
                        dialogX + kVariantSelectDialogWidth / 2 + 6,
                        buttonY,
                        buttonWidth,
                        kVariantSelectButtonsHeight)) {
            return {TouchTargetKind::VariantCancelButton, 0};
        }
        return {};
    }

    if (state.cheatBuildSelectVisible) {
        const int dialogX = CheatBuildDialogX(1280);
        const int dialogY = CheatBuildDialogY(720, state);
        const int dialogHeight = CheatBuildDialogHeight(state);
        const int buttonWidth = (kCheatBuildDialogWidth - 48) / 2;
        const int rowStartY = dialogY + CheatBuildDialogRowStartOffset(state);
        const int buttonY = rowStartY + CheatBuildDialogRowsHeight(state) + 14;
        const auto [windowStart, windowEnd] = GetCheatBuildVisibleWindow(state);
        for (std::size_t index = windowStart; index < windowEnd; ++index) {
            const int rowIndex = static_cast<int>(index - windowStart);
            const int rowY = rowStartY + rowIndex * (kCheatBuildRowHeight + kCheatBuildRowGap);
            if (PointInRect(touchX, touchY, dialogX + 18, rowY, kCheatBuildDialogWidth - 36, kCheatBuildRowHeight)) {
                return {TouchTargetKind::CheatBuildOption, static_cast<int>(index)};
            }
        }
        if (PointInRect(touchX, touchY, dialogX + 18, buttonY, buttonWidth, kCheatBuildButtonsHeight)) {
            return {TouchTargetKind::CheatBuildConfirmButton, 0};
        }
        if (PointInRect(touchX,
                        touchY,
                        dialogX + kCheatBuildDialogWidth / 2 + 6,
                        buttonY,
                        buttonWidth,
                        kCheatBuildButtonsHeight)) {
            return {TouchTargetKind::CheatBuildCancelButton, 0};
        }
        return {};
    }

    const std::vector<ContentSection> sections = {
        ContentSection::Translations,
        ContentSection::ModsTools,
        ContentSection::Cheats,
        ContentSection::SaveGames,
        ContentSection::About,
    };

    for (std::size_t index = 0; index < sections.size(); ++index) {
        const int itemY = kSidebarSectionY + static_cast<int>(index) * (kSidebarSectionHeight + kSidebarSectionGap);
        if (PointInRect(touchX, touchY, kSidebarSectionX, itemY, kSidebarSectionWidth, kSidebarSectionHeight)) {
            return {TouchTargetKind::Section, static_cast<int>(index)};
        }
    }

    if (PointInRect(touchX, touchY, kInstallButtonX, kInstallButtonY, kSidebarCardWidth, kSidebarActionCardHeight)) {
        return {TouchTargetKind::ActionButton, 0};
    }
    if (PointInRect(touchX, touchY, kExitButtonX, kExitButtonY, kSidebarCardWidth, kSidebarActionCardHeight)) {
        return {TouchTargetKind::SearchButton, 0};
    }
    if (PointInRect(touchX, touchY, kSortButtonX, kSortButtonY, kSidebarCardWidth, kSidebarActionCardHeight)) {
        return {TouchTargetKind::SortButton, 0};
    }
    if (PointInRect(touchX, touchY, kLanguageButtonX, kLanguageButtonY, kSidebarCardWidth, kSidebarActionCardHeight)) {
        return {TouchTargetKind::LanguageButton, 0};
    }
    if (PointInRect(touchX, touchY, kRefreshButtonX, kRefreshButtonY, kSidebarCardWidth, kSidebarActionCardHeight)) {
        return {TouchTargetKind::RefreshButton, 0};
    }
    if (PointInRect(touchX, touchY, kThemeButtonX, kThemeButtonY, kSidebarCardWidth, kSidebarActionCardHeight)) {
        return {TouchTargetKind::ThemeButton, 0};
    }

    if (!items.empty()) {
        const std::size_t clampedSelection = std::min(state.selection, items.size() - 1);
        const auto window = GetVisibleEntryWindow(items.size(), clampedSelection, kEntryListHeight);
        const std::size_t windowStart = window.first;
        const std::size_t windowEnd = window.second;
        int cardY = kEntryCardY;
        for (std::size_t index = windowStart; index < windowEnd; ++index) {
            if (PointInRect(touchX, touchY, kEntryCardX, cardY, kEntryCardWidth, kEntryCardHeight)) {
                return {TouchTargetKind::Entry, static_cast<int>(index)};
            }
            cardY += kEntryCardHeight + kEntryCardGap;
        }
    }

    return {};
}

bool SameTouchTarget(const TouchTarget& left, const TouchTarget& right) {
    return left.kind == right.kind && left.index == right.index;
}

std::string DeriveCheatsIndexLocation(const std::string& catalogSource) {
    if (catalogSource.empty()) {
        return {};
    }
    if (catalogSource == kSwitchLocalIndexPath) {
        return kSwitchLocalCheatsIndexPath;
    }
    if (catalogSource == kCatalogCachePath) {
        return kCheatsIndexCachePath;
    }
    if ((catalogSource.rfind("http://", 0) == 0 || catalogSource.rfind("https://", 0) == 0) &&
        catalogSource.size() >= 10 &&
        catalogSource.substr(catalogSource.size() - 10) == "index.json") {
        return catalogSource.substr(0, catalogSource.size() - 10) + "cheats-index.json";
    }
    return {};
}

std::string DeriveCheatsSummaryLocation(const std::string& catalogSource) {
    if (catalogSource.empty()) {
        return {};
    }
    if (catalogSource == kSwitchLocalIndexPath) {
        return kSwitchLocalCheatsSummaryPath;
    }
    if (catalogSource == kCatalogCachePath) {
        return kCheatsSummaryCachePath;
    }
    if ((catalogSource.rfind("http://", 0) == 0 || catalogSource.rfind("https://", 0) == 0) &&
        catalogSource.size() >= 10 &&
        catalogSource.substr(catalogSource.size() - 10) == "index.json") {
        return catalogSource.substr(0, catalogSource.size() - 10) + "cheats-summary.json";
    }
    return {};
}

std::string DeriveCheatsPackLocation(const std::string& cheatsIndexSource) {
    if (cheatsIndexSource.empty()) {
        return {};
    }
    if (cheatsIndexSource == kSwitchLocalCheatsIndexPath || cheatsIndexSource == kCheatsIndexCachePath) {
        return {};
    }
    if ((cheatsIndexSource.rfind("http://", 0) == 0 || cheatsIndexSource.rfind("https://", 0) == 0) &&
        cheatsIndexSource.size() >= 17 &&
        cheatsIndexSource.substr(cheatsIndexSource.size() - 17) == "cheats-index.json") {
        return cheatsIndexSource.substr(0, cheatsIndexSource.size() - 17) + "cheats-pack.zip";
    }
    return {};
}

std::string DeriveSavesIndexLocation(const std::string& catalogSource) {
    if (catalogSource.empty()) {
        return {};
    }
    if (catalogSource == kSwitchLocalIndexPath) {
        return kSwitchLocalSavesIndexPath;
    }
    if (catalogSource == kCatalogCachePath) {
        return kSavesIndexCachePath;
    }
    if ((catalogSource.rfind("http://", 0) == 0 || catalogSource.rfind("https://", 0) == 0) &&
        catalogSource.size() >= 10 &&
        catalogSource.substr(catalogSource.size() - 10) == "index.json") {
        return catalogSource.substr(0, catalogSource.size() - 10) + "saves-index.json";
    }
    return {};
}

std::string BaseUrlForDocumentLocation(const std::string& source) {
    if (source.rfind("http://", 0) != 0 && source.rfind("https://", 0) != 0) {
        return {};
    }
    const std::size_t slash = source.find_last_of('/');
    if (slash == std::string::npos) {
        return {};
    }
    return source.substr(0, slash + 1);
}

std::string JoinBaseUrlAndRelativePath(const std::string& baseUrl, const std::string& relativePath) {
    if (baseUrl.empty() || relativePath.empty()) {
        return {};
    }
    if (relativePath.rfind("http://", 0) == 0 || relativePath.rfind("https://", 0) == 0) {
        return relativePath;
    }
    if (baseUrl.back() == '/') {
        return baseUrl + relativePath;
    }
    return baseUrl + "/" + relativePath;
}

std::string ResolveDeliveryUrl(const std::string& relativePath,
                               const std::string& explicitUrl,
                               const std::string& deliveryBaseUrl,
                               const std::string& sourceDocument) {
    if (!explicitUrl.empty()) {
        return explicitUrl;
    }
    if (!relativePath.empty()) {
        if (const std::string fromDeliveryBase = JoinBaseUrlAndRelativePath(deliveryBaseUrl, relativePath);
            !fromDeliveryBase.empty()) {
            return fromDeliveryBase;
        }
        if (const std::string fromSource = JoinBaseUrlAndRelativePath(BaseUrlForDocumentLocation(sourceDocument), relativePath);
            !fromSource.empty()) {
            return fromSource;
        }
    }
    return {};
}

std::string ResolveThumbPackUrl(const CatalogIndex& catalog, const std::string& catalogSource) {
    return ResolveDeliveryUrl(catalog.thumbPackRelativePath,
                              catalog.thumbPackUrl,
                              catalog.deliveryBaseUrl,
                              catalogSource);
}

std::string ResolveCheatsPackUrl(const CheatsIndex& index, const std::string& cheatsIndexSource) {
    return ResolveDeliveryUrl(index.cheatsPackRelativePath,
                              index.cheatsPackUrl,
                              index.deliveryBaseUrl,
                              cheatsIndexSource);
}

std::string ResolveCatalogEntryDownloadUrl(const AppState& state, const CatalogEntry& entry) {
    return ResolveDeliveryUrl(entry.relativePath, entry.downloadUrl, state.catalog.deliveryBaseUrl, state.activeCatalogSource);
}

std::string ResolveCatalogVariantDownloadUrl(const AppState& state, const CatalogVariant& variant) {
    return ResolveDeliveryUrl(variant.relativePath, variant.downloadUrl, state.catalog.deliveryBaseUrl, state.activeCatalogSource);
}

std::string ResolveCheatBuildDownloadUrl(const AppState& state, const CheatBuildRecord& build) {
    return ResolveDeliveryUrl(build.relativePath, build.downloadUrl, state.cheatsIndex.deliveryBaseUrl, state.activeCheatsSource);
}

std::string ResolveCheatEntryDownloadUrl(const AppState& state, const CheatEntryRecord& entry) {
    return ResolveDeliveryUrl(entry.relativePath, entry.downloadUrl, state.cheatsIndex.deliveryBaseUrl, state.activeCheatsSource);
}

std::string ResolveSaveVariantDownloadUrl(const AppState& state, const SaveVariantRecord& variant) {
    return ResolveDeliveryUrl(variant.relativePath, variant.downloadUrl, state.savesIndex.deliveryBaseUrl, state.activeSavesSource);
}

std::string DeriveThumbManifestLocation(const CatalogIndex& catalog) {
    if (!catalog.thumbPackUrl.empty()) {
        const std::string suffix = "thumbs-pack.zip";
        if (catalog.thumbPackUrl.size() >= suffix.size() &&
            catalog.thumbPackUrl.substr(catalog.thumbPackUrl.size() - suffix.size()) == suffix) {
            return catalog.thumbPackUrl.substr(0, catalog.thumbPackUrl.size() - suffix.size()) + "thumbs-manifest.json";
        }
    }
    return {};
}

std::string DeriveCheatsManifestLocation(const CheatsIndex& index) {
    if (!index.cheatsPackUrl.empty()) {
        const std::string suffix = "cheats-pack.zip";
        if (index.cheatsPackUrl.size() >= suffix.size() &&
            index.cheatsPackUrl.substr(index.cheatsPackUrl.size() - suffix.size()) == suffix) {
            return index.cheatsPackUrl.substr(0, index.cheatsPackUrl.size() - suffix.size()) + "cheats-manifest.json";
        }
    }
    return {};
}

void ApplyCheatPackDefaults(CheatsIndex& index, const std::string& cheatsIndexSource) {
    if (index.cheatsPackRevision.empty()) {
        index.cheatsPackRevision = !index.catalogRevision.empty() ? index.catalogRevision : index.generatedAt;
    }
    if (index.cheatsPackUrl.empty()) {
        index.cheatsPackUrl = DeriveCheatsPackLocation(cheatsIndexSource);
    }
}

bool LoadCheatsIndex(AppState& state,
                     bool preferLocalCache = false,
                     bool allowRemote = true,
                     bool allowRemotePackDownload = false,
                     bool allowRyujinxRemote = false) {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kConfigRootDir);
    EnsureDirectory(kCacheDir);

    auto tryLoadLocalCheats = [&](const char* path) {
        std::string localError;
        CheatsIndex localIndex;
        if (!LoadCheatsIndexFromFile(path, localIndex, localError)) {
            return false;
        }
        ApplyCheatPackDefaults(localIndex, path);
        state.cheatsIndex = std::move(localIndex);
        state.cheatsIndexFiltered = false;
        PopulateDetectedCheatBuildIds(state, !IsRyujinxGuestEnvironment());
        FilterCheatsIndexToDetectedTitles(state);
        state.activeCheatsSource = path;
        RefreshDerivedCheatEntries(state);
        return true;
    };

    const std::string derivedSummarySource = DeriveCheatsSummaryLocation(state.activeCatalogSource);
    const std::string derivedIndexSource = DeriveCheatsIndexLocation(state.activeCatalogSource);
    if (derivedSummarySource == kSwitchLocalCheatsSummaryPath && tryLoadLocalCheats(kSwitchLocalCheatsSummaryPath)) {
        return true;
    }

    if (derivedIndexSource == kSwitchLocalCheatsIndexPath && tryLoadLocalCheats(kSwitchLocalCheatsIndexPath)) {
        return true;
    }

    if (tryLoadLocalCheats(kSwitchLocalCheatsSummaryPath)) {
        return true;
    }

    if (tryLoadLocalCheats(kSwitchLocalCheatsIndexPath)) {
        return true;
    }

    if (preferLocalCache && tryLoadLocalCheats(kCheatsSummaryCachePath)) {
        return true;
    }

    if (preferLocalCache && tryLoadLocalCheats(kCheatsIndexCachePath)) {
        return true;
    }

    if (FileExists(kCheatsSummaryTempPath)) {
        if (tryLoadLocalCheats(kCheatsSummaryTempPath)) {
            remove(kCheatsSummaryCachePath);
            std::rename(kCheatsSummaryTempPath, kCheatsSummaryCachePath);
            state.activeCheatsSource = kCheatsSummaryCachePath;
            return true;
        }
        remove(kCheatsSummaryTempPath);
    }

    if (FileExists(kCheatsIndexTempPath)) {
        if (tryLoadLocalCheats(kCheatsIndexTempPath)) {
            remove(kCheatsIndexCachePath);
            std::rename(kCheatsIndexTempPath, kCheatsIndexCachePath);
            state.activeCheatsSource = kCheatsIndexCachePath;
            return true;
        }
        remove(kCheatsIndexTempPath);
    }

    if (allowRemote && (!IsRyujinxGuestEnvironment() || allowRyujinxRemote)) {
        std::vector<std::string> remoteSources;
        auto appendRemoteSource = [&](const std::string& source) {
            if (source.empty()) {
                return;
            }
            if (source.rfind("http://", 0) != 0 && source.rfind("https://", 0) != 0) {
                return;
            }
            if (std::find(remoteSources.begin(), remoteSources.end(), source) == remoteSources.end()) {
                remoteSources.push_back(source);
            }
        };

        appendRemoteSource(derivedSummarySource);
        appendRemoteSource(derivedIndexSource);
        for (const std::string& catalogUrl : state.config.catalogUrls) {
            appendRemoteSource(DeriveCheatsSummaryLocation(catalogUrl));
            appendRemoteSource(DeriveCheatsIndexLocation(catalogUrl));
        }

        for (const std::string& remoteSource : remoteSources) {
            std::string error;
            HttpDownloadOptions options;
            options.connectTimeoutMs = 6000;
            options.requestTimeoutMs = 60000;
            options.retryCount = 2;
            options.probeDownloadInfo = false;
            options.allowResume = false;
            UiDownloadProgressContext indexProgress{
                &state,
                UiText(state, u8"Carregando trapaças", "Loading cheats"),
                u8"Baixando índice de trapaças...",
                "Downloading cheats index...",
                25,
                35};
            options.progressCallback = UpdateUiDownloadProgress;
            options.progressUserData = &indexProgress;

            const bool isSummarySource =
                remoteSource.size() >= 19 && remoteSource.substr(remoteSource.size() - 19) == "cheats-summary.json";
            const std::string tempPath = isSummarySource ? kCheatsSummaryTempPath : kCheatsIndexTempPath;
            std::size_t downloadedBytes = 0;
            if (!HttpDownloadToFileWithOptions(remoteSource, tempPath, options, &downloadedBytes, error) ||
                downloadedBytes == 0) {
                remove(tempPath.c_str());
                continue;
            }

            CheatsIndex remoteIndex;
            std::string parseError;
            if (!LoadCheatsIndexFromFile(tempPath, remoteIndex, parseError)) {
                remove(tempPath.c_str());
                continue;
            }

            ApplyCheatPackDefaults(remoteIndex, remoteSource);
            const char* cachePath = isSummarySource ? kCheatsSummaryCachePath : kCheatsIndexCachePath;
            const std::string existingCache = ReadTextFile(cachePath);
            const std::string downloadedCache = ReadTextFile(tempPath);
            if (existingCache != downloadedCache) {
                remove(cachePath);
                std::rename(tempPath.c_str(), cachePath);
            } else {
                remove(tempPath.c_str());
            }
            state.cheatsIndex = std::move(remoteIndex);
            state.cheatsIndexFiltered = false;
            PopulateDetectedCheatBuildIds(state, !IsRyujinxGuestEnvironment());
            FilterCheatsIndexToDetectedTitles(state);
            state.activeCheatsSource = remoteSource;
            RefreshDerivedCheatEntries(state);
            return true;
        }
    }

    if (tryLoadLocalCheats(kCheatsIndexCachePath)) {
        return true;
    }

    state.cheatsIndex = {};
    state.cheatsIndexFiltered = false;
    state.activeCheatsSource.clear();
    RefreshDerivedCheatEntries(state);
    return false;
}

bool EnsureCheatsIndexReady(AppState& state, bool forceRemoteRefresh, bool allowRyujinxRemote) {
    if (state.section != ContentSection::Cheats) {
        return false;
    }

    if (forceRemoteRefresh) {
        SetProgress(state,
                    UiText(state, u8"Carregando trapaças", "Loading cheats"),
                    UiText(state, u8"Atualizando índice e cache de trapaças...", "Updating cheats index and cache..."),
                    25);
        const bool loaded = LoadCheatsIndex(state, false, true, true, allowRyujinxRemote);
        SetProgress(state,
                    UiText(state, u8"Carregando trapaças", "Loading cheats"),
                    UiText(state, u8"Concluído.", "Done."),
                    100);
        ClearProgress(state);
        return loaded;
    }

    if (!state.cheatsIndex.titles.empty() || !state.activeCheatsSource.empty()) {
        if (!NormalizeSearchQuery(state.searchQuery).empty() && state.cheatsIndexFiltered) {
            if (LoadCheatsIndex(state, true, false, false, false)) {
                return true;
            }
            if (IsRyujinxGuestEnvironment()) {
                return false;
            }
            return LoadCheatsIndex(state, false, true, false, false);
        }
        PopulateDetectedCheatBuildIds(state, !IsRyujinxGuestEnvironment());
        return true;
    }

    if (LoadCheatsIndex(state, true, false, false, false)) {
        return true;
    }

    if (IsRyujinxGuestEnvironment()) {
        state.statusLine = UseEnglish(state) ? "Use Refresh to download cheats index."
                                             : u8"Use Atualizar para baixar o índice de trapaças.";
        return false;
    }

    SetProgress(state,
                UiText(state, u8"Carregando trapaças", "Loading cheats"),
                UiText(state, u8"Baixando índice inicial de trapaças...", "Downloading initial cheats index..."),
                25);
    const bool loaded = LoadCheatsIndex(state, false, true, false, false);
    SetProgress(state,
                UiText(state, u8"Carregando trapaças", "Loading cheats"),
                UiText(state, u8"Concluído.", "Done."),
                100);
    ClearProgress(state);
    return loaded;
}

bool LoadSavesIndex(AppState& state,
                    bool preferLocalCache = false,
                    bool allowRemote = true,
                    bool allowRyujinxRemote = false) {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kConfigRootDir);
    EnsureDirectory(kCacheDir);

    auto tryLoadLocalSaves = [&](const char* path) {
        std::string localError;
        SavesIndex localIndex;
        if (!LoadSavesIndexFromFile(path, localIndex, localError)) {
            return false;
        }
        state.savesIndex = std::move(localIndex);
        state.activeSavesSource = path;
        state.savesIndexLoaded = true;
        RefreshDerivedSaveEntries(state);
        return true;
    };

    const std::string derivedSource = DeriveSavesIndexLocation(state.activeCatalogSource);
    if (derivedSource == kSwitchLocalSavesIndexPath && tryLoadLocalSaves(kSwitchLocalSavesIndexPath)) {
        return true;
    }
    if (tryLoadLocalSaves(kSwitchLocalSavesIndexPath)) {
        return true;
    }
    if (preferLocalCache && tryLoadLocalSaves(kSavesIndexCachePath)) {
        return true;
    }

    if (FileExists(kSavesIndexTempPath)) {
        if (tryLoadLocalSaves(kSavesIndexTempPath)) {
            remove(kSavesIndexCachePath);
            std::rename(kSavesIndexTempPath, kSavesIndexCachePath);
            state.activeSavesSource = kSavesIndexCachePath;
            return true;
        }
        remove(kSavesIndexTempPath);
    }

    if (allowRemote && (!IsRyujinxGuestEnvironment() || allowRyujinxRemote)) {
        std::vector<std::string> remoteSources;
        auto appendRemoteSource = [&](const std::string& source) {
            if (source.empty()) {
                return;
            }
            if (source.rfind("http://", 0) != 0 && source.rfind("https://", 0) != 0) {
                return;
            }
            if (std::find(remoteSources.begin(), remoteSources.end(), source) == remoteSources.end()) {
                remoteSources.push_back(source);
            }
        };

        appendRemoteSource(derivedSource);
        for (const std::string& catalogUrl : state.config.catalogUrls) {
            appendRemoteSource(DeriveSavesIndexLocation(catalogUrl));
        }

        for (const std::string& remoteSource : remoteSources) {
            std::string error;
            HttpDownloadOptions options;
            options.connectTimeoutMs = 6000;
            options.requestTimeoutMs = 60000;
            options.retryCount = 2;
            options.probeDownloadInfo = false;
            options.allowResume = false;
            UiDownloadProgressContext indexProgress{
                &state,
                UiText(state, u8"Carregando saves", "Loading saves"),
                u8"Baixando índice de saves...",
                "Downloading saves index...",
                25,
                35};
            options.progressCallback = UpdateUiDownloadProgress;
            options.progressUserData = &indexProgress;

            std::size_t downloadedBytes = 0;
            if (!HttpDownloadToFileWithOptions(remoteSource, kSavesIndexTempPath, options, &downloadedBytes, error) ||
                downloadedBytes == 0) {
                remove(kSavesIndexTempPath);
                continue;
            }

            SavesIndex remoteIndex;
            std::string parseError;
            if (!LoadSavesIndexFromFile(kSavesIndexTempPath, remoteIndex, parseError)) {
                remove(kSavesIndexTempPath);
                continue;
            }

            const std::string existingCache = ReadTextFile(kSavesIndexCachePath);
            const std::string downloadedCache = ReadTextFile(kSavesIndexTempPath);
            if (existingCache != downloadedCache) {
                remove(kSavesIndexCachePath);
                std::rename(kSavesIndexTempPath, kSavesIndexCachePath);
            } else {
                remove(kSavesIndexTempPath);
            }

            state.savesIndex = std::move(remoteIndex);
            state.activeSavesSource = remoteSource;
            state.savesIndexLoaded = true;
            RefreshDerivedSaveEntries(state);
            return true;
        }
    }

    if (tryLoadLocalSaves(kSavesIndexCachePath)) {
        return true;
    }

    state.savesIndex = {};
    state.activeSavesSource.clear();
    state.savesIndexLoaded = false;
    RefreshDerivedSaveEntries(state);
    return false;
}

bool EnsureSavesIndexReady(AppState& state, bool forceRemoteRefresh, bool allowRyujinxRemote) {
    if (state.section != ContentSection::SaveGames) {
        return false;
    }

    if (forceRemoteRefresh) {
        SetProgress(state,
                    UiText(state, u8"Carregando saves", "Loading saves"),
                    UiText(state, u8"Atualizando índice e cache de saves...", "Updating saves index and cache..."),
                    25);
        const bool loaded = LoadSavesIndex(state, false, true, allowRyujinxRemote);
        SetProgress(state,
                    UiText(state, u8"Carregando saves", "Loading saves"),
                    UiText(state, u8"Concluído.", "Done."),
                    100);
        ClearProgress(state);
        return loaded;
    }

    if (state.savesIndexLoaded || !state.activeSavesSource.empty()) {
        return true;
    }

    if (LoadSavesIndex(state, true, false, false)) {
        return true;
    }

    if (IsRyujinxGuestEnvironment()) {
        state.statusLine = UseEnglish(state) ? "Use Refresh to download saves index."
                                             : u8"Use Atualizar para baixar o índice de saves.";
        return false;
    }

    SetProgress(state,
                UiText(state, u8"Carregando saves", "Loading saves"),
                UiText(state, u8"Baixando índice inicial de saves...", "Downloading initial saves index..."),
                25);
    const bool loaded = LoadSavesIndex(state, false, true, false);
    SetProgress(state,
                UiText(state, u8"Carregando saves", "Loading saves"),
                UiText(state, u8"Concluído.", "Done."),
                100);
    ClearProgress(state);
    return loaded;
}

bool LoadCatalog(AppState& state,
                 bool preferLocalCache = false,
                 bool allowRemote = true,
                 bool allowRemotePackDownload = false,
                 bool allowRyujinxRemote = false) {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kConfigRootDir);
    EnsureDirectory(kCacheDir);
    const bool english = state.config.language == LanguageMode::EnUs;

    auto tryLoadLocalCatalog = [&](const char* path, const char* statusMessage) {
        std::string localError;
        CatalogIndex localCatalog;
        if (!LoadCatalogFromFile(path, localCatalog, localError)) {
            return false;
        }
        state.catalog = std::move(localCatalog);
        RefreshCatalogSearchIndex(state);
        std::string thumbError;
        EnsureThumbPackCache(state.catalog, state.activeCatalogSource, thumbError, false);
        ClearThumbnailFailures(state);
        state.activeCatalogSource = path;
        RefreshDerivedCheatEntries(state);
        RefreshDerivedSaveEntries(state);
        state.statusLine = statusMessage;
        return true;
    };

    if (IsEmulatorEnvironment()) {
        if (tryLoadLocalCatalog(kSwitchLocalIndexPath,
                                english ? "Using local synchronized catalog." : "Usando catálogo local sincronizado.")) {
            return true;
        }
    }

    if (preferLocalCache) {
        if (tryLoadLocalCatalog(kCatalogCachePath,
                                english ? "Using cached catalog." : "Usando catálogo em cache.")) {
            return true;
        }
    }

    if (IsRyujinxGuestEnvironment() && !allowRyujinxRemote) {
        state.statusLine = english ? "Use Refresh to download catalog cache." : u8"Use Atualizar para baixar o cache do catálogo.";
        return false;
    }

    std::string error;
    for (const std::string& url : state.config.catalogUrls) {
        HttpResponse response;
        std::string candidateError;
        if (!HttpGetToString(url, response, candidateError)) {
            error = candidateError;
            continue;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            error = "HTTP " + std::to_string(response.statusCode) + " ao ler " + url;
            continue;
        }

        CatalogIndex remoteCatalog;
        std::string parseError;
        if (!LoadCatalogFromJsonString(response.body, remoteCatalog, parseError)) {
            error = parseError;
            continue;
        }

        const std::string existingCache = ReadTextFile(kCatalogCachePath);
        if (existingCache != response.body) {
            std::ofstream output(kCatalogCachePath, std::ios::binary | std::ios::trunc);
            if (output.good()) {
                output.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
            }
        }

        state.catalog = std::move(remoteCatalog);
        RefreshCatalogSearchIndex(state);
        std::string thumbError;
        UiDownloadProgressContext thumbProgress{
            &state,
            UiText(state, u8"Atualizando", "Refreshing"),
            u8"Baixando pacote de thumbs...",
            "Downloading thumb pack...",
            55,
            25};
        EnsureThumbPackCache(state.catalog,
                             state.activeCatalogSource,
                             thumbError,
                             allowRemotePackDownload || !IsRyujinxGuestEnvironment(),
                             &thumbProgress);
        ClearThumbnailFailures(state);
        state.activeCatalogSource = url;
        RefreshDerivedCheatEntries(state);
        RefreshDerivedSaveEntries(state);
        return true;
    }

    if (tryLoadLocalCatalog(kCatalogCachePath,
                            english ? "Remote catalog unavailable. Using local cache."
                                    : "Catálogo remoto indisponível. Usando cache local.")) {
        return true;
    }

    if (tryLoadLocalCatalog(kSwitchLocalIndexPath,
                            english ? "Using local synchronized catalog." : "Usando catálogo local sincronizado.")) {
        return true;
    }

    state.statusLine = std::string(english ? "Failed to load catalog: " : "Falha ao carregar catálogo: ") + error;
    return false;
}

std::string MakeCompatibilitySummary(const CatalogEntry& entry, const InstalledTitle* title) {
    const std::string variantSummary = EntryHasVariants(entry) ? VariantListSummary(entry) : std::string();
    if (!title) {
        if (!variantSummary.empty()) {
            return "Jogo não encontrado no console/emulador. Variantes disponíveis: " + variantSummary;
        }
        return "Jogo não encontrado no console/emulador.";
    }
    if (title->displayVersion.empty()) {
        if (!variantSummary.empty()) {
            return "Versão do jogo indisponível. Variantes disponíveis: " + variantSummary;
        }
        return "Versão do jogo indisponível.";
    }
    if (EntryMatchesInstalledVersion(entry, title)) {
        return "Compatível com a versão instalada: " + title->displayVersion;
    }

    std::string message = "Atenção: pacote fora da faixa suportada para o jogo instalado (" + title->displayVersion + ").";
    if (!variantSummary.empty()) {
        message += " Variantes disponíveis: " + variantSummary + ".";
    } else {
        if (!entry.compatibility.minGameVersion.empty()) {
            message += " Min: " + entry.compatibility.minGameVersion + ".";
        }
        if (!entry.compatibility.maxGameVersion.empty()) {
            message += " Max: " + entry.compatibility.maxGameVersion + ".";
        }
        if (!entry.compatibility.exactGameVersions.empty()) {
            message += " Exatas: ";
            for (std::size_t index = 0; index < entry.compatibility.exactGameVersions.size(); ++index) {
                if (index > 0) {
                    message += ", ";
                }
                message += entry.compatibility.exactGameVersions[index];
            }
            message += '.';
        }
    }
    return message;
}

void PrintLine(const std::string& text) {
    std::printf("%s\n", text.c_str());
}

bool UseEnglish(const AppState& state) {
    return state.config.language == LanguageMode::EnUs;
}

std::string NormalizeLanguageKeyPart(std::string text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool lastUnderscore = false;
    for (unsigned char ch : text) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            normalized.push_back(static_cast<char>(ch));
            lastUnderscore = false;
            continue;
        }
        if (ch >= 'A' && ch <= 'Z') {
            normalized.push_back(static_cast<char>(ch - 'A' + 'a'));
            lastUnderscore = false;
            continue;
        }
        if (!lastUnderscore) {
            normalized.push_back('_');
            lastUnderscore = true;
        }
    }
    while (!normalized.empty() && normalized.front() == '_') {
        normalized.erase(normalized.begin());
    }
    while (!normalized.empty() && normalized.back() == '_') {
        normalized.pop_back();
    }
    if (normalized.empty()) {
        normalized = "text";
    }
    return normalized;
}

std::string BuildAutoLanguageKey(const char* ptBr, const char* enUs) {
    std::string portuguese = ptBr != nullptr ? ptBr : "";
    std::string english = enUs != nullptr ? enUs : "";
    const std::string normalized = NormalizeLanguageKeyPart(english);
    std::uint32_t hash = 2166136261u;
    const std::string combined = portuguese + "" + english;
    for (unsigned char ch : combined) {
        hash ^= ch;
        hash *= 16777619u;
    }
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "%08x", hash);
    return "auto." + normalized + "." + suffix;
}

std::string LookupLanguageString(const AppState& state, const std::string& key) {
    const auto it = state.languageStrings.find(key);
    if (it != state.languageStrings.end() && !it->second.empty()) {
        return it->second;
    }
    return "[[" + key + "]]";
}

std::string UiText(const AppState& state, const char* ptBr, const char* enUs) {
    (void)ptBr;
    return LookupLanguageString(state, BuildAutoLanguageKey(ptBr, enUs));
}

std::string UiString(const AppState& state, const char* key, const char* ptBr, const char* enUs) {
    (void)ptBr;
    (void)enUs;
    return LookupLanguageString(state, key != nullptr ? key : "ui.missing_key");
}

void ReloadLanguageStrings(AppState& state) {
    std::unordered_map<std::string, std::string> loaded;
    std::string error;
    if (LoadLanguageStringsFromFile(LanguageJsonPath(state.config.language), loaded, error)) {
        state.languageStrings = std::move(loaded);
        state.languageStringsLoaded = true;
        return;
    }
    state.languageStrings.clear();
    state.languageStringsLoaded = false;
}

std::string LocalizedEntryIntro(const AppState& state, const CatalogEntry& entry) {
    if (UseEnglish(state)) {
        if (!entry.introEnUs.empty()) {
            return entry.introEnUs;
        }
        if (!entry.introPtBr.empty()) {
            return entry.introPtBr;
        }
    } else {
        if (!entry.introPtBr.empty()) {
            return entry.introPtBr;
        }
        if (!entry.introEnUs.empty()) {
            return entry.introEnUs;
        }
    }
    return entry.intro;
}

std::string LocalizedEntrySummary(const AppState& state, const CatalogEntry& entry) {
    if (UseEnglish(state)) {
        if (!entry.summaryEnUs.empty()) {
            return entry.summaryEnUs;
        }
        if (!entry.summaryPtBr.empty()) {
            return entry.summaryPtBr;
        }
    } else {
        if (!entry.summaryPtBr.empty()) {
            return entry.summaryPtBr;
        }
        if (!entry.summaryEnUs.empty()) {
            return entry.summaryEnUs;
        }
    }
    return entry.summary;
}

std::string LocalizeContentTypeLabel(const AppState& state, const std::string& rawType) {
    const std::string type = ToLowerAscii(rawType);
    if (type == "translation") {
        return UiString(state, "ui.content.translation", u8"Tradu??o", "Translation");
    }
    if (type == "dub") {
        return UiString(state, "ui.content.dubbing", "Dublagem", "Dubbing");
    }
    if (type == "mod") {
        return UiString(state, "ui.content.mod", "Mod", "Mod");
    }
    if (type == "cheat") {
        return UiString(state, "ui.content.cheat", u8"Trapa?a", "Cheat");
    }
    return rawType;
}

std::vector<std::string> EntryContentTypeLabels(const AppState& state, const CatalogEntry& entry) {
    std::vector<std::string> labels;
    const auto appendUnique = [&](const std::string& value) {
        if (!value.empty() && std::find(labels.begin(), labels.end(), value) == labels.end()) {
            labels.push_back(value);
        }
    };

    for (const auto& type : entry.contentTypes) {
        appendUnique(LocalizeContentTypeLabel(state, type));
    }

    if (labels.empty()) {
        switch (entry.section) {
            case ContentSection::Translations:
                appendUnique(UiString(state, "ui.content.translation", u8"Tradu??o", "Translation"));
                break;
            case ContentSection::ModsTools:
                appendUnique(UiString(state, "ui.content.mod", "Mod", "Mod"));
                break;
            case ContentSection::Cheats:
                appendUnique(UiString(state, "ui.content.cheat", u8"Trapa?a", "Cheat"));
                break;
            case ContentSection::SaveGames:
                appendUnique(UiString(state, "ui.content.save", "Save", "Save"));
                break;
            default:
                break;
        }
    }

    return labels;
}

std::string JoinLabels(const std::vector<std::string>& labels, const std::string& separator) {
    std::string result;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        if (index > 0) {
            result += separator;
        }
        result += labels[index];
    }
    return result;
}

const CheatTitleRecord* FindCheatTitleRecord(const CheatsIndex& index, const std::string& titleId);
const CheatBuildRecord* FindCheatBuildRecord(const CheatTitleRecord& title, const std::string& buildId);
bool EntryUsesCheatsIndex(const AppState& state, const CatalogEntry& entry);

std::string EntryVersionStatusLabel(const AppState& state, const CatalogEntry& entry, const InstalledTitle* installedTitle) {
    if (entry.section == ContentSection::SaveGames) {
        return {};
    }
    if (entry.section == ContentSection::Cheats && EntryUsesCheatsIndex(state, entry)) {
        if (installedTitle == nullptr || installedTitle->buildIdHex.empty()) {
            return UiText(state, "Build Desconhecida", "Build Unknown");
        }
        const CheatTitleRecord* cheatTitle = FindCheatTitleRecord(state.cheatsIndex, entry.titleId);
        if (cheatTitle != nullptr && FindCheatBuildRecord(*cheatTitle, installedTitle->buildIdHex) != nullptr) {
            return UiText(state, "Mesmo Build", "Matching Build");
        }
        return UiText(state, "Build Divergente", "Build Mismatch");
    }
    if (installedTitle == nullptr || installedTitle->displayVersion.empty()) {
        return UiText(state, "Versão Não Detectada", "Version Unknown");
    }
    if (EntryMatchesInstalledVersion(entry, installedTitle)) {
        return UiText(state, "Mesma Versão", "Matching Version");
    }
    return UiText(state, "Versão Divergente", "Version Mismatch");
}

bool SectionRequiresInstallConfirmation(ContentSection section) {
    return section == ContentSection::Translations || section == ContentSection::ModsTools || section == ContentSection::Cheats;
}

bool EntryHasCompatibilityRules(const CatalogEntry& entry) {
    return !entry.compatibility.minGameVersion.empty() || !entry.compatibility.maxGameVersion.empty() ||
           !entry.compatibility.exactGameVersions.empty();
}

bool CompatibilityRuleHasConstraints(const CompatibilityRule& rule) {
    return !rule.minGameVersion.empty() || !rule.maxGameVersion.empty() || !rule.exactGameVersions.empty();
}

bool EntryHasVariants(const CatalogEntry& entry) {
    return !entry.variants.empty();
}

std::string CompatibilityRuleLabel(const CompatibilityRule& rule) {
    if (!rule.exactGameVersions.empty()) {
        return JoinLabels(rule.exactGameVersions, ", ");
    }
    if (!rule.minGameVersion.empty() && !rule.maxGameVersion.empty()) {
        return rule.minGameVersion + " - " + rule.maxGameVersion;
    }
    if (!rule.minGameVersion.empty()) {
        return std::string(">= ") + rule.minGameVersion;
    }
    if (!rule.maxGameVersion.empty()) {
        return std::string("<= ") + rule.maxGameVersion;
    }
    return {};
}

std::string VariantDisplayLabel(const CatalogVariant& variant) {
    if (!variant.label.empty()) {
        return variant.label;
    }
    const std::string compatibilityLabel = CompatibilityRuleLabel(variant.compatibility);
    if (!compatibilityLabel.empty()) {
        return compatibilityLabel;
    }
    if (!variant.packageVersion.empty()) {
        return variant.packageVersion;
    }
    return variant.id;
}

const CatalogVariant* FindVariantById(const CatalogEntry& entry, const std::string& variantId) {
    for (const auto& variant : entry.variants) {
        if (variant.id == variantId) {
            return &variant;
        }
    }
    return nullptr;
}

std::vector<const CatalogVariant*> CollectAllVariants(const CatalogEntry& entry) {
    std::vector<const CatalogVariant*> variants;
    for (const auto& variant : entry.variants) {
        variants.push_back(&variant);
    }
    return variants;
}

std::vector<const CatalogVariant*> FindCompatibleVariants(const CatalogEntry& entry, const InstalledTitle* installedTitle) {
    std::vector<const CatalogVariant*> variants;
    if (!installedTitle || installedTitle->displayVersion.empty()) {
        return variants;
    }
    for (const auto& variant : entry.variants) {
        if (MatchesCompatibility(variant.compatibility, installedTitle->displayVersion)) {
            variants.push_back(&variant);
        }
    }
    return variants;
}

bool EntryMatchesInstalledVersion(const CatalogEntry& entry, const InstalledTitle* installedTitle) {
    if (installedTitle == nullptr || installedTitle->displayVersion.empty()) {
        return false;
    }
    if (EntryHasVariants(entry)) {
        return !FindCompatibleVariants(entry, installedTitle).empty();
    }
    return MatchesCompatibility(entry.compatibility, installedTitle->displayVersion);
}

bool EntryHasAnyCompatibilityInformation(const CatalogEntry& entry) {
    if (EntryHasCompatibilityRules(entry)) {
        return true;
    }
    for (const auto& variant : entry.variants) {
        if (CompatibilityRuleHasConstraints(variant.compatibility)) {
            return true;
        }
    }
    return false;
}

CatalogEntry ResolveEntryForVariant(const CatalogEntry& entry, const CatalogVariant* variant) {
    CatalogEntry resolved = entry;
    if (variant == nullptr) {
        return resolved;
    }
    if (!variant->assetId.empty()) {
        resolved.assetId = variant->assetId;
    }
    if (!variant->assetType.empty()) {
        resolved.assetType = variant->assetType;
    }
    if (!variant->contentHash.empty()) {
        resolved.contentHash = variant->contentHash;
    }
    if (!variant->relativePath.empty()) {
        resolved.relativePath = variant->relativePath;
    }
    if (!variant->downloadUrl.empty()) {
        resolved.downloadUrl = variant->downloadUrl;
    }
    if (variant->size > 0) {
        resolved.size = variant->size;
    }
    if (!variant->packageVersion.empty()) {
        resolved.packageVersion = variant->packageVersion;
    }
    if (!variant->contentRevision.empty()) {
        resolved.contentRevision = variant->contentRevision;
    }
    resolved.compatibility = variant->compatibility;
    return resolved;
}

std::string VariantListSummary(const CatalogEntry& entry) {
    std::vector<std::string> labels;
    for (const auto& variant : entry.variants) {
        AppendUniqueString(labels, VariantDisplayLabel(variant));
    }
    return JoinLabels(labels, ", ");
}

const CatalogEntry* FindCatalogEntryById(const CatalogIndex& catalog, const std::string& entryId) {
    for (const auto& entry : catalog.entries) {
        if (entry.id == entryId) {
            return &entry;
        }
    }
    return nullptr;
}

const CatalogEntry* FindVisibleEntryById(const AppState& state, const std::string& entryId) {
    if (const CatalogEntry* entry = FindCatalogEntryById(state.catalog, entryId)) {
        return entry;
    }
    for (const auto& entry : state.derivedCheatEntries) {
        if (entry.id == entryId) {
            return &entry;
        }
    }
    for (const auto& entry : state.derivedSaveEntries) {
        if (entry.id == entryId) {
            return &entry;
        }
    }
    return nullptr;
}

const CheatTitleRecord* FindCheatTitleRecord(const CheatsIndex& index, const std::string& titleId) {
    const std::string normalizedTitleId = ToLowerAscii(titleId);
    for (const auto& title : index.titles) {
        if (ToLowerAscii(title.titleId) == normalizedTitleId) {
            return &title;
        }
    }
    return nullptr;
}

const SaveTitleRecord* FindSaveTitleRecord(const SavesIndex& index, const std::string& titleId) {
    const std::string normalizedTitleId = ToLowerAscii(titleId);
    for (const auto& title : index.titles) {
        if (ToLowerAscii(title.titleId) == normalizedTitleId) {
            return &title;
        }
    }
    return nullptr;
}

const SaveVariantRecord* FindSaveVariantRecord(const SaveTitleRecord& title, const std::string& variantId) {
    const std::string normalizedVariantId = ToLowerAscii(variantId);
    for (const auto& variant : title.variants) {
        if (ToLowerAscii(variant.id) == normalizedVariantId) {
            return &variant;
        }
    }
    return nullptr;
}

const CheatBuildRecord* FindCheatBuildRecord(const CheatTitleRecord& title, const std::string& buildId) {
    const std::string normalizedBuildId = ToLowerAscii(buildId);
    for (const auto& build : title.builds) {
        if (build.buildId == normalizedBuildId) {
            return &build;
        }
    }
    return nullptr;
}

const CheatEntryRecord* BestCheatEntryForBuild(const CheatBuildRecord& build) {
    const CheatEntryRecord* best = nullptr;
    for (const auto& entry : build.entries) {
        if (best == nullptr || entry.priorityRank < best->priorityRank ||
            (entry.priorityRank == best->priorityRank && entry.sources.size() > best->sources.size())) {
            best = &entry;
        }
    }
    return best;
}

std::string CheatBuildLabel(const CheatBuildRecord& build) {
    std::string label = build.buildId;
    if (!build.categories.empty()) {
        label += " • " + JoinLabels(build.categories, ", ");
        return label;
    }
    const CheatEntryRecord* best = BestCheatEntryForBuild(build);
    if (best != nullptr && !best->categories.empty()) {
        label += " • " + JoinLabels(best->categories, ", ");
    }
    return label;
}

std::vector<std::string> CollectCheatBuildIds(const CheatTitleRecord& title) {
    std::vector<std::string> buildIds;
    for (const auto& build : title.builds) {
        buildIds.push_back(build.buildId);
    }
    return buildIds;
}

bool EntryUsesCheatsIndex(const AppState& state, const CatalogEntry& entry) {
    return entry.section == ContentSection::Cheats &&
           FindCheatTitleRecord(state.cheatsIndex, entry.titleId) != nullptr;
}

bool EntryUsesSavesIndex(const AppState& state, const CatalogEntry& entry) {
    return entry.section == ContentSection::SaveGames &&
           FindSaveTitleRecord(state.savesIndex, entry.titleId) != nullptr;
}

std::string LocalizedSaveCategoryLabel(const AppState& state, const std::string& rawCategory) {
    const std::string category = ToLowerAscii(rawCategory);
    if (category == "complete") {
        return UiText(state, "Completo", "Complete");
    }
    if (category == "starter") {
        return UiText(state, "Inicial", "Starter");
    }
    if (category == "event") {
        return UiText(state, "Evento", "Event");
    }
    if (category == "ngplus") {
        return "NG+";
    }
    if (category == "unlocked") {
        return UiText(state, "Desbloqueado", "Unlocked");
    }
    if (category == "modded") {
        return UiText(state, "Modificado", "Modded");
    }
    return rawCategory;
}

std::string SaveVariantCountLabel(const AppState& state, std::size_t variantCount) {
    if (variantCount == 1) {
        return UiText(state, "1 variante", "1 variant");
    }
    return std::to_string(variantCount) + " " + UiText(state, "variantes", "variants");
}

std::string LocalizedCheatCategoryLabel(const AppState& state, const std::string& rawCategory) {
    const std::string category = ToLowerAscii(rawCategory);
    if (category == "general") {
        return UiText(state, "Geral", "General");
    }
    if (category == "graphics") {
        return UiText(state, "Gráficos", "Graphics");
    }
    if (category == "fps" || category == "60fps") {
        return "60FPS";
    }
    if (category == "community") {
        return UiText(state, "Comunidade", "Community");
    }
    return rawCategory;
}

std::string LocalizedCheatSourceLabel(const AppState& state, const std::string& rawSource) {
    const std::string source = ToLowerAscii(rawSource);
    if (source == "titledb") {
        return "TitleDB";
    }
    if (source == "gbatempmirror") {
        return "GBAtemp Mirror";
    }
    if (source == "cheatslips") {
        return "Cheat Slips";
    }
    if (source == "chansey") {
        return "Chansey";
    }
    if (source == "ibnux") {
        return "ibnux";
    }
    return rawSource;
}

std::string SummarizeLabelList(const std::vector<std::string>& labels, std::size_t maxVisible) {
    if (labels.empty()) {
        return {};
    }

    std::vector<std::string> visible;
    const std::size_t clampedVisible = std::min(maxVisible, labels.size());
    for (std::size_t index = 0; index < clampedVisible; ++index) {
        visible.push_back(labels[index]);
    }

    std::string summary = JoinLabels(visible, ", ");
    if (labels.size() > clampedVisible) {
        summary += " +" + std::to_string(labels.size() - clampedVisible);
    }
    return summary;
}

std::vector<std::string> CollectCheatCategoriesLocalized(const AppState& state, const CheatTitleRecord& title) {
    std::vector<std::string> labels;
    for (const auto& build : title.builds) {
        for (const auto& category : build.categories) {
            AppendUniqueString(labels, LocalizedCheatCategoryLabel(state, category));
        }
        for (const auto& entry : build.entries) {
            for (const auto& category : entry.categories) {
                AppendUniqueString(labels, LocalizedCheatCategoryLabel(state, category));
            }
        }
    }
    return labels;
}

std::vector<std::string> CollectCheatSourcesLocalized(const AppState& state, const CheatTitleRecord& title) {
    std::vector<std::string> labels;
    for (const auto& build : title.builds) {
        if (!build.primarySource.empty()) {
            AppendUniqueString(labels, LocalizedCheatSourceLabel(state, build.primarySource));
        }
        for (const auto& source : build.sources) {
            AppendUniqueString(labels, LocalizedCheatSourceLabel(state, source));
        }
        for (const auto& entry : build.entries) {
            if (!entry.primarySource.empty()) {
                AppendUniqueString(labels, LocalizedCheatSourceLabel(state, entry.primarySource));
            }
            for (const auto& source : entry.sources) {
                AppendUniqueString(labels, LocalizedCheatSourceLabel(state, source));
            }
        }
    }
    return labels;
}

const CheatBuildRecord* PreferredCheatBuildForDisplay(const AppState& state,
                                                      const CatalogEntry& entry,
                                                      const InstalledTitle* installedTitle) {
    const CheatTitleRecord* title = FindCheatTitleRecord(state.cheatsIndex, entry.titleId);
    if (title == nullptr || title->builds.empty()) {
        return nullptr;
    }
    if (installedTitle != nullptr && !installedTitle->buildIdHex.empty()) {
        if (const CheatBuildRecord* matchingBuild = FindCheatBuildRecord(*title, installedTitle->buildIdHex)) {
            return matchingBuild;
        }
    }
    return &title->builds.front();
}

std::string CheatBuildCountLabel(const AppState& state, std::size_t buildCount) {
    if (buildCount == 1) {
        return UiText(state, "1 build", "1 build");
    }
    return std::to_string(buildCount) + " " + UiText(state, "builds", "builds");
}

std::string CheatCardSubtitle(const AppState& state, const CatalogEntry& entry, const InstalledTitle* installedTitle) {
    const CheatTitleRecord* title = FindCheatTitleRecord(state.cheatsIndex, entry.titleId);
    if (title == nullptr) {
        return JoinLabels(EntryContentTypeLabels(state, entry), ", ");
    }

    const CheatBuildRecord* preferredBuild = PreferredCheatBuildForDisplay(state, entry, installedTitle);
    std::vector<std::string> parts;

    if (preferredBuild != nullptr) {
        std::vector<std::string> categories;
        for (const auto& category : preferredBuild->categories) {
            AppendUniqueString(categories, LocalizedCheatCategoryLabel(state, category));
        }
        if (categories.empty()) {
            for (const auto& item : preferredBuild->entries) {
                for (const auto& category : item.categories) {
                    AppendUniqueString(categories, LocalizedCheatCategoryLabel(state, category));
                }
            }
        }
        const std::string categoriesSummary = SummarizeLabelList(categories, 2);
        if (!categoriesSummary.empty()) {
            parts.push_back(categoriesSummary);
        }

        const CheatEntryRecord* bestEntry = BestCheatEntryForBuild(*preferredBuild);
        if (bestEntry != nullptr || !preferredBuild->primarySource.empty() || !preferredBuild->sources.empty()) {
            std::vector<std::string> sources;
            if (bestEntry != nullptr && !bestEntry->primarySource.empty()) {
                AppendUniqueString(sources, LocalizedCheatSourceLabel(state, bestEntry->primarySource));
            }
            for (const auto& source : (bestEntry != nullptr ? bestEntry->sources : std::vector<std::string>{})) {
                AppendUniqueString(sources, LocalizedCheatSourceLabel(state, source));
            }
            if (!preferredBuild->primarySource.empty()) {
                AppendUniqueString(sources, LocalizedCheatSourceLabel(state, preferredBuild->primarySource));
            }
            for (const auto& source : preferredBuild->sources) {
                AppendUniqueString(sources, LocalizedCheatSourceLabel(state, source));
            }
            const std::string sourceSummary = SummarizeLabelList(sources, 2);
            if (!sourceSummary.empty()) {
                parts.push_back(sourceSummary);
            }
        }
    }

    parts.push_back(CheatBuildCountLabel(state, title->builds.size()));
    return JoinLabels(parts, " • ");
}

std::string SaveCardSubtitle(const AppState& state, const CatalogEntry& entry) {
    const SaveTitleRecord* title = FindSaveTitleRecord(state.savesIndex, entry.titleId);
    if (title == nullptr) {
        return JoinLabels(EntryContentTypeLabels(state, entry), ", ");
    }

    std::vector<std::string> parts;
    if (!title->categories.empty()) {
        std::vector<std::string> labels;
        for (const std::string& category : title->categories) {
            AppendUniqueString(labels, LocalizedSaveCategoryLabel(state, category));
        }
        const std::string categorySummary = SummarizeLabelList(labels, 2);
        if (!categorySummary.empty()) {
            parts.push_back(categorySummary);
        }
    }
    parts.push_back(SaveVariantCountLabel(state, title->variants.size()));
    return JoinLabels(parts, " • ");
}

std::string InstalledCheatBuildId(const InstallReceipt& receipt) {
    for (const auto& file : receipt.files) {
        const std::size_t slash = file.find_last_of("/\\");
        const std::string filename = slash == std::string::npos ? file : file.substr(slash + 1);
        if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".txt") {
            return filename.substr(0, filename.size() - 4);
        }
    }
    return {};
}

bool ShouldConfirmInstall(const AppState& state,
                          const CatalogEntry& entry,
                          const InstalledTitle* installedTitle,
                          std::string& title,
                          std::string& message) {
    if (!SectionRequiresInstallConfirmation(entry.section)) {
        return false;
    }

    if (entry.section == ContentSection::Cheats && EntryUsesCheatsIndex(state, entry)) {
        const CheatTitleRecord* cheatTitle = FindCheatTitleRecord(state.cheatsIndex, entry.titleId);
        if (installedTitle == nullptr) {
            title = UiText(state, "Título não detectado", "Title not detected");
            message = UiText(state,
                             "O título não foi detectado no console/emulador. Escolher manualmente um build de cheats?",
                             "The title was not detected on the console/emulator. Choose a cheat build manually?");
            return true;
        }

        if (installedTitle->buildIdHex.empty()) {
            title = UiText(state, "Build Desconhecida", "Build Unknown");
            message = UiText(state,
                             "O build ID do jogo não pôde ser detectado. Escolher manualmente um build de cheats?",
                             "The game's build ID could not be detected. Choose a cheat build manually?");
            return true;
        }

        if (cheatTitle == nullptr || FindCheatBuildRecord(*cheatTitle, installedTitle->buildIdHex) == nullptr) {
            title = UiText(state, "Build diferente", "Build mismatch");
            message = std::string(UiText(state,
                                         "Não há cheats publicados para o build detectado (",
                                         "There are no published cheats for the detected build (")) +
                      installedTitle->buildIdHex + "). " +
                      UiText(state, "Escolher manualmente outro build?", "Choose another build manually?");
            return true;
        }

        return false;
    }

    if (installedTitle == nullptr) {
        title = UiText(state, "Título não detectado", "Title not detected");
        message = UiText(state,
                         "O título não foi detectado no console/emulador. Prosseguir com a instalação?",
                         "The title was not detected on the console/emulator. Continue with installation?");
        return true;
    }

    if (installedTitle->displayVersion.empty()) {
        title = UiText(state, "Versão não detectada", "Version not detected");
        message = UiText(state,
                         "A versão do jogo não pôde ser detectada. Prosseguir com a instalação?",
                         "The game version could not be detected. Continue with installation?");
        return true;
    }

    if (!EntryHasAnyCompatibilityInformation(entry)) {
        title = UiText(state, "Compatibilidade não especificada", "Compatibility unspecified");
        message = UiText(state,
                         "O pacote não possui versão compatível especificada. Prosseguir com a instalação?",
                         "This package does not specify compatible game versions. Continue with installation?");
        return true;
    }

    if (!EntryMatchesInstalledVersion(entry, installedTitle)) {
        title = UiText(state, "Versão divergente", "Version mismatch");
        message = std::string(UiText(state,
                                     "A versão detectada do jogo é diferente da compatível com o pacote (",
                                     "The detected game version differs from the package compatibility (")) +
                  installedTitle->displayVersion + "). " +
                  UiText(state, "Prosseguir com a instalação?", "Continue with installation?");
        return true;
    }

    return false;
}

void ShowInstallConfirmation(AppState& state, const CatalogEntry& entry, const std::string& title, const std::string& message) {
    state.installConfirmVisible = true;
    state.installConfirmEntryId = entry.id;
    state.installConfirmTitle = title;
    state.installConfirmMessage = message;
}

void ClearInstallConfirmation(AppState& state) {
    state.installConfirmVisible = false;
    state.installConfirmEntryId.clear();
    state.installConfirmTitle.clear();
    state.installConfirmMessage.clear();
}

void ShowVariantSelection(AppState& state,
                          const CatalogEntry& entry,
                          const std::vector<const CatalogVariant*>& variants,
                          const std::string& title,
                          const std::string& message) {
    state.variantSelectVisible = true;
    state.variantSelectEntryId = entry.id;
    state.variantSelectTitle = title;
    state.variantSelectMessage = message;
    state.variantSelectIds.clear();
    for (const CatalogVariant* variant : variants) {
        if (variant != nullptr) {
            state.variantSelectIds.push_back(variant->id);
        }
    }
    state.variantSelectSelection = 0;
}

void ClearVariantSelection(AppState& state) {
    state.variantSelectVisible = false;
    state.variantSelectEntryId.clear();
    state.variantSelectTitle.clear();
    state.variantSelectMessage.clear();
    state.variantSelectIds.clear();
    state.variantSelectSelection = 0;
}

void ShowCheatBuildSelection(AppState& state,
                             const CatalogEntry& entry,
                             const std::vector<std::string>& buildIds,
                             const std::string& title,
                             const std::string& message) {
    state.cheatBuildSelectVisible = true;
    state.cheatBuildEntryId = entry.id;
    state.cheatBuildTitleId = ToLowerAscii(entry.titleId);
    state.cheatBuildTitle = title;
    state.cheatBuildMessage = message;
    state.cheatBuildIds = buildIds;
    state.cheatBuildSelection = 0;
}

void ClearCheatBuildSelection(AppState& state) {
    state.cheatBuildSelectVisible = false;
    state.cheatBuildEntryId.clear();
    state.cheatBuildTitleId.clear();
    state.cheatBuildTitle.clear();
    state.cheatBuildMessage.clear();
    state.cheatBuildIds.clear();
    state.cheatBuildSelection = 0;
}

const CatalogVariant* SelectedVariantForPopup(const AppState& state, const CatalogEntry& entry) {
    if (state.variantSelectIds.empty()) {
        return nullptr;
    }
    const std::size_t index = std::min(state.variantSelectSelection, state.variantSelectIds.size() - 1);
    return FindVariantById(entry, state.variantSelectIds[index]);
}

std::string VariantSelectionTitle(const AppState& state) {
    return UiText(state, "Escolha a variante", "Choose variant");
}

std::string VariantSelectionMessage(const AppState& state, const InstalledTitle* installedTitle, bool fallbackToAll) {
    if (installedTitle == nullptr) {
        return UiText(state,
                      "Selecione a variante do pacote para instalar neste título.",
                      "Select which package variant should be installed for this title.");
    }
    if (installedTitle->displayVersion.empty()) {
        return UiText(state,
                      "A versão do jogo não foi detectada. Escolha manualmente a variante do pacote.",
                      "The game version was not detected. Choose the package variant manually.");
    }
    if (fallbackToAll) {
        return std::string(UiText(state,
                                  "Nenhuma variante coincide exatamente com a versão detectada (",
                                  "No variant matches the detected game version (")) +
               installedTitle->displayVersion + "). " +
               UiText(state, "Escolha como deseja prosseguir.", "Choose how to proceed.");
    }
    return std::string(UiText(state,
                              "Mais de uma variante é compatível com a versão detectada (",
                              "More than one variant is compatible with the detected game version (")) +
           installedTitle->displayVersion + "). " +
           UiText(state, "Escolha a variante desejada.", "Choose the variant to install.");
}

const CheatBuildRecord* SelectedCheatBuildForPopup(const AppState& state, const CheatsIndex& index, const CatalogEntry& entry) {
    if (state.cheatBuildIds.empty()) {
        return nullptr;
    }
    const CheatTitleRecord* title = FindCheatTitleRecord(index, entry.titleId);
    if (title == nullptr) {
        return nullptr;
    }
    const std::size_t indexValue = std::min(state.cheatBuildSelection, state.cheatBuildIds.size() - 1);
    return FindCheatBuildRecord(*title, state.cheatBuildIds[indexValue]);
}

const CatalogEntry* FindCheatBuildEntryForPopup(const AppState& state) {
    if (const CatalogEntry* entry = FindVisibleEntryById(state, state.cheatBuildEntryId)) {
        return entry;
    }

    const std::string normalizedTitleId = ToLowerAscii(state.cheatBuildTitleId);
    for (const auto& entry : state.derivedCheatEntries) {
        if (entry.section == ContentSection::Cheats && ToLowerAscii(entry.titleId) == normalizedTitleId) {
            return &entry;
        }
    }

    return nullptr;
}

std::string CheatBuildSelectionTitle(const AppState& state) {
    return UiText(state, "Escolha o build do cheat", "Choose cheat build");
}

std::string CheatBuildSelectionMessage(const AppState& state, const InstalledTitle* installedTitle, bool fallbackToAll) {
    if (installedTitle == nullptr) {
        return UiText(state,
                      "Selecione manualmente o build de cheats para este título.",
                      "Select the cheat build manually for this title.");
    }
    if (installedTitle->buildIdHex.empty()) {
        return UiText(state,
                      "O build ID do jogo não foi detectado. Escolha manualmente o build de cheats.",
                      "The game's build ID was not detected. Choose the cheat build manually.");
    }
    if (fallbackToAll) {
        return std::string(UiText(state,
                                  "Nenhum cheat coincide com o build detectado (",
                                  "No cheat matches the detected build (")) +
               installedTitle->buildIdHex + "). " +
               UiText(state, "Escolha como deseja prosseguir.", "Choose how to proceed.");
    }
    return std::string(UiText(state,
                              "Mais de um conjunto de cheats está disponível para o build detectado (",
                              "More than one cheat set is available for the detected build (")) +
           installedTitle->buildIdHex + "). " +
           UiText(state, "Escolha o build desejado.", "Choose the desired build.");
}

bool InstallResolvedEntry(AppState& state, const CatalogEntry& entry, const CatalogVariant* variant) {
    const InstalledTitle* installedTitle = FindInstalledTitle(state.installedTitles, entry.titleId);
    CatalogEntry resolvedEntry = ResolveEntryForVariant(entry, variant);
    if (variant != nullptr && resolvedEntry.downloadUrl.empty()) {
        resolvedEntry.downloadUrl = ResolveCatalogVariantDownloadUrl(state, *variant);
    }
    if (resolvedEntry.downloadUrl.empty()) {
        resolvedEntry.downloadUrl = ResolveCatalogEntryDownloadUrl(state, resolvedEntry);
    }

    if (entry.section == ContentSection::SaveGames) {
        const SaveTitleRecord* saveTitle = FindSaveTitleRecord(state.savesIndex, entry.titleId);
        const SaveVariantRecord* saveVariant = nullptr;
        if (saveTitle != nullptr) {
            if (variant != nullptr) {
                saveVariant = FindSaveVariantRecord(*saveTitle, variant->id);
            }
            if (saveVariant == nullptr && saveTitle->variants.size() == 1) {
                saveVariant = &saveTitle->variants.front();
            }
        }

        if (saveVariant == nullptr) {
            state.statusLine = UiText(state,
                                      "Nenhuma variante de save válida foi encontrada para instalação.",
                                      "No valid save variant was found for installation.");
            return false;
        }

        SetProgress(state,
                    UiString(state, "ui.progress.installing_save", u8"Aplicando save", "Applying save"),
                    entry.name,
                    15);
        UiDownloadProgressContext downloadProgress{
            &state,
            UiString(state, "ui.progress.installing_save", u8"Aplicando save", "Applying save"),
            u8"Baixando save...",
            "Downloading save...",
            15,
            55};

        InstallReceipt newReceipt;
        std::string error;
        SaveVariantRecord resolvedSaveVariant = *saveVariant;
        if (resolvedSaveVariant.contentHash.empty()) {
            resolvedSaveVariant.contentHash = resolvedSaveVariant.sha256;
        }
        if (resolvedSaveVariant.downloadUrl.empty()) {
            resolvedSaveVariant.downloadUrl = ResolveSaveVariantDownloadUrl(state, resolvedSaveVariant);
        }
        if (resolvedSaveVariant.downloadUrl.empty()) {
            state.statusLine = UiText(state,
                                      "Nenhum download disponível para esta entrada/variante.",
                                      "No download is available for this entry/variant.");
            return false;
        }

        if (InstallSaveData(entry,
                            resolvedSaveVariant,
                            installedTitle,
                            newReceipt,
                            error,
                            UpdateUiDownloadProgress,
                            &downloadProgress)) {
            SetProgress(state,
                        UiString(state, "ui.progress.installing_save", u8"Aplicando save", "Applying save"),
                        UiString(state,
                                 "ui.progress.save_backup_restore",
                                 u8"Backup salvo e variante aplicada.",
                                 "Backup saved and variant applied."),
                        100);
            state.statusLine = std::string(UseEnglish(state) ? "Save applied: " : "Save aplicado: ") + entry.name;
            std::string note;
            state.receipts = LoadInstallReceipts(note);
            ClearProgress(state);
            return true;
        }

        state.statusLine = std::string(UseEnglish(state) ? "Save installation failed: " : "Falha na instalacao do save: ") + error;
        ClearProgress(state);
        return false;
    }

    if (resolvedEntry.downloadUrl.empty()) {
        state.statusLine = UiText(state,
                                  "Nenhum download disponível para esta entrada/variante.",
                                  "No download is available for this entry/variant.");
        return false;
    }

    SetProgress(state,
                UiText(state, u8"Instalando pacote", "Installing package"),
                entry.name,
                15);
    InstallReceipt newReceipt;
    UiDownloadProgressContext downloadProgress{
        &state,
        UiText(state, u8"Instalando pacote", "Installing package"),
        u8"Baixando pacote...",
        "Downloading package...",
        15,
        65};

    std::string error;
    if (InstallPackage(resolvedEntry, installedTitle, newReceipt, error, UpdateUiDownloadProgress, &downloadProgress)) {
        SetProgress(state,
                    UiText(state, u8"Instalando pacote", "Installing package"),
                    UiText(state, u8"Extraindo pacote...", "Extracting package..."),
                    90);
        SetProgress(state,
                    UiText(state, u8"Instalando pacote", "Installing package"),
                    entry.name,
                    100);
        state.statusLine = std::string(UseEnglish(state) ? "Package installed: " : "Pacote instalado: ") + entry.name;
        std::string note;
        state.receipts = LoadInstallReceipts(note);
        ClearProgress(state);
        return true;
    }

    state.statusLine = std::string(UseEnglish(state) ? "Installation failed: " : "Falha na instalação: ") + error;
    ClearProgress(state);
    return false;
}

bool InstallCheatBuild(AppState& state, const CatalogEntry& entry, const CheatBuildRecord& build) {
    const CheatEntryRecord* cheatEntry = BestCheatEntryForBuild(build);
    if (cheatEntry == nullptr && build.downloadUrl.empty() && build.relativePath.empty()) {
        state.statusLine = UiText(state,
                                  "Nenhum arquivo de cheat disponível para este build.",
                                  "No cheat file is available for this build.");
        return false;
    }

    InstallReceipt existingReceipt;
    if (FindReceiptForPackage(state.receipts, entry.id, &existingReceipt)) {
        std::string uninstallError;
        UninstallPackage(existingReceipt, uninstallError);
    }

    const InstalledTitle* installedTitle = FindInstalledTitle(state.installedTitles, entry.titleId);
    SetProgress(state,
                UiText(state, u8"Instalando trapaça", "Installing cheat"),
                build.buildId,
                20);
    InstallReceipt newReceipt;
    UiDownloadProgressContext downloadProgress{
        &state,
        UiText(state, u8"Instalando trapaça", "Installing cheat"),
        u8"Baixando trapaça...",
        "Downloading cheat...",
        20,
        70};

    std::string error;
    bool installed = false;

    std::string localCheatPath;
    if (!build.relativePath.empty()) {
        localCheatPath = std::string(kCacheDir) + "/" + build.relativePath;
    } else if (cheatEntry != nullptr && !cheatEntry->relativePath.empty()) {
        localCheatPath = std::string(kCacheDir) + "/" + cheatEntry->relativePath;
    }

    if (!localCheatPath.empty() && FileHasContent(localCheatPath)) {
        installed = InstallCheatTextFromFile(entry, build.buildId, localCheatPath, installedTitle, newReceipt, error);
    }

    std::string downloadUrl = ResolveCheatBuildDownloadUrl(state, build);
    if (downloadUrl.empty() && cheatEntry != nullptr) {
        downloadUrl = ResolveCheatEntryDownloadUrl(state, *cheatEntry);
    }

    if (!installed && !downloadUrl.empty()) {
        installed = InstallCheatText(entry,
                                     build.buildId,
                                     downloadUrl,
                                     installedTitle,
                                     newReceipt,
                                     error,
                                     UpdateUiDownloadProgress,
                                     &downloadProgress);
    }

    if (installed) {
        SetProgress(state,
                    UiText(state, u8"Instalando trapaça", "Installing cheat"),
                    build.buildId,
                    100);
        state.statusLine = UiString(state,
                                    "ui.status.cheat_installed_for_build_prefix",
                                    "",
                                    "") +
                           build.buildId;
        std::string note;
        state.receipts = LoadInstallReceipts(note);
        ClearProgress(state);
        return true;
    }

    state.statusLine = std::string(UseEnglish(state) ? "Cheat installation failed: " : u8"Falha na instalação da trapaça: ") + error;
    ClearProgress(state);
    return false;
}

bool BeginCheatIndexInstall(AppState& state,
                            const CatalogEntry& entry,
                            const InstalledTitle* installedTitle,
                            bool allowFallbackToAllBuilds) {
    if (!EntryUsesCheatsIndex(state, entry)) {
        return InstallResolvedEntry(state, entry, nullptr);
    }

    const CheatTitleRecord* title = FindCheatTitleRecord(state.cheatsIndex, entry.titleId);
    if (title == nullptr || title->builds.empty()) {
        state.statusLine = UiText(state,
                                  "Nenhum cheat publicado foi encontrado para este título.",
                                  "No published cheats were found for this title.");
        return false;
    }

    if (installedTitle != nullptr && !installedTitle->buildIdHex.empty()) {
        if (const CheatBuildRecord* exactBuild = FindCheatBuildRecord(*title, installedTitle->buildIdHex)) {
            return InstallCheatBuild(state, entry, *exactBuild);
        }
    }

    if (!allowFallbackToAllBuilds) {
        state.statusLine = UiText(state,
                                  "Cheat depende de build específico; confirmação necessária para escolher manualmente.",
                                  "Cheat depends on a specific build; confirmation is required to choose manually.");
        return false;
    }

    ShowCheatBuildSelection(state,
                            entry,
                            CollectCheatBuildIds(*title),
                            CheatBuildSelectionTitle(state),
                            CheatBuildSelectionMessage(state, installedTitle, true));
    return false;
}

bool BeginVariantAwareInstall(AppState& state,
                              const CatalogEntry& entry,
                              const InstalledTitle* installedTitle,
                              bool allowFallbackToAllVariants) {
    if (!EntryHasVariants(entry)) {
        return InstallResolvedEntry(state, entry, nullptr);
    }

    std::vector<const CatalogVariant*> candidates = FindCompatibleVariants(entry, installedTitle);
    if (candidates.empty() && allowFallbackToAllVariants) {
        candidates = CollectAllVariants(entry);
    }

    if (candidates.empty()) {
        state.statusLine = UiText(state,
                                  "Nenhuma variante compatível ou selecionável foi encontrada.",
                                  "No compatible or selectable variant was found.");
        return false;
    }

    if (candidates.size() == 1) {
        return InstallResolvedEntry(state, entry, candidates.front());
    }

    ShowVariantSelection(state,
                         entry,
                         candidates,
                         VariantSelectionTitle(state),
                         VariantSelectionMessage(state, installedTitle, allowFallbackToAllVariants));
    return false;
}

std::string NormalizeFooterStatus(std::string text) {
    const auto replaceAll = [&](const std::string& from, const std::string& to) {
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    };

    replaceAll("\r\n", u8" • ");
    replaceAll("\n", u8" • ");
    replaceAll("\r", u8" • ");
    replaceAll("Ambiente não confirmado. Modo automático segue em fallback seguro até detectar console com confiança.",
               "Modo automático segue em fallback");
    replaceAll("Unconfirmed environment. Auto mode remains in safe fallback until a console is detected with confidence.",
               "Auto mode stays in fallback");
    return text;
}

bool UseDarkTheme(const AppState& state) {
    return state.config.theme == ThemeMode::Dark;
}

bool IsRyujinxGuestEnvironment() {
    return ToLowerAscii(GetLoaderInfoSummary()).find("ryujinx") != std::string::npos;
}

std::string ThemeModeLabelLocalized(const AppState& state, ThemeMode mode) {
    if (!UseEnglish(state)) {
        return mode == ThemeMode::Dark ? "Escuro" : "Claro";
    }
    return mode == ThemeMode::Dark ? "Dark" : "Light";
}

ThemePalette ApplyUnifiedBorders(ThemePalette palette, std::uint32_t border = gfx::Rgba(175, 148, 67)) {
    palette.sidebarBorder = border;
    palette.sidebarBorderFocused = border;
    palette.sidebarItemBorder = border;
    palette.sidebarItemBorderSelected = border;
    palette.actionBorder = border;
    palette.actionBorderActive = border;
    palette.statusBorder = border;
    palette.contentPanelBorder = border;
    palette.contentPanelBorderFocused = border;
    palette.emptyBorder = border;
    palette.entryBorder = border;
    palette.entryBorderSelected = border;
    palette.coverBorder = border;
    palette.detailsBorder = border;
    palette.buttonPrimaryBorder = border;
    palette.buttonPrimaryBorderActive = border;
    palette.buttonSecondaryBorder = border;
    palette.footerBorder = border;
    return palette;
}

ThemePalette ApplyLightSurfaceTone(ThemePalette palette) {
    const std::uint32_t tone = gfx::Rgba(255, 252, 244);
    palette.entryFill = tone;
    palette.statusFill = tone;
    return palette;
}

ThemePalette ApplyRequestedLightPalette(ThemePalette palette) {
    const std::uint32_t text = gfx::Rgba(45, 45, 45);
    const std::uint32_t selected = gfx::Rgba(226, 190, 96);
    const std::uint32_t pale = gfx::Rgba(250, 242, 211);
    const std::uint32_t status = gfx::Rgba(250, 230, 200);
    const std::uint32_t background = gfx::Rgba(240, 240, 240);
    const std::uint32_t muted = gfx::Rgba(96, 96, 96);
    const std::uint32_t warning = gfx::Rgba(201, 140, 74);

    palette.windowTop = background;
    palette.windowBottom = background;
    palette.windowBase = background;
    palette.sidebarFill = background;
    palette.sidebarHeaderFill = background;
    palette.contentPanelFill = background;
    palette.contentHeaderFill = background;
    palette.detailsFill = background;
    palette.emptyFill = background;
    palette.coverFill = background;

    palette.sidebarItemFill = pale;
    palette.sidebarItemFillSelected = selected;
    palette.entryFill = pale;
    palette.entryFillSelected = selected;
    palette.actionFill = pale;
    palette.buttonPrimaryFill = pale;
    palette.buttonSecondaryFill = pale;

    palette.statusFill = status;
    palette.footerFill = status;
    palette.chipSuggestedFill = gfx::Rgba(118, 111, 72);
    palette.chipInstalledFill = gfx::Rgba(87, 116, 81);
    palette.chipNeutralFill = gfx::Rgba(103, 103, 103);

    palette.sidebarItemText = text;
    palette.sidebarItemTextSelected = text;
    palette.headerText = text;
    palette.headerMetaText = text;
    palette.primaryText = text;
    palette.secondaryText = text;
    palette.accentText = text;
    palette.actionText = text;
    palette.statusText = text;
    palette.footerText = text;
    palette.buttonTextPrimary = text;
    palette.buttonTextSecondary = text;
    palette.actionBadgeText = text;
    palette.buttonBadgeTextPrimary = text;
    palette.buttonBadgeTextSecondary = text;
    palette.chipSuggestedText = gfx::Rgba(244, 236, 204);
    palette.chipInstalledText = gfx::Rgba(226, 240, 222);
    palette.chipNeutralText = gfx::Rgba(244, 244, 244);
    palette.mutedText = muted;
    palette.warningText = warning;

    palette.entryAccent = gfx::Rgba(88, 88, 88);
    palette.entryAccentSelected = gfx::Rgba(88, 88, 88);
    palette.actionBadgeOuter = palette.actionBadgeInner;
    palette.actionBadgeOuterActive = palette.actionBadgeInner;
    palette.buttonBadgeOuter = palette.buttonBadgeInnerPrimary;

    return palette;
}

ThemePalette ApplySharedThemeFinishing(ThemePalette palette) {
    const std::uint32_t gold = gfx::Rgba(175, 148, 67);
    const std::uint32_t headerText = gfx::Rgba(45, 45, 45);

    palette = ApplyUnifiedBorders(palette, gold);
    palette.sidebarHeaderFill = gold;
    palette.contentHeaderFill = gold;
    palette.headerText = headerText;
    palette.headerMetaText = headerText;
    palette.entryFill = palette.sidebarItemFill;
    palette.entryFillSelected = palette.sidebarItemFillSelected;
    palette.entryAccentSelected = palette.entryAccent;
    return palette;
}

ThemePalette GetThemePalette(const AppState& state) {
    if (UseDarkTheme(state)) {
        return ApplySharedThemeFinishing(ApplyUnifiedBorders({
            gfx::Rgba(15, 15, 14),
            gfx::Rgba(8, 8, 8),
            gfx::Rgba(11, 11, 10),
            gfx::Rgba(16, 17, 16),
            gfx::Rgba(123, 111, 77),
            gfx::Rgba(201, 174, 98),
            gfx::Rgba(6, 6, 6),
            gfx::Rgba(233, 221, 189),
            gfx::Rgba(210, 194, 150),
            gfx::Rgba(38, 39, 41),
            gfx::Rgba(74, 75, 78),
            gfx::Rgba(226, 190, 96),
            gfx::Rgba(239, 211, 138),
            gfx::Rgba(228, 220, 202),
            gfx::Rgba(42, 33, 12),
            gfx::Rgba(39, 40, 42),
            gfx::Rgba(77, 78, 82),
            gfx::Rgba(199, 173, 101),
            gfx::Rgba(210, 184, 111),
            gfx::Rgba(226, 190, 96),
            gfx::Rgba(44, 34, 13),
            gfx::Rgba(232, 221, 193),
            gfx::Rgba(228, 220, 202),
            gfx::Rgba(39, 40, 42),
            gfx::Rgba(79, 80, 84),
            gfx::Rgba(228, 220, 202),
            gfx::Rgba(12, 12, 12),
            gfx::Rgba(123, 111, 77),
            gfx::Rgba(201, 174, 98),
            gfx::Rgba(6, 6, 6),
            gfx::Rgba(41, 42, 44),
            gfx::Rgba(76, 77, 81),
            gfx::Rgba(61, 62, 66),
            gfx::Rgba(70, 71, 75),
            gfx::Rgba(127, 115, 80),
            gfx::Rgba(225, 190, 98),
            gfx::Rgba(226, 190, 96),
            gfx::Rgba(240, 206, 120),
            gfx::Rgba(34, 34, 34),
            gfx::Rgba(88, 89, 91),
            gfx::Rgba(12, 12, 12),
            gfx::Rgba(123, 111, 77),
            gfx::Rgba(238, 233, 224),
            gfx::Rgba(208, 202, 190),
            gfx::Rgba(176, 170, 158),
            gfx::Rgba(212, 185, 109),
            gfx::Rgba(212, 151, 83),
            gfx::Rgba(87, 86, 62),
            gfx::Rgba(244, 236, 204),
            gfx::Rgba(67, 89, 70),
            gfx::Rgba(212, 240, 208),
            gfx::Rgba(80, 81, 83),
            gfx::Rgba(224, 220, 212),
            gfx::Rgba(48, 49, 52),
            gfx::Rgba(188, 164, 96),
            gfx::Rgba(225, 190, 98),
            gfx::Rgba(43, 44, 46),
            gfx::Rgba(92, 93, 96),
            gfx::Rgba(240, 235, 226),
            gfx::Rgba(232, 227, 219),
            gfx::Rgba(213, 182, 103),
            gfx::Rgba(225, 190, 96),
            gfx::Rgba(225, 190, 96),
            gfx::Rgba(51, 38, 13),
            gfx::Rgba(51, 38, 13),
            gfx::Rgba(26, 27, 28),
            gfx::Rgba(123, 111, 77),
            gfx::Rgba(228, 220, 202),
        }));
    }

    return ApplySharedThemeFinishing(ApplyRequestedLightPalette(ApplyLightSurfaceTone({
        gfx::Rgba(252, 249, 241),
        gfx::Rgba(243, 237, 225),
        gfx::Rgba(248, 244, 236),
        gfx::Rgba(246, 247, 248),
        gfx::Rgba(224, 214, 193),
        gfx::Rgba(212, 182, 100),
        gfx::Rgba(237, 241, 245),
        gfx::Rgba(41, 39, 36),
        gfx::Rgba(74, 70, 61),
        gfx::Rgba(241, 244, 247),
        gfx::Rgba(219, 223, 228),
        gfx::Rgba(237, 196, 86),
        gfx::Rgba(219, 184, 84),
        gfx::Rgba(51, 50, 47),
        gfx::Rgba(62, 50, 20),
        gfx::Rgba(240, 243, 246),
        gfx::Rgba(214, 218, 223),
        gfx::Rgba(212, 182, 100),
        gfx::Rgba(230, 205, 133),
        gfx::Rgba(232, 194, 86),
        gfx::Rgba(255, 244, 209),
        gfx::Rgba(124, 91, 24),
        gfx::Rgba(55, 53, 49),
        gfx::Rgba(241, 244, 247),
        gfx::Rgba(214, 218, 223),
        gfx::Rgba(55, 53, 49),
        gfx::Rgba(253, 252, 249),
        gfx::Rgba(224, 214, 193),
        gfx::Rgba(212, 182, 100),
        gfx::Rgba(239, 243, 246),
        gfx::Rgba(251, 251, 249),
        gfx::Rgba(226, 229, 233),
        gfx::Rgba(255, 255, 255),
        gfx::Rgba(255, 252, 244),
        gfx::Rgba(224, 214, 193),
        gfx::Rgba(232, 194, 86),
        gfx::Rgba(232, 194, 86),
        gfx::Rgba(244, 210, 115),
        gfx::Rgba(241, 243, 245),
        gfx::Rgba(220, 222, 226),
        gfx::Rgba(255, 255, 255),
        gfx::Rgba(226, 229, 233),
        gfx::Rgba(42, 40, 36),
        gfx::Rgba(89, 84, 77),
        gfx::Rgba(118, 112, 103),
        gfx::Rgba(143, 110, 43),
        gfx::Rgba(185, 118, 59),
        gfx::Rgba(238, 235, 214),
        gfx::Rgba(116, 98, 43),
        gfx::Rgba(219, 234, 218),
        gfx::Rgba(52, 92, 58),
        gfx::Rgba(235, 236, 238),
        gfx::Rgba(86, 84, 80),
        gfx::Rgba(245, 247, 249),
        gfx::Rgba(218, 184, 83),
        gfx::Rgba(232, 194, 86),
        gfx::Rgba(241, 244, 247),
        gfx::Rgba(214, 218, 223),
        gfx::Rgba(52, 50, 46),
        gfx::Rgba(52, 50, 46),
        gfx::Rgba(230, 205, 133),
        gfx::Rgba(255, 243, 207),
        gfx::Rgba(255, 243, 207),
        gfx::Rgba(124, 91, 24),
        gfx::Rgba(124, 91, 24),
        gfx::Rgba(240, 243, 246),
        gfx::Rgba(220, 210, 190),
        gfx::Rgba(52, 50, 46),
    })));
}

std::string LocalizePlatformNote(const AppState& state, const std::string& note) {
    if (!UseEnglish(state) || note.empty()) {
        return note;
    }

    if (note == "Biblioteca do emulador não fica visível ao homebrew. Sincronize os títulos para sdmc:/switch/mil_manager/cache/installed-titles-cache.json.") {
        return UiText(state, u8"Biblioteca do emulador n?o fica vis?vel ao homebrew. Sincronize os t?tulos para sdmc:/switch/mil_manager/cache/installed-titles-cache.json.", "The emulator library is not visible to homebrew. Sync titles to sdmc:/switch/mil_manager/cache/installed-titles-cache.json.");
    }
    if (note == "Arquivo installed-titles-cache.json inválido.") {
        return UiText(state, u8"Arquivo installed-titles-cache.json inv?lido.", "Invalid installed-titles-cache.json file.");
    }
    if (note == "Arquivo installed-titles-cache.json sem array titles.") {
        return UiText(state, u8"Arquivo installed-titles-cache.json sem array titles.", "installed-titles-cache.json does not contain a titles array.");
    }
    if (note == "Títulos importados de installed-titles-cache.json.") {
        return UiText(state, u8"T?tulos importados de installed-titles-cache.json.", "Titles imported from installed-titles-cache.json.");
    }
    if (note == "nsInitialize falhou. Serviço NS indisponível.") {
        return UiText(state, u8"nsInitialize falhou. Servi?o NS indispon?vel.", "nsInitialize failed. NS service unavailable.");
    }
    if (note == "nsListApplicationRecord falhou no modo full.") {
        return UiText(state, u8"nsListApplicationRecord falhou no modo full.", "nsListApplicationRecord failed in full mode.");
    }
    if (note == u8"Títulos instalados carregados por varredura completa." ||
        note == "Títulos instalados carregados por scan completo." ||
        note == "Títulos instalados carregados por busca completa." ||
        note == "TÃ­tulos instalados carregados por scan completo.") {
        return UiString(state,
                        "installed_titles_loaded_using_full_scan",
                        "",
                        "");
    }
    if (note == "Catálogo sem title IDs para sondagem local.") {
        return UiText(state, u8"Cat?logo sem title IDs para sondagem local.", "Catalog has no title IDs for local probing.");
    }
    if (note == "nsInitialize falhou. Emulador ou serviço NS indisponível.") {
        return UiText(state, u8"nsInitialize falhou. Emulador ou servi?o NS indispon?vel.", "nsInitialize failed. Emulator or NS service unavailable.");
    }
    if (note == "Títulos detectados por sondagem do catálogo.") {
        return UiText(state, u8"T?tulos detectados por sondagem do cat?logo.", "Titles detected by catalog probing.");
    }
    if (note == u8"Console detectado. Leitura local sempre ativa; ignorando scan_mode=off.") {
        return UiString(state,
                        "console_detected_local_reading_always_enabled_ignoring_scan_mode_off",
                        u8"Console detectado. Leitura local sempre ativa; ignorando busca desativada.",
                        "Console detected. Local reading always enabled; ignoring disabled search.");
    }
    return note;
}

std::string SortModeLabel(const AppState& state, SortMode mode) {
    switch (mode) {
        case SortMode::Recent:
            return UiString(state, "ui.sort.recent", "Recentes", "Recent");
        case SortMode::Name:
            return UiString(state, "ui.sort.name", "Nome", "Name");
        case SortMode::Recommended:
        default:
            return UiString(state, "ui.sort.recommended", u8"Relev?ncia", "Recommended");
    }
}

std::string GetCatalogSourceLabel(const AppState& state) {
    const std::string* activeSource = &state.activeCatalogSource;
    if (state.section == ContentSection::Cheats && !state.activeCheatsSource.empty()) {
        activeSource = &state.activeCheatsSource;
    } else if (state.section == ContentSection::SaveGames && !state.activeSavesSource.empty()) {
        activeSource = &state.activeSavesSource;
    }

    if (activeSource->empty()) {
        return UiString(state, "ui.source.none", "Nenhuma fonte ativa", "No active source");
    }
    if (*activeSource == kSwitchLocalIndexPath || *activeSource == kSwitchLocalCheatsIndexPath ||
        *activeSource == kSwitchLocalSavesIndexPath) {
        return UiString(state, "ui.source.emulator_sync", u8"Cat?logo sincronizado do emulador", "Emulator synchronized catalog");
    }
    if (*activeSource == kCatalogCachePath || *activeSource == kCheatsIndexCachePath || *activeSource == kSavesIndexCachePath) {
        return UiString(state, "ui.source.cache", u8"Cache local do cat?logo", "Local catalog cache");
    }
    if (activeSource->rfind("http://", 0) == 0 || activeSource->rfind("https://", 0) == 0) {
        return UiString(state, "ui.source.online", u8"Cat?logo online", "Online catalog");
    }
    return UiString(state, "ui.source.local", u8"Cat?logo local", "Local catalog");
}

std::string SectionLabelLocalized(const AppState& state, ContentSection section) {
    switch (section) {
        case ContentSection::Translations:
            return UiString(state, "ui.section.translations", u8"Tradu??es & Dublagens", "Translations & Dubs");
        case ContentSection::ModsTools:
            return UiString(state, "ui.section.mods", "Mods", "Mods");
        case ContentSection::Cheats:
            return UiString(state, "ui.section.cheats", u8"Trapa?as", "Cheats");
        case ContentSection::SaveGames:
            return UiString(state, "ui.section.saves", "Jogos Salvos", "Save Games");
        case ContentSection::About:
        default:
            return UiString(state, "ui.section.about", "Sobre a M.I.L.", "About M.I.L.");
    }
}

std::string MakeCompatibilitySummaryLocalized(const AppState& state, const CatalogEntry& entry, const InstalledTitle* title) {
    const std::string variantSummary = EntryHasVariants(entry) ? VariantListSummary(entry) : std::string();
    if (!title) {
        if (!variantSummary.empty()) {
            return UiString(state,
                            "ui.compatibility.game_not_found_with_variants",
                            u8"Jogo não encontrado no console/emulador. Variantes disponíveis: ",
                            "Game not found on the console/emulator. Available variants: ") +
                   variantSummary;
        }
        return UiString(state,
                        "ui.compatibility.game_not_found",
                        u8"Jogo não encontrado no console/emulador.",
                        "Game not found on the console/emulator.");
    }
    if (title->displayVersion.empty()) {
        if (!variantSummary.empty()) {
            return UiString(state,
                            "ui.compatibility.version_unavailable_with_variants",
                            u8"Versão do jogo indisponível. Variantes disponíveis: ",
                            "Installed game version unavailable. Available variants: ") +
                   variantSummary;
        }
        return UiString(state,
                        "ui.compatibility.version_unavailable",
                        u8"Versão do jogo indisponível.",
                        "Installed game version unavailable.");
    }
    if (EntryMatchesInstalledVersion(entry, title)) {
        return UiString(state,
                        "ui.compatibility.compatible_with_installed_version",
                        u8"Compatível com a versão instalada: ",
                        "Compatible with installed version: ") +
               title->displayVersion;
    }

    std::string message = UiString(state,
                                   "warning_package_is_outside_the_supported_range_for_the_installed_game",
                                   u8"Aten??o: pacote fora da faixa suportada para o jogo instalado (",
                                   "Warning: package is outside the supported range for the installed game (") +
                          title->displayVersion + ").";
    if (!variantSummary.empty()) {
        message += UiText(state, u8" Variantes dispon?veis: ", " Available variants: ") + variantSummary + ".";
    } else {
        if (!entry.compatibility.minGameVersion.empty()) {
            message += UiText(state, " Min: ", " Min: ") + entry.compatibility.minGameVersion + ".";
        }
        if (!entry.compatibility.maxGameVersion.empty()) {
            message += UiText(state, " Max: ", " Max: ") + entry.compatibility.maxGameVersion + ".";
        }
        if (!entry.compatibility.exactGameVersions.empty()) {
            message += UiText(state, u8" Exatas: ", " Exact: ");
            for (std::size_t index = 0; index < entry.compatibility.exactGameVersions.size(); ++index) {
                if (index > 0) {
                    message += ", ";
                }
                message += entry.compatibility.exactGameVersions[index];
            }
            message += '.';
        }
    }
    return message;
}

std::string MakeCheatAvailabilitySummaryLocalized(const AppState& state,
                                                  const CatalogEntry& entry,
                                                  const InstalledTitle* installedTitle) {
    const CheatTitleRecord* cheatTitle = FindCheatTitleRecord(state.cheatsIndex, entry.titleId);
    if (cheatTitle == nullptr) {
        return UiText(state,
                      "Nenhum índice de cheats publicado foi encontrado para este título.",
                      "No published cheats index was found for this title.");
    }
    if (installedTitle == nullptr) {
        return UiText(state,
                      "Título não detectado no console/emulador. A instalação vai pedir a escolha manual de um build.",
                      "Title not detected on the console/emulator. Installation will require choosing a build manually.");
    }
    if (installedTitle->buildIdHex.empty()) {
        return UiText(state,
                      "Build do jogo não detectado. A instalação vai pedir a escolha manual de um build.",
                      "Game build not detected. Installation will require choosing a build manually.");
    }
    if (const CheatBuildRecord* build = FindCheatBuildRecord(*cheatTitle, installedTitle->buildIdHex)) {
        const CheatEntryRecord* best = BestCheatEntryForBuild(*build);
        std::string message = std::string(UiText(state, "Cheat publicado para o build detectado: ", "Cheat published for detected build: ")) +
                              build->buildId;
        const std::string source = best != nullptr && !best->primarySource.empty() ? best->primarySource : build->primarySource;
        if (!source.empty()) {
            message += std::string(" • ") + UiText(state, "Origem: ", "Source: ") +
                       LocalizedCheatSourceLabel(state, source);
        }
        return message;
    }
    return std::string(UiText(state, "Nenhum cheat publicado para o build detectado: ", "No published cheat for detected build: ")) +
           installedTitle->buildIdHex;
}

std::string MakeSaveAvailabilitySummaryLocalized(const AppState& state,
                                                 const CatalogEntry& entry,
                                                 const InstalledTitle* installedTitle) {
    const std::string variantsSummary = EntryHasVariants(entry) ? VariantListSummary(entry) : std::string();
    if (installedTitle != nullptr) {
        return UiString(state,
                        "ui.save.summary.install_remove",
                        u8"A instalação faz backup do save atual e aplica a variante selecionada. Ao remover, o backup é restaurado.",
                        "Installing backs up the current save and applies the selected variant. Removing restores the backup.");
    }
    if (!variantsSummary.empty()) {
        return UiString(state,
                        "ui.save.summary.install_remove_variants",
                        u8"A instalação faz backup do save atual e aplica a variante selecionada. Variantes disponíveis: ",
                        "Installing backs up the current save and applies the selected variant. Available variants: ") +
               variantsSummary;
    }
    return UiString(state,
                    "ui.save.summary.install_remove",
                    u8"A instalação faz backup do save atual e aplica a variante selecionada. Ao remover, o backup é restaurado.",
                    "Installing backs up the current save and applies the selected variant. Removing restores the backup.");
}

std::string TruncateText(const std::string& text, std::size_t maxChars) {
    if (Utf8GlyphCountLocal(text) <= maxChars) {
        return text;
    }
    if (maxChars <= 3) {
        return text.substr(0, Utf8ByteOffsetForGlyphLocal(text, maxChars));
    }
    return text.substr(0, Utf8ByteOffsetForGlyphLocal(text, maxChars - 3)) + "...";
}

std::string TruncateTextToWidth(const std::string& text, int maxWidth, int scale = 1) {
    if (text.empty() || maxWidth <= 0) {
        return {};
    }

    if (gfx::MeasureTextWidth(text, scale) <= maxWidth) {
        return text;
    }

    constexpr const char* kEllipsis = "...";
    const int ellipsisWidth = gfx::MeasureTextWidth(kEllipsis, scale);
    if (ellipsisWidth >= maxWidth) {
        return {};
    }

    const std::size_t glyphCount = Utf8GlyphCountLocal(text);
    for (std::size_t glyphs = glyphCount; glyphs > 0; --glyphs) {
        const std::size_t byteOffset = Utf8ByteOffsetForGlyphLocal(text, glyphs);
        const std::string candidate = text.substr(0, byteOffset) + kEllipsis;
        if (gfx::MeasureTextWidth(candidate, scale) <= maxWidth) {
            return candidate;
        }
    }

    return kEllipsis;
}

std::string TruncateTextToWidthPx(const std::string& text, int maxWidth, int pixelHeight) {
    if (text.empty() || maxWidth <= 0) {
        return {};
    }

    if (gfx::MeasureTextWidthPx(text, pixelHeight) <= maxWidth) {
        return text;
    }

    constexpr const char* kEllipsis = "...";
    const int ellipsisWidth = gfx::MeasureTextWidthPx(kEllipsis, pixelHeight);
    if (ellipsisWidth >= maxWidth) {
        return {};
    }

    const std::size_t glyphCount = Utf8GlyphCountLocal(text);
    for (std::size_t glyphs = glyphCount; glyphs > 0; --glyphs) {
        const std::size_t byteOffset = Utf8ByteOffsetForGlyphLocal(text, glyphs);
        const std::string candidate = text.substr(0, byteOffset) + kEllipsis;
        if (gfx::MeasureTextWidthPx(candidate, pixelHeight) <= maxWidth) {
            return candidate;
        }
    }

    return kEllipsis;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        if (newline == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, newline - start));
        start = newline + 1;
    }
    if (lines.empty()) {
        lines.push_back({});
    }
    return lines;
}

void DrawPanel(gfx::Canvas& canvas, int x, int y, int width, int height, std::uint32_t fill, std::uint32_t border) {
    gfx::FillRect(canvas, x, y, width, height, fill);
    gfx::DrawRect(canvas, x, y, width, height, border);
}

void FillRoundedRect(gfx::Canvas& canvas, int x, int y, int width, int height, int radius, std::uint32_t color) {
    if (width <= 0 || height <= 0) {
        return;
    }

    const int clampedRadius = std::max(0, std::min(radius, std::min(width, height) / 2));
    if (clampedRadius == 0) {
        gfx::FillRect(canvas, x, y, width, height, color);
        return;
    }

    gfx::FillRect(canvas, x + clampedRadius, y, width - (clampedRadius * 2), height, color);
    gfx::FillRect(canvas, x, y + clampedRadius, clampedRadius, height - (clampedRadius * 2), color);
    gfx::FillRect(canvas, x + width - clampedRadius, y + clampedRadius, clampedRadius, height - (clampedRadius * 2), color);
    gfx::FillCircle(canvas, x + clampedRadius, y + clampedRadius, clampedRadius, color);
    gfx::FillCircle(canvas, x + width - clampedRadius - 1, y + clampedRadius, clampedRadius, color);
    gfx::FillCircle(canvas, x + clampedRadius, y + height - clampedRadius - 1, clampedRadius, color);
    gfx::FillCircle(canvas, x + width - clampedRadius - 1, y + height - clampedRadius - 1, clampedRadius, color);
}

std::string SymbolTokenForButtonLabel(const std::string& label) {
    if (label == "L") {
        return std::string("\xEE\xA4\x91");
    }
    if (label == "R") {
        return std::string("\xEE\xA4\x92");
    }
    if (label == "+") {
        return "+";
    }
    if (label == "A") {
        return "a";
    }
    if (label == "B") {
        return "b";
    }
    if (label == "X") {
        return "x";
    }
    if (label == "Y") {
        return "y";
    }
    return {};
}

int MeasureSymbolTextWidth(const std::string& text, int scale = 1) {
    const int width = gfx::MeasureTextWidthFont(text, gfx::FontFace::Symbols, scale);
    if (width > 0) {
        return width;
    }
    return gfx::MeasureTextWidth(text, scale);
}

int SymbolLineHeight(int scale = 1) {
    const int height = gfx::LineHeightFont(gfx::FontFace::Symbols, scale);
    if (height > 0) {
        return height;
    }
    return gfx::LineHeight(scale);
}

void DrawSymbolText(gfx::Canvas& canvas, int x, int y, const std::string& text, std::uint32_t color, int scale = 1) {
    if (gfx::MeasureTextWidthFont(text, gfx::FontFace::Symbols, scale) > 0) {
        gfx::DrawTextFont(canvas, x, y, text, color, gfx::FontFace::Symbols, scale);
        return;
    }
    gfx::DrawText(canvas, x, y, text, color, scale);
}

void DrawBulletSeparator(gfx::Canvas& canvas, int x, int y, std::uint32_t color) {
    gfx::FillCircle(canvas, x, y, 3, color);
}

void DrawTextWithBulletSeparator(gfx::Canvas& canvas,
                                 int x,
                                 int y,
                                 const std::string& left,
                                 const std::string& right,
                                 std::uint32_t textColor,
                                 int scale = 1) {
    gfx::DrawText(canvas, x, y, left, textColor, scale);
    const int leftWidth = gfx::MeasureTextWidth(left, scale);
    const std::string bullet = u8"\u2022";
    const int bulletX = x + leftWidth + 8;
    gfx::DrawText(canvas, bulletX, y, bullet, textColor, scale);
    gfx::DrawText(canvas, bulletX + gfx::MeasureTextWidth(bullet, scale) + 8, y, right, textColor, scale);
}

void DrawBadge(gfx::Canvas& canvas,
               int x,
               int y,
               int width,
               int height,
               const std::string& label,
                std::uint32_t outer,
                std::uint32_t inner,
                std::uint32_t textColor,
                int textYOffset = 0) {
    const std::string symbolToken = SymbolTokenForButtonLabel(label);
    if (!symbolToken.empty()) {
        const int symbolWidth = MeasureSymbolTextWidth(symbolToken, 1);
        const int symbolHeight = SymbolLineHeight(1);
        if (symbolWidth > 0 && symbolHeight > 0) {
            const int symbolX = x + (width - symbolWidth) / 2;
            const int symbolY = y + std::max(0, (height - symbolHeight) / 2) + 1;
            DrawSymbolText(canvas, symbolX, symbolY, symbolToken, textColor, 1);
            return;
        }
    }

    const int radius = 7;
    FillRoundedRect(canvas, x, y, width, height, radius, outer);
    FillRoundedRect(canvas, x + 2, y + 2, width - 4, height - 4, std::max(2, radius - 1), inner);

    const int textWidth = gfx::MeasureTextWidth(label, 1);
    const int textX = x + (width - textWidth) / 2;
    const int textY = y + std::max(0, (height - gfx::LineHeight(1)) / 2) + 2 + textYOffset;
    gfx::DrawText(canvas, textX, textY, label, textColor, 1);
    gfx::DrawText(canvas, textX + 1, textY, label, textColor, 1);
}

constexpr int kMenuTextPx = 20;
constexpr int kBodyTextPx = 18;
constexpr int kActionTextPx = 14;
constexpr int kStatusTextPx = 14;

int CenterTextY(int y, int height, int scale = 1) {
    return y + std::max(0, (height - gfx::LineHeight(scale)) / 2);
}

int CenterTextYPx(int y, int height, int pixelHeight) {
    return y + std::max(0, (height - gfx::LineHeightPx(pixelHeight)) / 2);
}

void DrawChip(gfx::Canvas& canvas, int x, int y, const std::string& text, std::uint32_t fill, std::uint32_t textColor) {
    const int width = gfx::MeasureTextWidth(text, 1) + 18;
    DrawPanel(canvas, x, y, width, 24, fill, fill);
    gfx::DrawText(canvas, x + 9, CenterTextY(y, 24, 1), text, textColor, 1);
}

int MeasureChipWidth(const std::string& text) {
    return gfx::MeasureTextWidth(text, 1) + 18;
}

void DrawChipRow(gfx::Canvas& canvas,
                 int x,
                 int y,
                 int maxRight,
                 const std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t>>& chips) {
    int chipX = x;
    for (const auto& chip : chips) {
        const std::string& label = std::get<0>(chip);
        if (label.empty()) {
            continue;
        }
        const int chipWidth = MeasureChipWidth(label);
        if (chipX + chipWidth > maxRight && chipX > x) {
            break;
        }
        DrawChip(canvas, chipX, y, label, std::get<1>(chip), std::get<2>(chip));
        chipX += chipWidth + 8;
    }
}

std::vector<std::string> SplitLinesNormalized(std::string text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while ((pos = text.find("\r\n", pos)) != std::string::npos) {
        text.replace(pos, 2, "\n");
    }
    std::replace(text.begin(), text.end(), '\r', '\n');

    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line = Trim(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!line.empty()) {
            lines.push_back(line);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    if (lines.empty()) {
        const std::string trimmed = Trim(text);
        if (!trimmed.empty()) {
            lines.push_back(trimmed);
        }
    }

    return lines;
}

void DrawButton(gfx::Canvas& canvas,
                const ThemePalette& palette,
                int x,
                int y,
                int width,
                int height,
                const std::string& text,
                bool primary,
                bool active = false,
                bool borderless = false) {
    const std::uint32_t fill = borderless ? palette.entryFill : (primary ? palette.buttonPrimaryFill : palette.buttonSecondaryFill);
    const std::uint32_t border = borderless
                                     ? fill
                                     : (active ? palette.buttonPrimaryBorderActive
                                               : (primary ? palette.buttonPrimaryBorder : palette.buttonSecondaryBorder));
    const std::uint32_t textColor = borderless ? palette.primaryText : (primary ? palette.buttonTextPrimary : palette.buttonTextSecondary);
    DrawPanel(canvas, x, y, width, height, fill, border);

    const bool hasBadge = text.size() > 3 && text[1] == ' ' && text[2] == ' ';
    if (hasBadge) {
        const std::string badgeLabel(1, text[0]);
        const std::string buttonText = text.substr(3);
        const std::uint32_t badgeOuter = palette.buttonBadgeOuter;
        const std::uint32_t badgeInner = primary ? palette.buttonBadgeInnerPrimary : palette.buttonBadgeInnerSecondary;
        const std::uint32_t badgeText = primary ? palette.buttonBadgeTextPrimary : palette.buttonBadgeTextSecondary;
        DrawBadge(canvas, x + 12, y + 6, 34, 32, badgeLabel, badgeOuter, badgeInner, badgeText, badgeLabel == "A" ? 4 : 0);

        const int textWidth = gfx::MeasureTextWidth(buttonText, 1);
        const int contentWidth = 36 + 12 + textWidth;
        const int textX = x + std::max(60, (width - contentWidth) / 2 + 46);
        const int textY = CenterTextY(y, height, 1);
        gfx::DrawText(canvas, textX, textY, buttonText, textColor, 1);
        gfx::DrawText(canvas, textX + 1, textY, buttonText, textColor, 1);
        return;
    }

    const int textWidth = gfx::MeasureTextWidth(text, 1);
    gfx::DrawText(canvas, x + std::max(10, (width - textWidth) / 2), CenterTextY(y, height, 1), text, textColor, 1);
}

void DrawConfirmButton(gfx::Canvas& canvas,
                       const ThemePalette& palette,
                       bool darkTheme,
                       int x,
                       int y,
                       int width,
                       int height,
                       const std::string& badgeLabel,
                       const std::string& text,
                       bool primary,
                       bool active = false) {
    const std::uint32_t fill = primary ? palette.buttonPrimaryFill : palette.buttonSecondaryFill;
    const std::uint32_t border = active ? palette.buttonPrimaryBorderActive
                                        : (primary ? palette.buttonPrimaryBorder : palette.buttonSecondaryBorder);
    const std::uint32_t textColor = primary ? palette.buttonTextPrimary : palette.buttonTextSecondary;
    DrawPanel(canvas, x, y, width, height, fill, border);

    const std::uint32_t badgeOuter = palette.buttonBadgeOuter;
    const std::uint32_t badgeInner = primary ? palette.buttonBadgeInnerPrimary : palette.buttonBadgeInnerSecondary;
    const std::uint32_t badgeText = darkTheme ? gfx::Rgba(255, 255, 255) : gfx::Rgba(45, 45, 45);
    DrawBadge(canvas, x + 12, y + 6, 34, 32, badgeLabel, badgeOuter, badgeInner, badgeText, badgeLabel == "A" ? 4 : 0);

    const int textWidth = gfx::MeasureTextWidth(text, 1);
    const int contentWidth = 36 + 12 + textWidth;
    const int textX = x + std::max(60, (width - contentWidth) / 2 + 46);
    const int textY = CenterTextY(y, height, 1);
    gfx::DrawText(canvas, textX, textY, text, textColor, 1);
    gfx::DrawText(canvas, textX + 1, textY, text, textColor, 1);
}

void DrawSidebarActionCard(gfx::Canvas& canvas,
                           const ThemePalette& palette,
                           int x,
                           int y,
                           int width,
                           int height,
                           const std::string& buttonLabel,
                           const std::string& text,
                           bool highlighted = false,
                           bool active = false,
                           bool disabled = false) {
    const std::uint32_t fill = palette.entryFill;
    const std::uint32_t border = fill;
    const std::uint32_t outer = disabled ? palette.secondaryText : (active ? palette.actionBadgeOuterActive : palette.actionBadgeOuter);
    const std::uint32_t inner = disabled ? palette.entryFill : palette.actionBadgeInner;
    const std::uint32_t badgeText = disabled ? palette.mutedText : palette.actionBadgeText;
    DrawPanel(canvas, x, y, width, height, fill, border);

    DrawBadge(canvas, x + 10, y + 4, 28, 26, buttonLabel, outer, inner, badgeText);

    const int textX = x + 40;
    const int textY = CenterTextYPx(y, height, kActionTextPx) + 1;
    const std::uint32_t textColor = disabled ? palette.mutedText : palette.primaryText;
    const std::string truncated = TruncateTextToWidthPx(text, width - 42, kActionTextPx);
    gfx::DrawTextPx(canvas, textX, textY, truncated, textColor, kActionTextPx);
    gfx::DrawTextPx(canvas, textX + 1, textY, truncated, textColor, kActionTextPx);
}

void DrawSidebarStatusCard(gfx::Canvas& canvas,
                           const ThemePalette& palette,
                           int x,
                           int y,
                           int width,
                           int height,
                           const std::string& text) {
    DrawPanel(canvas, x, y, width, height, palette.statusFill, palette.statusBorder);
    const std::string truncated = TruncateTextToWidthPx(text, width - 18, kStatusTextPx);
    const int textWidth = gfx::MeasureTextWidthPx(truncated, kStatusTextPx);
    const int textX = x + std::max(10, (width - textWidth) / 2);
    gfx::DrawTextPx(canvas, textX, CenterTextYPx(y, height, kStatusTextPx) + 1, truncated, palette.statusText, kStatusTextPx);
}

void DrawSidebar(gfx::Canvas& canvas,
                 const AppState& state,
                 const std::vector<const CatalogEntry*>& items,
                 int x,
                 int y,
                 int width,
                 int height) {
    const ThemePalette palette = GetThemePalette(state);
    const bool focused = state.focus == AppState::FocusPane::Sections;
    const std::uint32_t focusedFill = UseDarkTheme(state) ? gfx::Rgba(23, 23, 23) : gfx::Rgba(230, 230, 230);
    DrawPanel(canvas,
              x,
              y,
              width,
              height,
              focused ? focusedFill : palette.sidebarFill,
              focused ? palette.sidebarBorderFocused : palette.sidebarBorder);

    gfx::FillRect(canvas, x, y, width, 84, palette.sidebarHeaderFill);
    gfx::DrawRect(canvas, x, y, width, 84, palette.sidebarBorder);
    gfx::DrawText(canvas,
                  x + 20,
                  y + 16,
                  UiString(state, "ui.app.title", "Gerenciador M.I.L.", "M.I.L. Manager"),
                  palette.headerText,
                  2);
    gfx::DrawText(canvas,
                  x + 21,
                  y + 16,
                  UiString(state, "ui.app.title", "Gerenciador M.I.L.", "M.I.L. Manager"),
                  palette.headerText,
                  2);

    const std::vector<ContentSection> sections = {
        ContentSection::Translations,
        ContentSection::ModsTools,
        ContentSection::Cheats,
        ContentSection::SaveGames,
        ContentSection::About,
    };

    const bool hasSelectionAction = state.section != ContentSection::About && !items.empty();
    const bool selectionInstalled =
        hasSelectionAction && FindReceiptForPackage(state.receipts, items[std::min(state.selection, items.size() - 1)]->id, nullptr);
    int itemY = y + 104;
    for (ContentSection section : sections) {
        const bool selected = state.section == section;
        const std::uint32_t fill = selected ? palette.sidebarItemFillSelected : palette.entryFill;
        const std::uint32_t border = fill;
        DrawPanel(canvas, x + 16, itemY, width - 32, 52, fill, border);
        gfx::DrawTextWrappedPx(canvas,
                               x + 30,
                               CenterTextYPx(itemY, 52, kMenuTextPx),
                               width - 60,
                               TruncateText(SectionLabelLocalized(state, section), 24),
                               selected ? palette.sidebarItemTextSelected : palette.sidebarItemText,
                               kMenuTextPx,
                               1);
        itemY += 62;
    }
    DrawSidebarActionCard(canvas,
                          palette,
                          kInstallButtonX,
                          kInstallButtonY,
                          kSidebarCardWidth,
                          kSidebarActionCardHeight,
                          "A",
                          selectionInstalled ? UiString(state, "ui.button.remove", "Remover", "Remove") : UiString(state, "ui.button.install", "Instalar", "Install"),
                          false,
                          state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::ActionButton);
    DrawSidebarActionCard(canvas,
                          palette,
                          kExitButtonX,
                          kExitButtonY,
                          kSidebarCardWidth,
                          kSidebarActionCardHeight,
                          "+",
                          UiString(state, "ui.button.search", "Pesquisar", "Search"),
                          false,
                          state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::SearchButton);
    DrawSidebarActionCard(canvas,
                          palette,
                          kRefreshButtonX,
                          kRefreshButtonY,
                          kSidebarCardWidth,
                          kSidebarActionCardHeight,
                          "X",
                          UiString(state, "ui.button.refresh", "Atualizar", "Refresh"),
                          false,
                          state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::RefreshButton);
    DrawSidebarActionCard(canvas,
                          palette,
                          kSortButtonX,
                          kSortButtonY,
                          kSidebarCardWidth,
                          kSidebarActionCardHeight,
                          "Y",
                          UiString(state, "ui.button.sort", "Ordenar", "Sort"),
                          false,
                          state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::SortButton);
    DrawSidebarActionCard(canvas,
                          palette,
                          kLanguageButtonX,
                          kLanguageButtonY,
                          kSidebarCardWidth,
                          kSidebarActionCardHeight,
                          "L",
                          UiString(state, "ui.button.language", "Idioma", "Language"),
                          false,
                          state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::LanguageButton);
    DrawSidebarActionCard(canvas,
                          palette,
                          kThemeButtonX,
                          kThemeButtonY,
                          kSidebarCardWidth,
                          kSidebarActionCardHeight,
                          "R",
                          UiString(state, "ui.button.theme", "Tema", "Theme"),
                          false,
                          state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::ThemeButton);
    DrawSidebarStatusCard(canvas,
                          palette,
                          kSidebarInfoX,
                          kSidebarStatusCardY,
                          kSidebarCardWidth,
                          kSidebarStatusCardHeight,
                          UiString(state, "ui.status.games_prefix", "Jogos ", "Games ") + std::to_string(state.installedTitles.size()));
    DrawSidebarStatusCard(canvas,
                          palette,
                          kSidebarInfoX + kSidebarCardWidth + kSidebarCardGap,
                          kSidebarStatusCardY,
                          kSidebarCardWidth,
                          kSidebarStatusCardHeight,
                          UiString(state, "ui.status.packages_prefix", "Pacotes ", "Packages ") + std::to_string(state.receipts.size()));
}

void DrawEmptyState(gfx::Canvas& canvas, const AppState& state, int x, int y, int width, int height) {
    const ThemePalette palette = GetThemePalette(state);
    DrawPanel(canvas, x, y, width, height, palette.emptyFill, palette.emptyBorder);
    gfx::DrawText(canvas, x + 22, y + 22, UiString(state, "ui.empty.title", u8"Nenhum item", "No items"), palette.primaryText, 2);
    gfx::DrawTextWrapped(canvas,
                         x + 22,
                         y + 62,
                         width - 44,
                         UiString(state,
                                  "ui.empty.default",
                                  u8"Ainda n?o h? conte?do dispon?vel para esta se??o no cat?logo atual.",
                                  "There is no content available for this section in the current catalog."),
                         palette.mutedText,
                         1,
                         4);
}

void DrawCheatsEmptyState(gfx::Canvas& canvas, const AppState& state, int x, int y, int width, int height) {
    const ThemePalette palette = GetThemePalette(state);
    DrawPanel(canvas, x, y, width, height, palette.emptyFill, palette.emptyBorder);
    gfx::DrawText(canvas, x + 22, y + 24, UiString(state, "ui.empty.title", u8"Nenhum item", "No items"), palette.primaryText, 2);
    gfx::DrawTextWrapped(canvas,
                         x + 22,
                         y + 68,
                         width - 44,
                         UiString(state,
                                  "ui.empty.cheats",
                                  u8"T?tulos n?o identificados. Utilize a Pesquisa para localizar a trapa?a desejada.",
                                  "Titles not identified. Use Search to find the desired cheats."),
                         palette.mutedText,
                         1,
                         4);
}

void DrawAboutCenterContent(gfx::Canvas& canvas, const AppState& state, int x, int y, int width, int height) {
    const ThemePalette palette = GetThemePalette(state);
    DrawPanel(canvas, x, y, width, height, palette.emptyFill, palette.emptyBorder);

    int cursorY = y + 26;
    cursorY += gfx::DrawTextWrapped(canvas,
                                    x + 22,
                                    cursorY,
                                    width - 44,
                                    UiString(state,
                                             "ui.about.center.paragraph1",
                                             "Criada em março de 2024, a M.I.L. Traduções é um formada por um pequeno grupo de pessoas, apaixonadas por games, que se dedicam à tradução como um passatempo e de forma voluntária.",
                                             "Founded in March 2024, M.I.L. Traduções is a small group of people passionate about games who dedicate themselves to translation as a voluntary hobby."),
                                    palette.primaryText,
                                    1,
                                    7);
    cursorY += 16;
    cursorY += gfx::DrawTextWrapped(canvas,
                                    x + 22,
                                    cursorY,
                                    width - 44,
                                    UiString(state,
                                             "ui.about.center.paragraph2",
                                             "Com isso desejamos que mais pessoas possam mergulhar nas histórias, narrativas e universos incríveis que os jogos oferecem, eliminando as barreiras linguísticas e tornando o entretenimento acessível para todos!",
                                             "Our goal is to help more people dive into the incredible stories, narratives, and universes that games offer, breaking down language barriers and making entertainment accessible to everyone!"),
                                    palette.primaryText,
                                    1,
                                    7);
    cursorY += 16;
    cursorY += gfx::DrawTextWrapped(canvas,
                                    x + 22,
                                    cursorY,
                                    width - 44,
                                    UiString(state,
                                             "ui.about.center.paragraph3",
                                             "Não incentivamos a pirataria! Todo e qualquer trabalho feito pela M.I.L. é inteiramente sem fins lucrativos. Logo, aqui disponibilizamos, de forma gratuita, APENAS os arquivos de tradução, modificações e salvamentos.",
                                             "We do not encourage piracy! Any and all work done by M.I.L. is strictly non-profit. Therefore, we provide—free of charge—ONLY the translation files, mods, and save files."),
                                    palette.warningText,
                                    1,
                                    8);
    cursorY += 18;
    cursorY += gfx::DrawTextWrapped(canvas,
                                    x + 22,
                                    cursorY,
                                    width - 44,
                                    UiString(state,
                                             "ui.about.center.links_label",
                                             "Conheça nosso trabalho em:",
                                             "Check out our work at:"),
                                    palette.accentText,
                                    1,
                                    3);
    cursorY += 6;
    gfx::DrawText(canvas,
                  x + 22,
                  cursorY,
                  UiString(state, "ui.about.center.link_site", "miltraducoes.com", "miltraducoes.com"),
                  palette.primaryText,
                  1);
    cursorY += gfx::LineHeight(1) + 6;
    gfx::DrawText(canvas,
                  x + 22,
                  cursorY,
                  UiString(state, "ui.about.center.link_discord", "dsc.gg/miltraducoes", "dsc.gg/miltraducoes"),
                  palette.primaryText,
                  1);
}

void DrawSaveGamesEmptyState(gfx::Canvas& canvas, const AppState& state, int x, int y, int width, int height) {
    const ThemePalette palette = GetThemePalette(state);
    DrawPanel(canvas, x, y, width, height, palette.emptyFill, palette.emptyBorder);
    gfx::DrawText(canvas, x + 22, y + 22, UiString(state, "ui.empty.title", u8"Nenhum item", "No items"), palette.primaryText, 2);
    gfx::DrawTextWrapped(canvas,
                         x + 22,
                         y + 62,
                         width - 44,
                         UiString(state,
                                  "ui.empty.saves",
                                  u8"T?tulos n?o identificados. Utilize a Pesquisa para localizar o save desejado.",
                                  "Titles not identified. Use Search to find the desired save."),
                         palette.mutedText,
                         1,
                         4);
}

void DrawEntryList(gfx::Canvas& canvas,
                   const AppState& state,
                   const std::vector<const CatalogEntry*>& items,
                   int x,
                   int y,
                   int width,
                   int height) {
    const ThemePalette palette = GetThemePalette(state);
    const bool focused = state.focus == AppState::FocusPane::Catalog;
    const std::uint32_t focusedFill = UseDarkTheme(state) ? gfx::Rgba(23, 23, 23) : gfx::Rgba(230, 230, 230);
    DrawPanel(canvas,
              x,
              y,
              width,
              height,
              focused ? focusedFill : palette.contentPanelFill,
              focused ? palette.contentPanelBorderFocused : palette.contentPanelBorder);
    gfx::FillRect(canvas, x, y, width, 84, palette.contentHeaderFill);
    gfx::DrawRect(canvas, x, y, width, 84, palette.contentPanelBorder);
    gfx::DrawText(canvas, x + 20, y + 16, SectionLabelLocalized(state, state.section), palette.headerText, 2);
    gfx::DrawText(canvas, x + 21, y + 16, SectionLabelLocalized(state, state.section), palette.headerText, 2);
    DrawTextWithBulletSeparator(canvas,
                                x + 20,
                                y + 52,
                                std::string(UiString(state, "ui.label.source_prefix", "Fonte: ", "Source: ")) + GetCatalogSourceLabel(state),
                                std::string(UiString(state, "ui.label.sort_prefix", "Ordem: ", "Sort: ")) + SortModeLabel(state, state.sortMode),
                                palette.headerMetaText,
                                1);

    if (state.section == ContentSection::About) {
        DrawAboutCenterContent(canvas, state, x + 18, y + 96, width - 36, height - 114);
        return;
    }

    if (items.empty()) {
        if (state.section == ContentSection::Cheats) {
            DrawCheatsEmptyState(canvas, state, x + 18, y + 96, width - 36, height - 114);
        } else if (state.section == ContentSection::SaveGames) {
            DrawSaveGamesEmptyState(canvas, state, x + 18, y + 96, width - 36, height - 114);
        } else {
            DrawEmptyState(canvas, state, x + 18, y + 96, width - 36, height - 114);
        }
        return;
    }

    const std::size_t clampedSelection = std::min(state.selection, items.size() - 1);
    const auto window = GetVisibleEntryWindow(items.size(), clampedSelection, height);
    const std::size_t windowStart = window.first;
    const std::size_t windowEnd = window.second;
    int cardY = y + kEntryListTopOffset;

    for (std::size_t index = windowStart; index < windowEnd; ++index) {
        const CatalogEntry& entry = *items[index];
        const bool selected = index == clampedSelection;
        const InstalledTitle* installedTitle = FindInstalledTitle(state.installedTitles, entry.titleId);
        const bool suggested = EntryIsSuggested(state, entry);
        const bool installed = FindReceiptForPackage(state.receipts, entry.id, nullptr);

        const std::uint32_t fill = selected ? palette.entryFillSelected : palette.entryFill;
        const std::uint32_t border = selected ? palette.entryBorderSelected : palette.entryBorder;
        DrawPanel(canvas, x + 18, cardY, width - 36, kEntryCardHeight, fill, border);
        const std::uint32_t accentColor =
            selected ? (UseDarkTheme(state) ? palette.sidebarItemFill : palette.entryAccentSelected) : palette.entryAccent;
        gfx::FillRect(canvas,
                      x + 18,
                      cardY,
                      8,
                      kEntryCardHeight,
                      accentColor);

        const std::string thumbnailPath = PreferredThumbnailPathForEntry(state, entry);
        const bool showProofCover = !thumbnailPath.empty();
        const int coverWidth = 110;
        const int coverHeight = 110;
        const int coverX = x + width - coverWidth - 23;
        const int coverY = cardY + 4;
        const int textRightPadding = showProofCover ? 6 : 26;
        const int textMaxWidth = (showProofCover ? coverX : (x + width - 18)) - (x + 38) - textRightPadding;

        if (showProofCover) {
            gfx::DrawImageFile(canvas, coverX, coverY, coverWidth, coverHeight, thumbnailPath);
        }

        const std::uint32_t titleColor = selected ? palette.sidebarItemTextSelected : palette.primaryText;
        const std::uint32_t secondaryColor = selected ? palette.sidebarItemTextSelected : palette.secondaryText;
        gfx::DrawText(canvas,
                      x + 38,
                      cardY + 10,
                      TruncateTextToWidth(entry.name, textMaxWidth, 1),
                      titleColor,
                      1);

        const std::string contentTypesText =
            (entry.section == ContentSection::Cheats && EntryUsesCheatsIndex(state, entry))
                ? CheatCardSubtitle(state, entry, installedTitle)
                : (entry.section == ContentSection::SaveGames && EntryUsesSavesIndex(state, entry))
                      ? SaveCardSubtitle(state, entry)
                : JoinLabels(EntryContentTypeLabels(state, entry), ", ");
        if (!contentTypesText.empty()) {
            gfx::DrawText(canvas,
                          x + 38,
                          cardY + 40,
                          TruncateTextToWidth(contentTypesText, textMaxWidth, 1),
                          secondaryColor,
                          1);
        }

        std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t>> statusChips;
        if (suggested) {
            statusChips.emplace_back(UseEnglish(state) ? "Suggested" : "Sugerido", palette.chipSuggestedFill, palette.chipSuggestedText);
        }
        statusChips.emplace_back(installed ? UiText(state, "Instalado", "Installed") : UiText(state, "Não Instalado", "Not Installed"),
                                 installed ? palette.chipInstalledFill : palette.chipNeutralFill,
                                 installed ? palette.chipInstalledText : palette.chipNeutralText);
        const std::string versionLabel = EntryVersionStatusLabel(state, entry, installedTitle);
        if (!versionLabel.empty()) {
            bool versionKnown = installedTitle != nullptr && !installedTitle->displayVersion.empty();
            bool versionMatches = versionKnown && EntryMatchesInstalledVersion(entry, installedTitle);
            if (entry.section == ContentSection::Cheats && EntryUsesCheatsIndex(state, entry)) {
                versionKnown = installedTitle != nullptr && !installedTitle->buildIdHex.empty();
                versionMatches = false;
                if (versionKnown) {
                    const CheatTitleRecord* cheatTitle = FindCheatTitleRecord(state.cheatsIndex, entry.titleId);
                    versionMatches = cheatTitle != nullptr &&
                                     FindCheatBuildRecord(*cheatTitle, installedTitle->buildIdHex) != nullptr;
                }
            }
            statusChips.emplace_back(versionLabel,
                                     versionKnown ? (versionMatches ? palette.chipInstalledFill : palette.chipSuggestedFill)
                                                  : palette.chipNeutralFill,
                                     versionKnown ? (versionMatches ? palette.chipInstalledText : palette.chipSuggestedText)
                                                  : palette.chipNeutralText);
        }
        DrawChipRow(canvas, x + 38, cardY + 72, x + 38 + textMaxWidth, statusChips);
        cardY += kEntryCardHeight + kEntryCardGap;
    }
}

void DrawDetails(gfx::Canvas& canvas,
                 const AppState& state,
                 const std::vector<const CatalogEntry*>& items,
                 int x,
                 int y,
                 int width,
                 int height) {
    const ThemePalette palette = GetThemePalette(state);
    DrawPanel(canvas, x, y, width, height, palette.detailsFill, palette.detailsBorder);

    if (state.section == ContentSection::About) {
        gfx::FillRect(canvas, x, y, width, 84, palette.contentHeaderFill);
        gfx::DrawRect(canvas, x, y, width, 84, palette.detailsBorder);
        gfx::DrawText(canvas, x + 20, y + 16, UiString(state, "ui.section.about", "Sobre a M.I.L.", "About M.I.L."), palette.headerText, 2);
        gfx::DrawText(canvas, x + 21, y + 16, UiString(state, "ui.section.about", "Sobre a M.I.L.", "About M.I.L."), palette.headerText, 2);
        gfx::DrawTextWrapped(canvas,
                             x + 20,
                             y + 96,
                             width - 40,
                             UiText(state,
                                    "Gerenciador dedicado a traduções, dublagens, mods, cheats e saves para Switch real e Ryujinx.",
                                    "Manager dedicated to translations, dubs, mods, cheats and saves for both real Switch and Ryujinx."),
                             palette.secondaryText,
                             1,
                             8);
        gfx::DrawTextWrapped(canvas,
                             x + 20,
                             y + 180,
                             width - 40,
                             std::string(UiString(state, "ui.about.active_source", "Fonte ativa: ", "Active source: ")) + GetCatalogSourceLabel(state),
                             palette.accentText,
                             1,
                             3);
        gfx::DrawTextWrapped(canvas,
                             x + 20,
                             y + 242,
                             width - 40,
                             std::string(UiString(state, "ui.about.environment", "Ambiente: ", "Environment: ")) +
                                 RuntimeEnvironmentLabel(GetRuntimeEnvironment()),
                             palette.accentText,
                             1,
                             2);
        gfx::DrawTextWrapped(canvas,
                             x + 20,
                             y + 284,
                             width - 40,
                             std::string(UiString(state, "ui.about.loader_info", "Loader info: ", "Loader info: ")) +
                                 TruncateText(GetLoaderInfoSummary(), 42),
                             palette.accentText,
                             1,
                             3);
        return;
    }

    if (items.empty()) {
        gfx::DrawTextWrapped(canvas,
                             x + 20,
                             y + 120,
                             width - 40,
                             UiText(state,
                                    "Carregue um catálogo ou troque de seção para ver os detalhes do pacote selecionado.",
                                    "Load a catalog or change section to view package details."),
                             palette.secondaryText,
                             1,
                             6);
        return;
    }

    const CatalogEntry& entry = *items[std::min(state.selection, items.size() - 1)];
    const InstalledTitle* installedTitle = FindInstalledTitle(state.installedTitles, entry.titleId);
    const std::string localizedIntro = LocalizedEntryIntro(state, entry);
    const std::string localizedSummary = LocalizedEntrySummary(state, entry);
    const int infoStep = gfx::LineHeight(1) + 1;
    gfx::FillRect(canvas, x, y, width, 84, palette.contentHeaderFill);
    gfx::DrawRect(canvas, x, y, width, 84, palette.detailsBorder);
    gfx::DrawTextWrapped(canvas, x + 20, y + 16, width - 40, entry.name, palette.headerText, 2, 2);
    gfx::DrawTextWrapped(canvas, x + 21, y + 16, width - 40, entry.name, palette.headerText, 2, 2);

    int cursorY = y + 114;
    if (!localizedIntro.empty()) {
        const int introHeight = gfx::DrawTextWrapped(canvas, x + 20, cursorY, width - 40, localizedIntro, palette.primaryText, 1, 4);
        gfx::DrawTextWrapped(canvas, x + 21, cursorY, width - 40, localizedIntro, palette.primaryText, 1, 4);
        cursorY += introHeight;
        cursorY += gfx::LineHeight(1);
    }

    if (!localizedSummary.empty()) {
        cursorY += gfx::DrawTextWrapped(canvas, x + 20, cursorY, width - 40, localizedSummary, palette.primaryText, 1, 6);
        cursorY += gfx::LineHeight(1) + 2;
    }

    gfx::DrawText(canvas, x + 20, cursorY, std::string(UiText(state, "Pacote: ", "Package: ")) + entry.id, palette.secondaryText, 1);
    cursorY += infoStep;
    gfx::DrawText(canvas, x + 20, cursorY, std::string(UiText(state, "ID Título: ", "Title ID: ")) + entry.titleId, palette.secondaryText, 1);
    cursorY += infoStep;
    if (EntryHasVariants(entry)) {
        cursorY += gfx::DrawTextWrapped(canvas,
                                        x + 20,
                                        cursorY,
                                        width - 40,
                                        std::string(UiText(state, "Variantes: ", "Variants: ")) + VariantListSummary(entry),
                                        palette.secondaryText,
                                        1,
                                        3,
                                        2);
    }

    if (entry.section == ContentSection::Cheats && EntryUsesCheatsIndex(state, entry)) {
        const CheatTitleRecord* cheatTitle = FindCheatTitleRecord(state.cheatsIndex, entry.titleId);
        if (cheatTitle != nullptr) {
            const std::vector<std::string> categories = CollectCheatCategoriesLocalized(state, *cheatTitle);
            if (!categories.empty()) {
                cursorY += gfx::DrawTextWrapped(canvas,
                                                x + 20,
                                                cursorY,
                                                width - 40,
                                                std::string(UiText(state, "Categorias: ", "Categories: ")) +
                                                    SummarizeLabelList(categories, 3),
                                                palette.secondaryText,
                                                1,
                                                3);
            }

            const std::vector<std::string> sources = CollectCheatSourcesLocalized(state, *cheatTitle);
            if (!sources.empty()) {
                cursorY += gfx::DrawTextWrapped(canvas,
                                                x + 20,
                                                cursorY,
                                                width - 40,
                                                std::string(UiText(state, "Origens: ", "Sources: ")) +
                                                    SummarizeLabelList(sources, 3),
                                                palette.secondaryText,
                                                1,
                                                3);
            }

            gfx::DrawText(canvas,
                          x + 20,
                          cursorY,
                          std::string(UiText(state, "Builds publicados: ", "Published builds: ")) +
                              std::to_string(cheatTitle->builds.size()),
                          palette.secondaryText,
                          1);
            cursorY += infoStep;

            gfx::DrawText(canvas,
                          x + 20,
                          cursorY,
                          std::string(UiText(state, "Build detectado: ", "Detected build: ")) +
                              ((installedTitle != nullptr && !installedTitle->buildIdHex.empty())
                                   ? installedTitle->buildIdHex
                                   : UiText(state, "não disponível", "unavailable")),
                          palette.secondaryText,
                          1);
            cursorY += infoStep;

            InstallReceipt cheatReceipt;
            if (FindReceiptForPackage(state.receipts, entry.id, &cheatReceipt)) {
                const std::string installedBuild = InstalledCheatBuildId(cheatReceipt);
                if (!installedBuild.empty()) {
                    gfx::DrawText(canvas,
                                  x + 20,
                                  cursorY,
                                  std::string(UiText(state, "Build instalado: ", "Installed build: ")) + installedBuild,
                                  palette.secondaryText,
                                  1);
                    cursorY += infoStep;
                }
            }
        }
    }

    if (!entry.author.empty()) {
        gfx::DrawText(canvas, x + 20, cursorY, UiText(state, "Autoria:", "Credits:"), palette.secondaryText, 1);
        cursorY += infoStep;
        cursorY += gfx::DrawTextWrapped(canvas, x + 20, cursorY, width - 40, entry.author, palette.secondaryText, 1, 6);
    }

    if (entry.section == ContentSection::SaveGames && EntryUsesSavesIndex(state, entry)) {
        const SaveTitleRecord* saveTitle = FindSaveTitleRecord(state.savesIndex, entry.titleId);
        if (saveTitle != nullptr) {
            if (!saveTitle->categories.empty()) {
                std::vector<std::string> labels;
                for (const std::string& category : saveTitle->categories) {
                    AppendUniqueString(labels, LocalizedSaveCategoryLabel(state, category));
                }
                cursorY += gfx::DrawTextWrapped(canvas,
                                                x + 20,
                                                cursorY,
                                                width - 40,
                                                std::string(UiText(state, "Categorias: ", "Categories: ")) +
                                                    SummarizeLabelList(labels, 3),
                                                palette.secondaryText,
                                                1,
                                                3);
            }

            gfx::DrawText(canvas,
                          x + 20,
                          cursorY,
                          std::string(UiText(state, "Variantes publicadas: ", "Published variants: ")) +
                              std::to_string(saveTitle->variants.size()),
                          palette.secondaryText,
                          1);
            cursorY += infoStep;
        }
    }

    if (installedTitle && !installedTitle->displayVersion.empty()) {
        gfx::DrawText(canvas,
                      x + 20,
                      cursorY,
                      std::string(UiText(state, "Versão do jogo: ", "Game version: ")) + installedTitle->displayVersion,
                      palette.secondaryText,
                      1);
        cursorY += infoStep;
    }
    if (!entry.packageVersion.empty()) {
        gfx::DrawText(canvas,
                      x + 20,
                      cursorY,
                      std::string(UiText(state, "Versão do pacote: ", "Package version: ")) + entry.packageVersion,
                      palette.secondaryText,
                      1);
        cursorY += infoStep;
    }

    if (!entry.contentRevision.empty()) {
        gfx::DrawText(canvas,
                      x + 20,
                      cursorY,
                      std::string(UiText(state, "Última atualização: ", "Last update: ")) + entry.contentRevision,
                      palette.secondaryText,
                      1);
        cursorY += infoStep;
    }

    std::string footerSummary;
    if (entry.section == ContentSection::Cheats && EntryUsesCheatsIndex(state, entry)) {
        footerSummary = MakeCheatAvailabilitySummaryLocalized(state, entry, installedTitle);
    } else if (entry.section == ContentSection::SaveGames && EntryUsesSavesIndex(state, entry)) {
        footerSummary = MakeSaveAvailabilitySummaryLocalized(state, entry, installedTitle);
    } else {
        footerSummary = MakeCompatibilitySummaryLocalized(state, entry, installedTitle);
    }
    cursorY += gfx::LineHeight(1) + 4;
    cursorY += gfx::DrawTextWrapped(canvas,
                                    x + 20,
                                    cursorY,
                                    width - 40,
                                    footerSummary,
                                    palette.warningText,
                                    1,
                                    6);
}

void DrawInstallConfirmationDialog(gfx::Canvas& canvas, const AppState& state) {
    if (!state.installConfirmVisible) {
        return;
    }

    const ThemePalette palette = GetThemePalette(state);
    const bool darkTheme = UseDarkTheme(state);
    const int dialogWidth = kInstallConfirmDialogWidth;
    const int dialogHeight = kInstallConfirmDialogHeight;
    const int dialogX = InstallConfirmDialogX(canvas);
    const int dialogY = InstallConfirmDialogY(canvas);
    const int buttonWidth = (dialogWidth - 48) / 2;
    const int leftButtonX = dialogX + 18;
    const int rightButtonX = dialogX + dialogWidth - 18 - buttonWidth;
    const int buttonY = dialogY + dialogHeight - 68;

    DrawPanel(canvas, dialogX, dialogY, dialogWidth, dialogHeight, palette.detailsFill, palette.detailsBorder);
    gfx::FillRect(canvas, dialogX, dialogY, dialogWidth, 52, palette.contentHeaderFill);
    gfx::DrawRect(canvas, dialogX, dialogY, dialogWidth, 52, palette.detailsBorder);
    gfx::DrawText(canvas, dialogX + 18, dialogY + 14, state.installConfirmTitle, palette.headerText, 2);
    gfx::DrawText(canvas, dialogX + 19, dialogY + 14, state.installConfirmTitle, palette.headerText, 2);

    gfx::DrawTextWrapped(canvas,
                         dialogX + 18,
                         dialogY + 72,
                         dialogWidth - 36,
                         state.installConfirmMessage,
                         palette.primaryText,
                         1,
                         4);

    DrawConfirmButton(canvas,
                      palette,
                      darkTheme,
                      leftButtonX,
                      buttonY,
                      buttonWidth,
                      40,
                      "A",
                      UiString(state, "ui.button.yes", "Sim", "Yes"),
                      true,
                      state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::ConfirmYesButton);
    DrawConfirmButton(canvas,
                      palette,
                      darkTheme,
                      rightButtonX,
                      buttonY,
                      buttonWidth,
                      40,
                      "B",
                      UiText(state, "Não", "No"),
                      false,
                      state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::ConfirmNoButton);
}

void DrawVariantSelectionDialog(gfx::Canvas& canvas, const AppState& state) {
    if (!state.variantSelectVisible) {
        return;
    }

    const CatalogEntry* entry = FindVisibleEntryById(state, state.variantSelectEntryId);
    if (entry == nullptr) {
        return;
    }

    const ThemePalette palette = GetThemePalette(state);
    const bool darkTheme = UseDarkTheme(state);
    const int dialogWidth = kVariantSelectDialogWidth;
    const int dialogHeight = VariantSelectDialogHeight(state);
    const int dialogX = VariantSelectDialogX(canvas);
    const int dialogY = VariantSelectDialogY(canvas, state);
    const int rowStartY = dialogY + VariantSelectDialogRowStartOffset(state);
    const int rowCount = std::max(1, static_cast<int>(state.variantSelectIds.size()));
    const int buttonWidth = (dialogWidth - 48) / 2;
    const int leftButtonX = dialogX + 18;
    const int rightButtonX = dialogX + dialogWidth - 18 - buttonWidth;
    const int buttonY = rowStartY + rowCount * kVariantSelectRowHeight + std::max(0, rowCount - 1) * kVariantSelectRowGap + 14;

    DrawPanel(canvas, dialogX, dialogY, dialogWidth, dialogHeight, palette.detailsFill, palette.detailsBorder);
    gfx::FillRect(canvas, dialogX, dialogY, dialogWidth, 52, palette.contentHeaderFill);
    gfx::DrawRect(canvas, dialogX, dialogY, dialogWidth, 52, palette.detailsBorder);
    gfx::DrawText(canvas, dialogX + 18, dialogY + 14, state.variantSelectTitle, palette.headerText, 2);
    gfx::DrawText(canvas, dialogX + 19, dialogY + 14, state.variantSelectTitle, palette.headerText, 2);
    gfx::DrawTextWrapped(canvas,
                         dialogX + 18,
                         dialogY + 62,
                         dialogWidth - 36,
                         state.variantSelectMessage,
                         palette.primaryText,
                         1,
                         3);

    for (std::size_t index = 0; index < state.variantSelectIds.size(); ++index) {
        const CatalogVariant* variant = FindVariantById(*entry, state.variantSelectIds[index]);
        if (variant == nullptr) {
            continue;
        }
        const int rowY = rowStartY + static_cast<int>(index) * (kVariantSelectRowHeight + kVariantSelectRowGap);
        const bool selected = index == std::min(state.variantSelectSelection, state.variantSelectIds.size() - 1);
        const bool pressed = state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::VariantOption &&
                             state.activeTouchTarget.index == static_cast<int>(index);
        const std::uint32_t fill = selected ? palette.entryFillSelected : palette.entryFill;
        const std::uint32_t border = selected ? palette.entryBorderSelected : palette.entryBorder;
        const std::uint32_t textColor = selected ? palette.sidebarItemTextSelected : palette.primaryText;
        DrawPanel(canvas, dialogX + 18, rowY, dialogWidth - 36, kVariantSelectRowHeight, fill, border);
        if (pressed) {
            gfx::FillRect(canvas, dialogX + 20, rowY + 2, dialogWidth - 40, kVariantSelectRowHeight - 4, gfx::Rgba(255, 255, 255, 24));
        }
        const std::string label = VariantDisplayLabel(*variant);
        const std::string versionText = CompatibilityRuleLabel(variant->compatibility);
        gfx::DrawText(canvas, dialogX + 28, rowY + 8, TruncateTextToWidth(label, dialogWidth - 56, 1), textColor, 1);
        if (!versionText.empty()) {
            gfx::DrawText(canvas,
                          dialogX + dialogWidth - 28 - gfx::MeasureTextWidth(versionText, 1),
                          rowY + 8,
                          versionText,
                          selected ? palette.sidebarItemTextSelected : palette.secondaryText,
                          1);
        }
    }

    DrawConfirmButton(canvas,
                      palette,
                      darkTheme,
                      leftButtonX,
                      buttonY,
                      buttonWidth,
                      kVariantSelectButtonsHeight,
                      "A",
                      UiString(state, "ui.button.install", "Instalar", "Install"),
                      true,
                      state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::VariantConfirmButton);
    DrawConfirmButton(canvas,
                      palette,
                      darkTheme,
                      rightButtonX,
                      buttonY,
                      buttonWidth,
                      kVariantSelectButtonsHeight,
                      "B",
                      UiString(state, "ui.button.cancel", "Cancelar", "Cancel"),
                      false,
                      state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::VariantCancelButton);
}

void DrawCheatBuildSelectionDialog(gfx::Canvas& canvas, const AppState& state) {
    if (!state.cheatBuildSelectVisible) {
        return;
    }

    const CatalogEntry* entry = FindCheatBuildEntryForPopup(state);
    (void)entry;
    const CheatTitleRecord* title = FindCheatTitleRecord(state.cheatsIndex, state.cheatBuildTitleId);

    const ThemePalette palette = GetThemePalette(state);
    const bool darkTheme = UseDarkTheme(state);
    const int dialogWidth = kCheatBuildDialogWidth;
    const int dialogHeight = CheatBuildDialogHeight(state);
    const int dialogX = CheatBuildDialogX(canvas);
    const int dialogY = CheatBuildDialogY(canvas, state);
    const int rowStartY = dialogY + CheatBuildDialogRowStartOffset(state);
    const int buttonWidth = (dialogWidth - 48) / 2;
    const int leftButtonX = dialogX + 18;
    const int rightButtonX = dialogX + dialogWidth - 18 - buttonWidth;
    const int buttonY = rowStartY + CheatBuildDialogRowsHeight(state) + 14;

    DrawPanel(canvas, dialogX, dialogY, dialogWidth, dialogHeight, palette.detailsFill, palette.detailsBorder);
    gfx::FillRect(canvas, dialogX, dialogY, dialogWidth, 52, palette.contentHeaderFill);
    gfx::DrawRect(canvas, dialogX, dialogY, dialogWidth, 52, palette.detailsBorder);
    gfx::DrawText(canvas, dialogX + 18, dialogY + 14, state.cheatBuildTitle, palette.headerText, 2);
    gfx::DrawText(canvas, dialogX + 19, dialogY + 14, state.cheatBuildTitle, palette.headerText, 2);
    gfx::DrawTextWrapped(canvas,
                         dialogX + 18,
                         dialogY + 62,
                         dialogWidth - 36,
                         state.cheatBuildMessage,
                         palette.primaryText,
                         1,
                         3);

    const auto [windowStart, windowEnd] = GetCheatBuildVisibleWindow(state);
    for (std::size_t index = windowStart; index < windowEnd; ++index) {
        const CheatBuildRecord* build = title != nullptr ? FindCheatBuildRecord(*title, state.cheatBuildIds[index]) : nullptr;
        const int rowIndex = static_cast<int>(index - windowStart);
        const int rowY = rowStartY + rowIndex * (kCheatBuildRowHeight + kCheatBuildRowGap);
        const bool selected = index == std::min(state.cheatBuildSelection, state.cheatBuildIds.size() - 1);
        const bool pressed = state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::CheatBuildOption &&
                             state.activeTouchTarget.index == static_cast<int>(index);
        const std::uint32_t fill = selected ? palette.entryFillSelected : palette.entryFill;
        const std::uint32_t border = selected ? palette.entryBorderSelected : palette.entryBorder;
        const std::uint32_t textColor = selected ? palette.sidebarItemTextSelected : palette.primaryText;
        DrawPanel(canvas, dialogX + 18, rowY, dialogWidth - 36, kCheatBuildRowHeight, fill, border);
        if (pressed) {
            gfx::FillRect(canvas, dialogX + 20, rowY + 2, dialogWidth - 40, kCheatBuildRowHeight - 4, gfx::Rgba(255, 255, 255, 24));
        }
        const std::string label = build != nullptr ? CheatBuildLabel(*build) : state.cheatBuildIds[index];
        gfx::DrawText(canvas, dialogX + 28, rowY + 8, TruncateTextToWidth(label, dialogWidth - 56, 1), textColor, 1);
    }

    if (windowStart > 0) {
        gfx::DrawText(canvas, dialogX + dialogWidth - 34, rowStartY - 18, "^", palette.secondaryText, 1);
    }
    if (windowEnd < state.cheatBuildIds.size()) {
        const int indicatorY =
            rowStartY + static_cast<int>(windowEnd - windowStart) * (kCheatBuildRowHeight + kCheatBuildRowGap) - 8;
        gfx::DrawText(canvas, dialogX + dialogWidth - 34, indicatorY, "v", palette.secondaryText, 1);
    }

    DrawConfirmButton(canvas,
                      palette,
                      darkTheme,
                      leftButtonX,
                      buttonY,
                      buttonWidth,
                      kCheatBuildButtonsHeight,
                      "A",
                      UiString(state, "ui.button.install", "Instalar", "Install"),
                      true,
                      state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::CheatBuildConfirmButton);
    DrawConfirmButton(canvas,
                      palette,
                      darkTheme,
                      rightButtonX,
                      buttonY,
                      buttonWidth,
                      kCheatBuildButtonsHeight,
                      "B",
                      UiString(state, "ui.button.cancel", "Cancelar", "Cancel"),
                      false,
                      state.touchActive && state.activeTouchTarget.kind == TouchTargetKind::CheatBuildCancelButton);
}

void DrawProgressOverlay(gfx::Canvas& canvas, const AppState& state) {
    if (!state.progressVisible) {
        return;
    }

    const ThemePalette palette = GetThemePalette(state);
    const int dialogWidth = 420;
    const int dialogHeight = 118;
    const int dialogX = (canvas.width - dialogWidth) / 2;
    const int dialogY = canvas.height - dialogHeight - 44;
    const int barX = dialogX + 18;
    const int barY = dialogY + 78;
    const int barWidth = dialogWidth - 36;
    const int barHeight = 16;
    const int fillWidth = std::max(0, std::min(barWidth, (barWidth * state.progressPercent) / 100));

    DrawPanel(canvas, dialogX, dialogY, dialogWidth, dialogHeight, palette.detailsFill, palette.detailsBorder);
    gfx::DrawText(canvas, dialogX + 18, dialogY + 16, state.progressTitle, palette.primaryText, 2);
    gfx::DrawTextWrapped(canvas, dialogX + 18, dialogY + 48, dialogWidth - 36, state.progressDetail, palette.secondaryText, 1, 4);
    gfx::FillRect(canvas, barX, barY, barWidth, barHeight, palette.buttonSecondaryFill);
    gfx::DrawRect(canvas, barX, barY, barWidth, barHeight, palette.buttonSecondaryBorder);
    if (fillWidth > 0) {
        gfx::FillRect(canvas, barX + 1, barY + 1, std::max(1, fillWidth - 2), barHeight - 2, palette.buttonPrimaryFill);
    }
    const std::string percentText = std::to_string(state.progressPercent) + "%";
    gfx::DrawText(canvas,
                  barX + barWidth - 8 - gfx::MeasureTextWidth(percentText, 1),
                  dialogY + 52,
                  percentText,
                  palette.mutedText,
                  1);
}

void RenderUi(PlatformSession& session, const AppState& state, const std::vector<const CatalogEntry*>& items) {
    if (!session.framebufferReady) {
        return;
    }

    const ThemePalette palette = GetThemePalette(state);
    gfx::Canvas canvas = gfx::BeginFrame(session.framebuffer);
    if (!canvas.pixels) {
        return;
    }

    gfx::ClearVerticalGradient(canvas, palette.windowTop, palette.windowBottom);
    gfx::FillRect(canvas, 0, 0, canvas.width, canvas.height, palette.windowBase);

    DrawSidebar(canvas, state, items, 24, 28, 270, 664);
    DrawEntryList(canvas, state, items, kEntryListX, kEntryListY, kEntryListWidth, kEntryListHeight);
    DrawDetails(canvas, state, items, kDetailsX, kDetailsY, kDetailsWidth, kDetailsHeight);

    const std::string localizedPlatformNote = LocalizePlatformNote(state, state.platformNote);
    std::string footerStatus = NormalizeFooterStatus(state.statusLine);
    if (!localizedPlatformNote.empty()) {
        if (!footerStatus.empty()) {
            footerStatus += u8" • ";
        }
        footerStatus += NormalizeFooterStatus(localizedPlatformNote);
    }

    DrawPanel(canvas, 316, kFooterY, 940, kFooterHeight, palette.footerFill, palette.footerBorder);
    gfx::DrawTextPx(canvas,
                    328,
                    CenterTextYPx(kFooterY, kFooterHeight, kActionTextPx),
                    TruncateTextToWidthPx(footerStatus, 916, kActionTextPx),
                    palette.footerText,
                    kActionTextPx);

    DrawInstallConfirmationDialog(canvas, state);
    DrawVariantSelectionDialog(canvas, state);
    DrawCheatBuildSelectionDialog(canvas, state);
    DrawProgressOverlay(canvas, state);

    gfx::EndFrame(session.framebuffer);
}

void RenderAbout(const AppState& state) {
    PrintLine(UiText(state, "Objetivo", "Purpose"));
    PrintLine(UiText(state,
                     "Aplicativo homebrew para listar e instalar traduções, mods, cheats e saves.",
                     "Homebrew app to list and install translations, mods, cheats and save games."));
    PrintLine("");
    PrintLine(UiText(state, "Arquitetura atual", "Current architecture"));
    PrintLine(UiText(state, "- Catálogo remoto em JSON com cache local automático", "- Remote JSON catalog with automatic local cache"));
    PrintLine(UiText(state,
                     "- Instalação por ZIP em sdmc:/atmosphere/contents/ para mods, traduções e cheats",
                     "- ZIP installation to sdmc:/atmosphere/contents/ for mods, translations and cheats"));
    PrintLine(UiText(state, "- Compatibilidade console + emulador por degradação de serviços", "- Console + emulator compatibility with service fallback"));
    PrintLine(UiText(state, "- Configuração em sdmc:/switch/mil_manager/settings.ini", "- Configuration in sdmc:/switch/mil_manager/settings.ini"));
    PrintLine("");
    PrintLine(UiText(state, "Diretórios", "Directories"));
    PrintLine(UiText(state, "- Cache: sdmc:/switch/mil_manager/cache", "- Cache: sdmc:/switch/mil_manager/cache"));
    PrintLine(UiText(state, "- Cache do índice: sdmc:/switch/mil_manager/cache/index.json", "- Catalog cache: sdmc:/switch/mil_manager/cache/index.json"));
    PrintLine(UiText(state, "- Recibos: sdmc:/switch/mil_manager/receipts", "- Receipts: sdmc:/switch/mil_manager/receipts"));
    PrintLine(UiText(state,
                     "- Conteúdo instalado: sdmc:/atmosphere/contents/ (saves continuam em sdmc:/)",
                     "- Installed content: sdmc:/atmosphere/contents/ (saves remain in sdmc:/)"));
    if (IsEmulatorEnvironment()) {
        PrintLine(UiText(state,
                         "- Importação do emulador: sdmc:/switch/mil_manager/cache/installed-titles-cache.json",
                         "- Emulator import: sdmc:/switch/mil_manager/cache/installed-titles-cache.json"));
    }
    PrintLine("");
    PrintLine(UiText(state, "Ambiente detectado", "Detected environment"));
    PrintLine(RuntimeEnvironmentLabel(GetRuntimeEnvironment()));
    PrintLine(std::string(UiString(state, "ui.about.loader_info", "Loader info: ", "Loader info: ")) + GetLoaderInfoSummary());
    PrintLine("");
    PrintLine(UiText(state, "Fonte ativa do catálogo", "Active catalog source"));
    PrintLine(GetCatalogSourceLabel(state));
    if (!state.catalog.catalogName.empty() || !state.catalog.catalogRevision.empty() || !state.catalog.channel.empty()) {
        PrintLine("");
        PrintLine(UiText(state, "Metadados do catálogo", "Catalog metadata"));
        if (!state.catalog.catalogName.empty()) {
            PrintLine(std::string(UiText(state, "Nome: ", "Name: ")) + state.catalog.catalogName);
        }
        if (!state.catalog.catalogRevision.empty()) {
            PrintLine(std::string(UiText(state, "Revisão: ", "Revision: ")) + state.catalog.catalogRevision);
        }
        if (!state.catalog.channel.empty()) {
            PrintLine(std::string(UiText(state, "Canal: ", "Channel: ")) + state.catalog.channel);
        }
        if (!state.catalog.generatedAt.empty()) {
            PrintLine(std::string(UiText(state, "Gerado em: ", "Generated at: ")) + state.catalog.generatedAt);
        }
        if (!state.catalog.schemaVersion.empty()) {
            PrintLine("Schema: " + state.catalog.schemaVersion);
        }
    }
}

void RenderEntries(const AppState& state, const std::vector<const CatalogEntry*>& items) {
    if (items.empty()) {
        PrintLine(UiText(state, "Nenhum item disponível nesta seção.", "No items available in this section."));
        return;
    }

    const std::size_t clampedSelection = std::min(state.selection, items.size() - 1);
    const std::size_t windowStart = clampedSelection > 4 ? clampedSelection - 4 : 0;
    const std::size_t windowEnd = std::min(items.size(), windowStart + 10);

    PrintLine(UiText(state, "Lista", "List"));
    for (std::size_t index = windowStart; index < windowEnd; ++index) {
        const CatalogEntry& entry = *items[index];
        const bool selected = index == clampedSelection;
        const bool suggested = EntryIsSuggested(state, entry);
        const bool installed = FindReceiptForPackage(state.receipts, entry.id, nullptr);

        std::string prefix = selected ? "> " : "  ";
        std::string line = prefix + entry.name;
        if (suggested) {
            line += UiText(state, " [SUGERIDO]", " [SUGGESTED]");
        }
        if (installed) {
            line += UiText(state, " [INSTALADO]", " [INSTALLED]");
        }
        PrintLine(line);
    }

    const CatalogEntry& selectedEntry = *items[clampedSelection];
    const InstalledTitle* installedTitle = FindInstalledTitle(state.installedTitles, selectedEntry.titleId);

    PrintLine("");
    PrintLine(UiText(state, "Detalhes", "Details"));
    if (!selectedEntry.intro.empty()) {
        PrintLine(selectedEntry.intro);
        PrintLine("");
    }
    PrintLine(std::string(UiText(state, "Pacote: ", "Package: ")) + selectedEntry.id +
              std::string(UseEnglish(state) ? " • Revision: " : " • Revisão: ") + selectedEntry.contentRevision);
    PrintLine(std::string(UiText(state, "Jogo: ", "Game: ")) + selectedEntry.titleId +
              (installedTitle ? UiText(state, " • Instalado localmente", " • Installed locally")
                              : UiText(state, " • Não instalado", " • Not installed")));
    if (!selectedEntry.packageVersion.empty()) {
        PrintLine(std::string(UiText(state, "Versão do pacote: ", "Package version: ")) + selectedEntry.packageVersion);
    }
    if (installedTitle != nullptr && !installedTitle->displayVersion.empty()) {
        PrintLine(std::string(UiText(state, "Versão do jogo: ", "Game version: ")) + installedTitle->displayVersion);
    }
    if (!selectedEntry.summary.empty()) {
        PrintLine(selectedEntry.summary);
    }
    PrintLine(MakeCompatibilitySummaryLocalized(state, selectedEntry, installedTitle));
}

void Render(AppState& state) {
    consoleClear();

    PrintLine(UiText(state, "Gerenciador MIL", "MIL Manager"));
    PrintLine(std::string(UiText(state, "Seção: ", "Section: ")) + SectionLabelLocalized(state, state.section) +
              std::string(UseEnglish(state) ? " • Installed games: " : " • Jogos instalados: ") + std::to_string(state.installedTitles.size()) +
              std::string(UseEnglish(state) ? " • Installed packages: " : " • Pacotes instalados: ") + std::to_string(state.receipts.size()));
    PrintLine(std::string(UiText(state, "Idioma: ", "Language: ")) + std::string(LanguageModeLabel(state.config.language)) +
              std::string(UseEnglish(state) ? " • Theme: " : " • Tema: ") + ThemeModeLabelLocalized(state, state.config.theme) +
              std::string(UseEnglish(state) ? " • Search: " : " • Busca: ") + std::string(InstalledTitleScanModeLabel(state.config.scanMode)));
    PrintLine(std::string(UiText(state, "Fonte: ", "Source: ")) + GetCatalogSourceLabel(state));
    PrintLine(state.statusLine);
    if (!state.platformNote.empty()) {
        PrintLine(LocalizePlatformNote(state, state.platformNote));
    }
    PrintLine("");

    if (state.section == ContentSection::About) {
        RenderAbout(state);
    } else {
        RenderEntries(state, BuildVisibleEntries(state));
    }

    PrintLine("");
    PrintLine(UiText(state, "Controles", "Controls"));
    PrintLine(UiText(state,
                     "LEFT/RIGHT muda foco • UP/DOWN navega • Y ordena • X atualiza",
                     "LEFT/RIGHT change focus • UP/DOWN navigate • Y sort • X refresh"));
    PrintLine(UiText(state,
                     "A instala/remove • L alterna idioma • R alterna tema • + pesquisa • - sai",
                     "A install/remove • L switch language • R switch theme • + search • - exit"));
}

void ReloadLocalState(AppState& state) {
    std::string configNote;
    state.config = LoadAppConfig(configNote);
    ReloadLanguageStrings(state);
    state.platformNote = configNote;

    std::string receiptsNote;
    state.receipts = LoadInstallReceipts(receiptsNote);
}

void RefreshInstalledTitles(AppState& state) {
    if (GetRuntimeEnvironment() == RuntimeEnvironment::Unknown &&
        GetLoaderInfoSummary() == "(empty)" &&
        !FileHasContent(kInstalledTitlesCachePath)) {
        state.installedTitles.clear();
        RefreshInstalledTitleIndex(state);
        state.platformNote = "Loader info vazio. Leitura local por NS foi bloqueada em modo seguro.";
        InvalidateVisibleEntries(state);
        return;
    }

    std::string titlesNote;
    state.installedTitles = LoadInstalledTitles(state.config, &state.catalog, titlesNote);
    RefreshInstalledTitleIndex(state);
    if (!titlesNote.empty()) {
        state.platformNote = titlesNote;
    }
    InvalidateVisibleEntries(state);
}

void CycleLanguage(AppState& state) {
    switch (state.config.language) {
        case LanguageMode::PtBr:
            state.config.language = LanguageMode::EnUs;
            break;
        case LanguageMode::EnUs:
        default:
            state.config.language = LanguageMode::PtBr;
            break;
    }

    std::string saveError;
    if (SaveAppConfig(state.config, saveError)) {
        ReloadLanguageStrings(state);
        state.statusLine = UiString(state, "ui.status.language_saved", "Idioma salvo em settings.ini", "Language saved to settings.ini");
    } else {
        state.statusLine = saveError;
    }
}

void CycleTheme(AppState& state) {
    state.config.theme = state.config.theme == ThemeMode::Dark ? ThemeMode::Light : ThemeMode::Dark;

    std::string saveError;
    if (SaveAppConfig(state.config, saveError)) {
        state.statusLine = std::string(UiString(state, "ui.status.theme_changed", "Tema alterado para ", "Theme changed to ")) +
                           ThemeModeLabelLocalized(state, state.config.theme);
    } else {
        state.statusLine = saveError;
    }
}

void CycleSortMode(AppState& state) {
    switch (state.sortMode) {
        case SortMode::Recommended:
            state.sortMode = SortMode::Recent;
            break;
        case SortMode::Recent:
            state.sortMode = SortMode::Name;
            break;
        case SortMode::Name:
        default:
            state.sortMode = SortMode::Recommended;
            break;
    }

    state.selection = 0;
    InvalidateVisibleEntries(state);
    state.statusLine = std::string(UiText(state, "Ordenação: ", "Sort mode: ")) + SortModeLabel(state, state.sortMode);
}

void HandleSelectionAction(AppState& state) {
    if (state.section == ContentSection::About) {
        return;
    }

    const auto items = BuildVisibleEntries(state);
    if (items.empty()) {
        return;
    }

    const CatalogEntry& entry = *items[std::min(state.selection, items.size() - 1)];
    InstallReceipt receipt;
    if (FindReceiptForPackage(state.receipts, entry.id, &receipt)) {
        std::string error;
        if (receipt.installType == "save") {
            SetProgress(state,
                        UiString(state, "ui.progress.restoring_save", u8"Restaurando save", "Restoring save"),
                        entry.name,
                        20);
        }
        if (UninstallPackage(receipt, error)) {
            state.statusLine = std::string(UseEnglish(state) ? "Package removed: " : "Pacote removido: ") + entry.name;
            std::string note;
            state.receipts = LoadInstallReceipts(note);
            ClearProgress(state);
        } else {
            state.statusLine = error;
            ClearProgress(state);
        }
        return;
    }

    const InstalledTitle* installedTitle = FindInstalledTitle(state.installedTitles, entry.titleId);
    std::string confirmTitle;
    std::string confirmMessage;
    if (ShouldConfirmInstall(state, entry, installedTitle, confirmTitle, confirmMessage)) {
        ShowInstallConfirmation(state, entry, confirmTitle, confirmMessage);
        return;
    }

    if (entry.section == ContentSection::Cheats) {
        BeginCheatIndexInstall(state, entry, installedTitle, false);
        return;
    }

    if (entry.section == ContentSection::SaveGames) {
        BeginVariantAwareInstall(state, entry, installedTitle, true);
        return;
    }

    BeginVariantAwareInstall(state, entry, installedTitle, false);
}

void ConfirmPendingInstall(AppState& state) {
    const CatalogEntry* entry = FindVisibleEntryById(state, state.installConfirmEntryId);
    if (entry == nullptr) {
        ClearInstallConfirmation(state);
        state.statusLine = UiText(state, "Entrada não encontrada para instalação.", "Entry not found for installation.");
        return;
    }

    ClearInstallConfirmation(state);
    const InstalledTitle* installedTitle = FindInstalledTitle(state.installedTitles, entry->titleId);
    if (entry->section == ContentSection::Cheats) {
        BeginCheatIndexInstall(state, *entry, installedTitle, true);
        return;
    }
    BeginVariantAwareInstall(state, *entry, installedTitle, true);
}

void ConfirmSelectedVariantInstall(AppState& state) {
    const CatalogEntry* entry = FindVisibleEntryById(state, state.variantSelectEntryId);
    if (entry == nullptr) {
        ClearVariantSelection(state);
        state.statusLine = UiText(state, "Entrada não encontrada para instalação.", "Entry not found for installation.");
        return;
    }

    const CatalogVariant* variant = SelectedVariantForPopup(state, *entry);
    ClearVariantSelection(state);
    if (variant == nullptr) {
        state.statusLine = UiText(state, "Nenhuma variante selecionada.", "No variant selected.");
        return;
    }

    InstallResolvedEntry(state, *entry, variant);
}

void ConfirmSelectedCheatBuildInstall(AppState& state) {
    const CatalogEntry* entry = FindCheatBuildEntryForPopup(state);
    if (entry == nullptr) {
        ClearCheatBuildSelection(state);
        state.statusLine = UiText(state, "Entrada não encontrada para instalação.", "Entry not found for installation.");
        return;
    }

    const CheatBuildRecord* build = SelectedCheatBuildForPopup(state, state.cheatsIndex, *entry);
    ClearCheatBuildSelection(state);
    if (build == nullptr) {
        state.statusLine = UiText(state, "Nenhum build de cheat selecionado.", "No cheat build selected.");
        return;
    }

    InstallCheatBuild(state, *entry, *build);
}

void PreviewTouchTarget(AppState& state, const TouchTarget& target) {
    const std::vector<ContentSection> sections = {
        ContentSection::Translations,
        ContentSection::ModsTools,
        ContentSection::Cheats,
        ContentSection::SaveGames,
        ContentSection::About,
    };

    switch (target.kind) {
        case TouchTargetKind::Section:
            if (target.index >= 0 && static_cast<std::size_t>(target.index) < sections.size()) {
                state.focus = AppState::FocusPane::Sections;
                state.section = sections[static_cast<std::size_t>(target.index)];
                state.selection = 0;
                InvalidateVisibleEntries(state);
                EnsureCheatsIndexReady(state, false);
                EnsureSavesIndexReady(state, false);
            }
            break;
        case TouchTargetKind::Entry:
            if (target.index >= 0) {
                state.focus = AppState::FocusPane::Catalog;
                state.selection = static_cast<std::size_t>(target.index);
            }
            break;
        default:
            break;
    }
}

void ActivateTouchTarget(AppState& state, const TouchTarget& target) {
    switch (target.kind) {
        case TouchTargetKind::SortButton:
            CycleSortMode(state);
            break;
        case TouchTargetKind::LanguageButton:
            CycleLanguage(state);
            break;
        case TouchTargetKind::ThemeButton:
            CycleTheme(state);
            break;
        case TouchTargetKind::SearchButton:
            OpenSearchDialog(state);
            break;
        case TouchTargetKind::RefreshButton: {
            SetProgress(state,
                        UiText(state, u8"Atualizando", "Refreshing"),
                        UiText(state, u8"Recarregando dados locais...", "Reloading local data..."),
                        10);
            ReloadLocalState(state);
            const bool allowNetworkRefresh = !IsRyujinxGuestEnvironment();
            SetProgress(state,
                        UiText(state, u8"Atualizando", "Refreshing"),
                        allowNetworkRefresh
                            ? UiString(state,
                                       "ui.progress.checking_catalog_and_cache",
                                       u8"Verificando catálogo e cache...",
                                       "Checking catalog and cache...")
                            : UiString(state,
                                       "ui.progress.using_local_catalog_cache",
                                       u8"Usando cache local do catálogo...",
                                       "Using local catalog cache..."),
                        35);
            if (LoadCatalog(state, false, allowNetworkRefresh, allowNetworkRefresh, allowNetworkRefresh)) {
                SetProgress(state,
                            UiText(state, u8"Atualizando", "Refreshing"),
                            UiText(state, u8"Lendo títulos instalados...", "Reading installed titles..."),
                            60);
                RefreshInstalledTitles(state);
                if (state.section == ContentSection::Cheats) {
                    SetProgress(state,
                                UiText(state, u8"Atualizando", "Refreshing"),
                                UiText(state,
                                       allowNetworkRefresh ? u8"Atualizando índice de trapaças..." : u8"Usando índice local de trapaças...",
                                       allowNetworkRefresh ? "Updating cheats index..." : "Using local cheats index..."),
                                82);
                    EnsureCheatsIndexReady(state, allowNetworkRefresh, allowNetworkRefresh);
                } else if (state.section == ContentSection::SaveGames) {
                    SetProgress(state,
                                UiText(state, u8"Atualizando", "Refreshing"),
                                UiText(state,
                                       allowNetworkRefresh ? u8"Atualizando índice de saves..." : u8"Usando índice local de saves...",
                                       allowNetworkRefresh ? "Updating saves index..." : "Using local saves index..."),
                                82);
                    EnsureSavesIndexReady(state, allowNetworkRefresh, allowNetworkRefresh);
                }
                state.statusLine = UiString(state,
                                            "ui.status.catalog_and_title_search_updated",
                                            "Catálogo e busca de títulos atualizados.",
                                            "Catalog and title search updated.");
            }
            SetProgress(state, UiText(state, u8"Atualizando", "Refreshing"), UiText(state, u8"Concluído.", "Done."), 100);
            ClearProgress(state);
            break;
        }
        case TouchTargetKind::ActionButton:
            HandleSelectionAction(state);
            if (!state.installConfirmVisible && !state.variantSelectVisible && !state.cheatBuildSelectVisible) {
                ReloadLocalState(state);
                RefreshInstalledTitles(state);
                if (state.section == ContentSection::Cheats) {
                    EnsureCheatsIndexReady(state, !IsRyujinxGuestEnvironment());
                }
            }
            break;
        default:
            break;
    }
}

}  // namespace

int RunApplication() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

    PlatformSession session;
    AppState state;
    state.activeSession = &session;

    std::string platformInitNote;
    InitializePlatform(session, platformInitNote);
    state.platformNote = platformInitNote;
    state.thumbnailWorkerEnabled = false;

    ReloadLocalState(state);
    if (LoadCatalog(state, true)) {
        RefreshInstalledTitles(state);
        state.statusLine = UseEnglish(state) ? "Ready. Catalog and local title state loaded."
                                             : "Pronto. Catálogo e estado local dos títulos carregados.";
    } else {
        RefreshInstalledTitles(state);
    }

    const std::vector<ContentSection> sections = {
        ContentSection::Translations,
        ContentSection::ModsTools,
        ContentSection::Cheats,
        ContentSection::SaveGames,
        ContentSection::About,
    };

    while (appletMainLoop()) {
        ++state.frameCounter;
        padUpdate(&pad);
        const u64 buttonsDown = padGetButtonsDown(&pad);

        std::size_t currentSectionIndex = std::find(sections.begin(), sections.end(), state.section) - sections.begin();
        auto visibleEntries = BuildVisibleEntries(state);
        PrefetchVisibleThumbnail(state, visibleEntries);

        if (state.installConfirmVisible) {
            HidTouchScreenState modalTouchState {};
            const size_t modalTouchStates = hidGetTouchScreenStates(&modalTouchState, 1);
            const bool modalIsTouching = modalTouchStates > 0 && modalTouchState.count > 0;
            if (modalIsTouching) {
                state.lastTouchX = static_cast<int>(modalTouchState.touches[0].x);
                state.lastTouchY = static_cast<int>(modalTouchState.touches[0].y);
                const TouchTarget currentTouchTarget =
                    HitTestTouchTarget(state, visibleEntries, state.lastTouchX, state.lastTouchY);
                state.touchActive = true;
                state.activeTouchTarget = currentTouchTarget;
            } else if (state.touchActive) {
                const TouchTarget releasedTarget = HitTestTouchTarget(state, visibleEntries, state.lastTouchX, state.lastTouchY);
                if (SameTouchTarget(state.activeTouchTarget, releasedTarget)) {
                    if (releasedTarget.kind == TouchTargetKind::ConfirmYesButton) {
                        ConfirmPendingInstall(state);
                        if (!state.installConfirmVisible && !state.variantSelectVisible && !state.cheatBuildSelectVisible) {
                            ReloadLocalState(state);
                            RefreshInstalledTitles(state);
                        }
                    } else if (releasedTarget.kind == TouchTargetKind::ConfirmNoButton) {
                        ClearInstallConfirmation(state);
                        state.statusLine = UiText(state, "Instalação cancelada.", "Installation cancelled.");
                    }
                }
                state.touchActive = false;
                state.activeTouchTarget = {};
            }
            if (buttonsDown & HidNpadButton_A) {
                ConfirmPendingInstall(state);
                if (!state.installConfirmVisible && !state.variantSelectVisible && !state.cheatBuildSelectVisible) {
                    ReloadLocalState(state);
                    RefreshInstalledTitles(state);
                }
            } else if (buttonsDown & HidNpadButton_B) {
                ClearInstallConfirmation(state);
                state.statusLine = UiText(state, "Instalação cancelada.", "Installation cancelled.");
            }

            visibleEntries = BuildVisibleEntries(state);
            RenderUi(session, state, visibleEntries);
            continue;
        }

        if (state.variantSelectVisible) {
            HidTouchScreenState modalTouchState {};
            const size_t modalTouchStates = hidGetTouchScreenStates(&modalTouchState, 1);
            const bool modalIsTouching = modalTouchStates > 0 && modalTouchState.count > 0;
            if (modalIsTouching) {
                state.lastTouchX = static_cast<int>(modalTouchState.touches[0].x);
                state.lastTouchY = static_cast<int>(modalTouchState.touches[0].y);
                const TouchTarget currentTouchTarget =
                    HitTestTouchTarget(state, visibleEntries, state.lastTouchX, state.lastTouchY);
                if (currentTouchTarget.kind == TouchTargetKind::VariantOption && currentTouchTarget.index >= 0) {
                    state.variantSelectSelection = static_cast<std::size_t>(currentTouchTarget.index);
                }
                state.touchActive = true;
                state.activeTouchTarget = currentTouchTarget;
            } else if (state.touchActive) {
                const TouchTarget releasedTarget = HitTestTouchTarget(state, visibleEntries, state.lastTouchX, state.lastTouchY);
                if (SameTouchTarget(state.activeTouchTarget, releasedTarget)) {
                    if (releasedTarget.kind == TouchTargetKind::VariantOption && releasedTarget.index >= 0) {
                        state.variantSelectSelection = static_cast<std::size_t>(releasedTarget.index);
                    } else if (releasedTarget.kind == TouchTargetKind::VariantConfirmButton) {
                        ConfirmSelectedVariantInstall(state);
                        ReloadLocalState(state);
                        RefreshInstalledTitles(state);
                    } else if (releasedTarget.kind == TouchTargetKind::VariantCancelButton) {
                        ClearVariantSelection(state);
                        state.statusLine = UiText(state, "Instalação cancelada.", "Installation cancelled.");
                    }
                }
                state.touchActive = false;
                state.activeTouchTarget = {};
            }

            if (buttonsDown & HidNpadButton_Down) {
                if (!state.variantSelectIds.empty()) {
                    state.variantSelectSelection = std::min(state.variantSelectSelection + 1, state.variantSelectIds.size() - 1);
                }
            }
            if (buttonsDown & HidNpadButton_Up) {
                if (state.variantSelectSelection > 0) {
                    state.variantSelectSelection -= 1;
                }
            }
            if (buttonsDown & HidNpadButton_A) {
                ConfirmSelectedVariantInstall(state);
                ReloadLocalState(state);
                RefreshInstalledTitles(state);
            } else if (buttonsDown & HidNpadButton_B) {
                ClearVariantSelection(state);
                state.statusLine = UiText(state, "Instalação cancelada.", "Installation cancelled.");
            }

            visibleEntries = BuildVisibleEntries(state);
            RenderUi(session, state, visibleEntries);
            continue;
        }

        if (state.cheatBuildSelectVisible) {
            HidTouchScreenState modalTouchState {};
            const size_t modalTouchStates = hidGetTouchScreenStates(&modalTouchState, 1);
            const bool modalIsTouching = modalTouchStates > 0 && modalTouchState.count > 0;
            if (modalIsTouching) {
                state.lastTouchX = static_cast<int>(modalTouchState.touches[0].x);
                state.lastTouchY = static_cast<int>(modalTouchState.touches[0].y);
                const TouchTarget currentTouchTarget =
                    HitTestTouchTarget(state, visibleEntries, state.lastTouchX, state.lastTouchY);
                if (currentTouchTarget.kind == TouchTargetKind::CheatBuildOption && currentTouchTarget.index >= 0) {
                    state.cheatBuildSelection = static_cast<std::size_t>(currentTouchTarget.index);
                }
                state.touchActive = true;
                state.activeTouchTarget = currentTouchTarget;
            } else if (state.touchActive) {
                const TouchTarget releasedTarget = HitTestTouchTarget(state, visibleEntries, state.lastTouchX, state.lastTouchY);
                if (SameTouchTarget(state.activeTouchTarget, releasedTarget)) {
                    if (releasedTarget.kind == TouchTargetKind::CheatBuildOption && releasedTarget.index >= 0) {
                        state.cheatBuildSelection = static_cast<std::size_t>(releasedTarget.index);
                    } else if (releasedTarget.kind == TouchTargetKind::CheatBuildConfirmButton) {
                        ConfirmSelectedCheatBuildInstall(state);
                        ReloadLocalState(state);
                        RefreshInstalledTitles(state);
                    } else if (releasedTarget.kind == TouchTargetKind::CheatBuildCancelButton) {
                        ClearCheatBuildSelection(state);
                        state.statusLine = UiText(state, "Instalação cancelada.", "Installation cancelled.");
                    }
                }
                state.touchActive = false;
                state.activeTouchTarget = {};
            }

            if (buttonsDown & HidNpadButton_Down) {
                if (!state.cheatBuildIds.empty()) {
                    state.cheatBuildSelection = std::min(state.cheatBuildSelection + 1, state.cheatBuildIds.size() - 1);
                }
            }
            if (buttonsDown & HidNpadButton_Up) {
                if (state.cheatBuildSelection > 0) {
                    state.cheatBuildSelection -= 1;
                }
            }
            if (buttonsDown & HidNpadButton_A) {
                ConfirmSelectedCheatBuildInstall(state);
                ReloadLocalState(state);
                RefreshInstalledTitles(state);
            } else if (buttonsDown & HidNpadButton_B) {
                ClearCheatBuildSelection(state);
                state.statusLine = UiText(state, "Instalação cancelada.", "Installation cancelled.");
            }

            visibleEntries = BuildVisibleEntries(state);
            RenderUi(session, state, visibleEntries);
            continue;
        }

        HidTouchScreenState touchState{};
        const size_t touchStates = hidGetTouchScreenStates(&touchState, 1);
        const bool isTouching = touchStates > 0 && touchState.count > 0;

        if (isTouching) {
            state.lastTouchX = static_cast<int>(touchState.touches[0].x);
            state.lastTouchY = static_cast<int>(touchState.touches[0].y);
            const TouchTarget currentTouchTarget = HitTestTouchTarget(state, visibleEntries, state.lastTouchX, state.lastTouchY);

            if (!state.touchActive) {
                state.touchActive = true;
                state.activeTouchTarget = currentTouchTarget;
                PreviewTouchTarget(state, currentTouchTarget);
            } else {
                PreviewTouchTarget(state, currentTouchTarget);
            }
        } else if (state.touchActive) {
            const TouchTarget releasedTarget = HitTestTouchTarget(state, visibleEntries, state.lastTouchX, state.lastTouchY);
            if (SameTouchTarget(state.activeTouchTarget, releasedTarget)) {
                ActivateTouchTarget(state, releasedTarget);
            }
            state.touchActive = false;
            state.activeTouchTarget = {};
        }

        const u64 exitButtons = HidNpadButton_Minus;
        if (state.exitRequested || (CanUseExitControl() && (buttonsDown & exitButtons))) {
            state.statusLine = UseEnglish(state) ? "Closing application..." : "Encerrando aplicativo...";
            break;
        }
        if (buttonsDown & HidNpadButton_Plus) {
            OpenSearchDialog(state);
        }
        if (buttonsDown & HidNpadButton_Right) {
            state.focus = AppState::FocusPane::Catalog;
        }
        if (buttonsDown & HidNpadButton_Left) {
            state.focus = AppState::FocusPane::Sections;
        }
        if (buttonsDown & HidNpadButton_Down) {
            if (state.focus == AppState::FocusPane::Sections) {
                currentSectionIndex = (currentSectionIndex + 1) % sections.size();
                state.section = sections[currentSectionIndex];
                state.selection = 0;
                InvalidateVisibleEntries(state);
                EnsureCheatsIndexReady(state, false);
                EnsureSavesIndexReady(state, false);
            } else if (!visibleEntries.empty()) {
                state.selection = std::min(state.selection + 1, visibleEntries.size() - 1);
            }
        }
        if (buttonsDown & HidNpadButton_Up) {
            if (state.focus == AppState::FocusPane::Sections) {
                currentSectionIndex = (currentSectionIndex + sections.size() - 1) % sections.size();
                state.section = sections[currentSectionIndex];
                state.selection = 0;
                InvalidateVisibleEntries(state);
                EnsureCheatsIndexReady(state, false);
                EnsureSavesIndexReady(state, false);
            } else if (state.selection > 0) {
                state.selection -= 1;
            }
        }
        if (buttonsDown & HidNpadButton_L) {
            CycleLanguage(state);
        }
        if (buttonsDown & HidNpadButton_R) {
            CycleTheme(state);
        }
        if (buttonsDown & HidNpadButton_Y) {
            CycleSortMode(state);
        }
        if (buttonsDown & HidNpadButton_X) {
            SetProgress(state,
                        UiText(state, u8"Atualizando", "Refreshing"),
                        UiText(state, u8"Recarregando dados locais...", "Reloading local data..."),
                        10);
            ReloadLocalState(state);
            const bool allowNetworkRefresh = !IsRyujinxGuestEnvironment();
            SetProgress(state,
                        UiText(state, u8"Atualizando", "Refreshing"),
                        allowNetworkRefresh
                            ? UiString(state,
                                       "ui.progress.checking_catalog_and_cache",
                                       u8"Verificando catálogo e cache...",
                                       "Checking catalog and cache...")
                            : UiString(state,
                                       "ui.progress.using_local_catalog_cache",
                                       u8"Usando cache local do catálogo...",
                                       "Using local catalog cache..."),
                        35);
            if (LoadCatalog(state, false, allowNetworkRefresh, allowNetworkRefresh, allowNetworkRefresh)) {
                SetProgress(state,
                            UiText(state, u8"Atualizando", "Refreshing"),
                            UiText(state, u8"Lendo títulos instalados...", "Reading installed titles..."),
                            60);
                RefreshInstalledTitles(state);
                if (state.section == ContentSection::Cheats) {
                    SetProgress(state,
                                UiText(state, u8"Atualizando", "Refreshing"),
                                UiText(state,
                                       allowNetworkRefresh ? u8"Atualizando índice de trapaças..." : u8"Usando índice local de trapaças...",
                                       allowNetworkRefresh ? "Updating cheats index..." : "Using local cheats index..."),
                                82);
                    EnsureCheatsIndexReady(state, allowNetworkRefresh, allowNetworkRefresh);
                } else if (state.section == ContentSection::SaveGames) {
                    SetProgress(state,
                                UiText(state, u8"Atualizando", "Refreshing"),
                                UiText(state,
                                       allowNetworkRefresh ? u8"Atualizando índice de saves..." : u8"Usando índice local de saves...",
                                       allowNetworkRefresh ? "Updating saves index..." : "Using local saves index..."),
                                82);
                    EnsureSavesIndexReady(state, allowNetworkRefresh, allowNetworkRefresh);
                }
                state.statusLine = UiString(state,
                                            "ui.status.catalog_and_title_search_updated",
                                            "Catálogo e busca de títulos atualizados.",
                                            "Catalog and title search updated.");
            }
            SetProgress(state, UiText(state, u8"Atualizando", "Refreshing"), UiText(state, u8"Concluído.", "Done."), 100);
            ClearProgress(state);
        }
        if (buttonsDown & HidNpadButton_A) {
            HandleSelectionAction(state);
            if (!state.installConfirmVisible && !state.variantSelectVisible && !state.cheatBuildSelectVisible) {
                ReloadLocalState(state);
                RefreshInstalledTitles(state);
            }
        }

        visibleEntries = BuildVisibleEntries(state);
        RenderUi(session, state, visibleEntries);
    }

    ShutdownPlatform(session);
    return 0;
}

}  // namespace mil

