#include "mil/platform.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <set>
#include <vector>

#include <switch.h>

#include "mil/config.hpp"
#include "mil/http.hpp"
#include "picojson.h"

namespace mil {

namespace {

constexpr u32 kFramebufferWidth = 1280;
constexpr u32 kFramebufferHeight = 720;
constexpr const char* kInstalledIconCacheDir = "sdmc:/switch/mil_manager/cache/installed-icons/";
constexpr const char* kBuildIdCacheDir = "sdmc:/switch/mil_manager/cache/build-ids/";
constexpr const char* kCheatVersionsDirectory =
    "https://raw.githubusercontent.com/HamletDuFromage/switch-cheats-db/master/versions/";

struct DmntCheatProcessMetadata {
    u64 process_id;
    u64 title_id;
    u8 main_nso_build_id[0x20];
    u64 main_nso_extents_base;
    u64 main_nso_extents_size;
    u64 heap_extents_base;
    u64 heap_extents_size;
};

std::string SafeStringCopy(const char* source, std::size_t maxLength) {
    if (!source || maxLength == 0) {
        return {};
    }
    std::size_t length = 0;
    while (length < maxLength && source[length] != '\0') {
        ++length;
    }
    return std::string(source, length);
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
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool WriteTextFile(const std::string& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return output.good();
}

std::string BuildIdCachePath(std::uint64_t applicationId) {
    return std::string(kBuildIdCacheDir) + ToLowerAscii(FormatTitleId(applicationId)) + ".txt";
}

std::string InstalledIconCachePath(std::uint64_t applicationId) {
    return std::string(kInstalledIconCacheDir) + ToLowerAscii(FormatTitleId(applicationId)) + ".jpg";
}

std::string FormatBuildIdHex(std::uint64_t value) {
    std::ostringstream stream;
    stream.setf(std::ios::hex, std::ios::basefield);
    stream.width(16);
    stream.fill('0');
    stream << std::nouppercase << value;
    return stream.str();
}

bool LoadCachedBuildId(std::uint64_t applicationId, std::uint32_t version, std::string& buildIdHex) {
    const std::string cachePath = BuildIdCachePath(applicationId);
    std::ifstream input(cachePath);
    if (!input.good()) {
        return false;
    }

    std::string line;
    std::uint32_t cachedVersion = 0;
    std::string cachedBuildId;
    while (std::getline(input, line)) {
        const std::size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, equalsPos);
        const std::string value = line.substr(equalsPos + 1);
        if (key == "version") {
            cachedVersion = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "build_id") {
            cachedBuildId = ToLowerAscii(value);
        }
    }

    if (cachedVersion != version || cachedBuildId.empty()) {
        return false;
    }
    buildIdHex = cachedBuildId;
    return true;
}

void SaveCachedBuildId(std::uint64_t applicationId, std::uint32_t version, const std::string& buildIdHex) {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kConfigRootDir);
    EnsureDirectory(kCacheDir);
    EnsureDirectory(kBuildIdCacheDir);
    const std::string content = "version=" + std::to_string(version) + "\nbuild_id=" + ToLowerAscii(buildIdHex) + "\n";
    WriteTextFile(BuildIdCachePath(applicationId), content);
}

bool CacheInstalledTitleIcon(std::uint64_t applicationId,
                             const NsApplicationControlData& controlData,
                             u64 actualSize,
                             std::string& cachedPath) {
    cachedPath.clear();
    if (actualSize <= sizeof(controlData.nacp)) {
        return false;
    }

    const std::size_t iconSize = static_cast<std::size_t>(actualSize - sizeof(controlData.nacp));
    if (iconSize < 4 || iconSize > sizeof(controlData.icon)) {
        return false;
    }

    const unsigned char* iconBytes = controlData.icon;
    if (!(iconBytes[0] == 0xFF && iconBytes[1] == 0xD8 && iconBytes[2] == 0xFF)) {
        return false;
    }

    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kConfigRootDir);
    EnsureDirectory(kCacheDir);
    EnsureDirectory(kInstalledIconCacheDir);

    const std::string path = InstalledIconCachePath(applicationId);
    if (!FileExists(path)) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.good()) {
            return false;
        }
        output.write(reinterpret_cast<const char*>(iconBytes), static_cast<std::streamsize>(iconSize));
        if (!output.good()) {
            return false;
        }
    }

    cachedPath = path;
    return true;
}

std::string GetLoaderInfoString() {
    const char* info = envGetLoaderInfo();
    if (info == nullptr) {
        return {};
    }

    const u64 size = envGetLoaderInfoSize();
    if (size == 0) {
        return std::string(info);
    }
    return std::string(info, static_cast<std::size_t>(size));
}

bool IsLikelyEmulatorLoader() {
    const std::string loaderInfo = ToLowerAscii(GetLoaderInfoString());
    return loaderInfo.find("ryujinx") != std::string::npos ||
           loaderInfo.find("eden") != std::string::npos ||
           loaderInfo.find("yuzu") != std::string::npos ||
           loaderInfo.find("suyu") != std::string::npos ||
           loaderInfo.find("torzu") != std::string::npos ||
           loaderInfo.find("sudachi") != std::string::npos;
}

bool IsSphairaLoader() {
    return ToLowerAscii(GetLoaderInfoString()).find("sphaira") != std::string::npos;
}

bool IsApplicationApplet() {
    return appletGetAppletType() == AppletType_Application;
}

bool IsUnsafeForwarderLikeContext() {
    const std::string loaderInfo = ToLowerAscii(GetLoaderInfoString());
    const bool isNxHbloader = loaderInfo.find("nx-hbloader") != std::string::npos;
    const bool weakLoaderInfo = loaderInfo.empty() ||
                                (!isNxHbloader && loaderInfo.find("hbl") != std::string::npos) ||
                                loaderInfo.find("hbmenu") != std::string::npos;
    return !envIsNso() && IsApplicationApplet() && weakLoaderInfo;
}

bool HasImportedTitlesFile() {
    std::ifstream input(kInstalledTitlesCachePath);
    return input.good();
}

bool HasEdenSdLayoutMarker() {
    return DirectoryExists("sdmc:/.get") || DirectoryExists("sdmc:/.get/packages") || DirectoryExists("sdmc:/.get/tmp");
}

const char* GetImportedTitlesPath() {
    return kInstalledTitlesCachePath;
}

std::string StripUtf8Bom(std::string value) {
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        value.erase(0, 3);
    }
    return value;
}

const picojson::value* FindObjectValue(const picojson::object& object,
                                       std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const auto it = object.find(key);
        if (it != object.end()) {
            return &it->second;
        }
    }
    return nullptr;
}

std::string JsonToString(const picojson::value* value) {
    if (value == nullptr) {
        return {};
    }
    if (value->is<std::string>()) {
        return value->get<std::string>();
    }
    if (value->is<double>()) {
        std::ostringstream stream;
        stream << value->get<double>();
        return stream.str();
    }
    if (value->is<bool>()) {
        return value->get<bool>() ? "true" : "false";
    }
    return {};
}

bool JsonToBool(const picojson::value* value, bool fallback) {
    if (value == nullptr) {
        return fallback;
    }
    if (value->is<bool>()) {
        return value->get<bool>();
    }
    if (value->is<std::string>()) {
        const std::string lowered = ToLowerAscii(value->get<std::string>());
        if (lowered == "true" || lowered == "1" || lowered == "yes") {
            return true;
        }
        if (lowered == "false" || lowered == "0" || lowered == "no") {
            return false;
        }
    }
    return fallback;
}

std::vector<std::string> JsonToStringVector(const picojson::value* value) {
    std::vector<std::string> output;
    if (value == nullptr || !value->is<picojson::array>()) {
        return output;
    }
    for (const picojson::value& item : value->get<picojson::array>()) {
        const std::string text = JsonToString(&item);
        if (!text.empty()) {
            output.push_back(text);
        }
    }
    return output;
}

std::vector<InstalledTitle> LoadInstalledTitlesFromImportedFile(std::string& note) {
    std::vector<InstalledTitle> titles;
    const char* importPath = GetImportedTitlesPath();
    std::ifstream input(importPath);
    if (!input.good()) {
        note = "Biblioteca do emulador não fica visível ao homebrew. Sincronize os títulos para " +
               std::string(kInstalledTitlesCachePath);
        return titles;
    }

    std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    json = StripUtf8Bom(std::move(json));
    picojson::value root;
    const std::string parseError = picojson::parse(root, json);
    if (!parseError.empty() || !root.is<picojson::object>()) {
        note = "Arquivo installed-titles-cache.json inválido.";
        return {};
    }

    const auto& object = root.get<picojson::object>();
    const auto titlesIt = object.find("titles");
    if (titlesIt == object.end() || !titlesIt->second.is<picojson::array>()) {
        note = "Arquivo installed-titles-cache.json sem array titles.";
        return {};
    }

    for (const picojson::value& item : titlesIt->second.get<picojson::array>()) {
        if (!item.is<picojson::object>()) {
            continue;
        }
        const auto& titleObject = item.get<picojson::object>();
        const auto titleIdIt = titleObject.find("titleId");
        const auto legacyTitleIdIt = titleObject.find("title_id");
        const picojson::value* titleIdValue = nullptr;
        if (titleIdIt != titleObject.end() && titleIdIt->second.is<std::string>()) {
            titleIdValue = &titleIdIt->second;
        } else if (legacyTitleIdIt != titleObject.end() && legacyTitleIdIt->second.is<std::string>()) {
            titleIdValue = &legacyTitleIdIt->second;
        }
        if (titleIdValue == nullptr) {
            continue;
        }

        InstalledTitle title;
        title.titleIdHex = ToLowerAscii(titleIdValue->get<std::string>());
        title.applicationId = std::strtoull(title.titleIdHex.c_str(), nullptr, 16);
        title.name = "Title " + title.titleIdHex;

        const auto nameIt = titleObject.find("name");
        if (nameIt != titleObject.end() && nameIt->second.is<std::string>()) {
            title.name = nameIt->second.get<std::string>();
        }
        const auto publisherIt = titleObject.find("publisher");
        if (publisherIt != titleObject.end() && publisherIt->second.is<std::string>()) {
            title.publisher = publisherIt->second.get<std::string>();
        }
        const auto versionIt = titleObject.find("displayVersion");
        const auto legacyVersionIt = titleObject.find("display_version");
        if (versionIt != titleObject.end() && versionIt->second.is<std::string>()) {
            title.displayVersion = versionIt->second.get<std::string>();
        } else if (legacyVersionIt != titleObject.end() && legacyVersionIt->second.is<std::string>()) {
            title.displayVersion = legacyVersionIt->second.get<std::string>();
        }
        const auto metadataIt = titleObject.find("metadataAvailable");
        if (metadataIt != titleObject.end() && metadataIt->second.is<bool>()) {
            title.metadataAvailable = metadataIt->second.get<bool>();
        } else {
            title.metadataAvailable = !title.name.empty();
        }
        const auto iconPathIt = titleObject.find("localIconPath");
        if (iconPathIt != titleObject.end() && iconPathIt->second.is<std::string>()) {
            title.localIconPath = iconPathIt->second.get<std::string>();
        }
        titles.push_back(title);
    }

    note = "Títulos importados de installed-titles-cache.json.";
    return titles;
}

std::vector<InstalledTitle> LoadInstalledTitlesFromImportedManifest(std::string& note) {
    std::vector<InstalledTitle> titles;
    const char* importPath = GetImportedTitlesPath();
    std::ifstream input(importPath);
    if (!input.good()) {
        note = "Biblioteca do emulador nao fica visivel ao homebrew. Sincronize os titulos para " +
               std::string(kInstalledTitlesCachePath);
        return titles;
    }

    std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    json = StripUtf8Bom(std::move(json));
    picojson::value root;
    const std::string parseError = picojson::parse(root, json);
    if (!parseError.empty() || !root.is<picojson::object>()) {
        note = "Arquivo installed-titles-cache.json invalido.";
        return {};
    }

    const auto& object = root.get<picojson::object>();
    std::string manifestSchemaVersion = JsonToString(FindObjectValue(object, {"schemaVersion", "schema_version"}));
    if (manifestSchemaVersion.empty()) {
        manifestSchemaVersion = "1";
    }

    std::string manifestEmulatorName;
    if (const picojson::value* emulatorValue = FindObjectValue(object, {"emulator"})) {
        if (emulatorValue->is<std::string>()) {
            manifestEmulatorName = emulatorValue->get<std::string>();
        } else if (emulatorValue->is<picojson::object>()) {
            manifestEmulatorName = JsonToString(
                FindObjectValue(emulatorValue->get<picojson::object>(), {"name", "id"}));
        }
    }

    const picojson::value* titlesValue = FindObjectValue(object, {"titles"});
    if (titlesValue == nullptr || !titlesValue->is<picojson::array>()) {
        note = "Arquivo installed-titles-cache.json sem array titles.";
        return {};
    }

    for (const picojson::value& item : titlesValue->get<picojson::array>()) {
        if (!item.is<picojson::object>()) {
            continue;
        }

        const auto& titleObject = item.get<picojson::object>();
        const picojson::value* titleIdValue = FindObjectValue(titleObject, {"titleId", "title_id"});
        if (titleIdValue == nullptr) {
            continue;
        }

        InstalledTitle title;
        title.titleIdHex = ToLowerAscii(JsonToString(titleIdValue));
        if (title.titleIdHex.empty()) {
            continue;
        }

        title.applicationId = std::strtoull(title.titleIdHex.c_str(), nullptr, 16);
        title.baseTitleIdHex = ToLowerAscii(
            JsonToString(FindObjectValue(titleObject, {"baseTitleId", "base_title_id"})));
        title.buildIdHex = ToLowerAscii(
            JsonToString(FindObjectValue(titleObject, {"buildId", "build_id"})));
        if (title.baseTitleIdHex.empty()) {
            title.baseTitleIdHex = title.titleIdHex;
        }

        title.name = "Title " + title.titleIdHex;
        if (const picojson::value* nameValue = FindObjectValue(titleObject, {"name"})) {
            const std::string parsed = JsonToString(nameValue);
            if (!parsed.empty()) {
                title.name = parsed;
            }
        }

        title.publisher = JsonToString(FindObjectValue(titleObject, {"publisher", "developer"}));
        title.displayVersion = JsonToString(
            FindObjectValue(titleObject, {"displayVersion", "display_version", "version"}));
        title.metadataAvailable =
            JsonToBool(FindObjectValue(titleObject, {"metadataAvailable", "metadata_available"}),
                       !title.name.empty());
        title.localIconPath =
            JsonToString(FindObjectValue(titleObject, {"localIconPath", "local_icon_path"}));
        title.sourcePath =
            JsonToString(FindObjectValue(titleObject, {"sourcePath", "source_path", "path"}));
        title.basePath = JsonToString(FindObjectValue(titleObject, {"basePath", "base_path"}));
        title.updatePath = JsonToString(FindObjectValue(titleObject, {"updatePath", "update_path"}));
        title.dlcPaths = JsonToStringVector(FindObjectValue(titleObject, {"dlcPaths", "dlc_paths"}));
        title.fileType = JsonToString(FindObjectValue(titleObject, {"fileType", "file_type"}));
        title.source = JsonToString(FindObjectValue(titleObject, {"source"}));
        title.emulatorName = JsonToString(FindObjectValue(titleObject, {"emulator"}));
        title.lastPlayedUtc =
            JsonToString(FindObjectValue(titleObject, {"lastPlayedUtc", "last_played_utc"}));
        title.playTime = JsonToString(FindObjectValue(titleObject, {"playTime", "play_time"}));
        title.favorite = JsonToBool(FindObjectValue(titleObject, {"favorite"}), false);

        if (const picojson::value* pathsValue = FindObjectValue(titleObject, {"paths"});
            pathsValue != nullptr && pathsValue->is<picojson::object>()) {
            const auto& pathsObject = pathsValue->get<picojson::object>();
            if (title.sourcePath.empty()) {
                title.sourcePath = JsonToString(FindObjectValue(pathsObject, {"source", "main"}));
            }
            if (title.basePath.empty()) {
                title.basePath = JsonToString(FindObjectValue(pathsObject, {"base"}));
            }
            if (title.updatePath.empty()) {
                title.updatePath = JsonToString(FindObjectValue(pathsObject, {"update"}));
            }
            if (title.dlcPaths.empty()) {
                title.dlcPaths = JsonToStringVector(FindObjectValue(pathsObject, {"dlc"}));
            }
        }

        if (title.localIconPath.empty()) {
            if (const picojson::value* iconValue = FindObjectValue(titleObject, {"icon"});
                iconValue != nullptr && iconValue->is<picojson::object>()) {
                title.localIconPath = JsonToString(
                    FindObjectValue(iconValue->get<picojson::object>(), {"path", "localPath"}));
            }
        }

        if (title.emulatorName.empty()) {
            title.emulatorName = manifestEmulatorName;
        }
        if (title.sourcePath.empty()) {
            title.sourcePath = title.basePath.empty() ? title.updatePath : title.basePath;
        }

        titles.push_back(title);
    }

    note = manifestSchemaVersion == "2"
               ? "Manifesto v2 de titulos do emulador carregado."
               : "Titulos importados de installed-titles-cache.json.";
    return titles;
}

bool TryReadTitleMetadata(std::uint64_t applicationId, InstalledTitle& title) {
    NsApplicationControlData controlData{};
    u64 actualSize = 0;
    const Result controlResult = nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                                             applicationId,
                                                             &controlData,
                                                             sizeof(controlData),
                                                             &actualSize);
    if (R_FAILED(controlResult) || actualSize < sizeof(controlData.nacp)) {
        return false;
    }

    title.applicationId = applicationId;
    title.titleIdHex = FormatTitleId(applicationId);

    NacpLanguageEntry* languageEntry = nullptr;
    if (R_SUCCEEDED(nacpGetLanguageEntry(&controlData.nacp, &languageEntry)) && languageEntry != nullptr) {
        title.name = SafeStringCopy(languageEntry->name, sizeof(languageEntry->name));
        title.publisher = SafeStringCopy(languageEntry->author, sizeof(languageEntry->author));
    }
    title.displayVersion = SafeStringCopy(controlData.nacp.display_version, sizeof(controlData.nacp.display_version));
    title.metadataAvailable = !title.name.empty();
    CacheInstalledTitleIcon(applicationId, controlData, actualSize, title.localIconPath);

    if (title.name.empty()) {
        title.name = "Title " + title.titleIdHex;
    }
    return true;
}

bool TryReadBuildIdFromDmnt(std::uint64_t applicationId, std::string& buildIdHex) {
    buildIdHex.clear();

    Service dmntcht{};
    if (R_FAILED(smGetService(&dmntcht, "dmnt:cht"))) {
        return false;
    }

    DmntCheatProcessMetadata metadata{};
    const Result queryResult = serviceDispatch(&dmntcht, 65003);
    const Result metadataResult = R_SUCCEEDED(queryResult) ? serviceDispatchOut(&dmntcht, 65002, metadata)
                                                           : queryResult;
    serviceClose(&dmntcht);
    if (R_FAILED(metadataResult) || metadata.title_id != applicationId) {
        return false;
    }

    std::uint64_t buildValue = 0;
    std::memcpy(&buildValue, metadata.main_nso_build_id, sizeof(buildValue));
    buildIdHex = FormatBuildIdHex(__builtin_bswap64(buildValue));
    return !buildIdHex.empty();
}

bool TryGetHighestInstalledVersionRaw(std::uint64_t applicationId, std::uint32_t& version) {
    version = 0;

    const Result initResult = nsInitialize();
    if (R_FAILED(initResult)) {
        return false;
    }

    std::vector<NsApplicationContentMetaStatus> statuses(32);
    s32 count = 0;
    const Result listResult =
        nsListApplicationContentMetaStatus(applicationId, 0, statuses.data(), static_cast<s32>(statuses.size()), &count);
    nsExit();
    if (R_FAILED(listResult) || count <= 0) {
        return false;
    }

    std::uint32_t highest = 0;
    for (s32 index = 0; index < count; ++index) {
        highest = std::max(highest, static_cast<std::uint32_t>(statuses[index].version));
    }

    version = highest;
    return true;
}

bool TryResolveBuildIdFromVersionsDirectory(std::uint64_t applicationId,
                                            std::uint32_t installedVersion,
                                            std::string& buildIdHex) {
    buildIdHex.clear();
    if (installedVersion == 0 && applicationId == 0) {
        return false;
    }

    std::string cachedBuildId;
    if (LoadCachedBuildId(applicationId, installedVersion, cachedBuildId)) {
        buildIdHex = cachedBuildId;
        return true;
    }

    HttpResponse response;
    std::string error;
    const std::string url = std::string(kCheatVersionsDirectory) + FormatTitleId(applicationId) + ".json";
    if (!HttpGetToString(url, response, error) || response.statusCode < 200 || response.statusCode >= 300) {
        return false;
    }

    picojson::value root;
    const std::string parseError = picojson::parse(root, StripUtf8Bom(response.body));
    if (!parseError.empty() || !root.is<picojson::object>()) {
        return false;
    }

    const auto& object = root.get<picojson::object>();
    const auto versionIt = object.find(std::to_string(installedVersion));
    if (versionIt == object.end() || !versionIt->second.is<std::string>()) {
        return false;
    }

    buildIdHex = ToLowerAscii(versionIt->second.get<std::string>());
    if (buildIdHex.empty()) {
        return false;
    }

    SaveCachedBuildId(applicationId, installedVersion, buildIdHex);
    return true;
}

std::vector<InstalledTitle> LoadInstalledTitlesFull(std::string& note) {
    std::vector<InstalledTitle> titles;

    const Result initResult = nsInitialize();
    if (R_FAILED(initResult)) {
        note = "nsInitialize falhou. Serviço NS indisponível.";
        return titles;
    }

    std::vector<NsApplicationRecord> records(64);
    s32 offset = 0;
    s32 entryCount = 0;

    do {
        entryCount = 0;
        const Result listResult = nsListApplicationRecord(records.data(),
                                                          static_cast<s32>(records.size()),
                                                          offset,
                                                          &entryCount);
        if (R_FAILED(listResult)) {
            note = "nsListApplicationRecord falhou no modo full.";
            nsExit();
            return {};
        }

        for (s32 index = 0; index < entryCount; ++index) {
            InstalledTitle title;
            if (TryReadTitleMetadata(records[index].application_id, title)) {
                titles.push_back(title);
            }
        }

        offset += entryCount;
    } while (entryCount > 0);

    nsExit();
    note = u8"Títulos instalados carregados por varredura completa.";
    return titles;
}

std::vector<InstalledTitle> LoadInstalledTitlesFromCatalog(const CatalogIndex& catalog, std::string& note) {
    std::vector<InstalledTitle> titles;
    std::set<std::string> uniqueTitleIds;

    for (const CatalogEntry& entry : catalog.entries) {
        if (!entry.titleId.empty()) {
            uniqueTitleIds.insert(ToLowerAscii(entry.titleId));
        }
    }

    if (uniqueTitleIds.empty()) {
        note = "Catálogo sem title IDs para sondagem local.";
        return titles;
    }

    const Result initResult = nsInitialize();
    if (R_FAILED(initResult)) {
        note = "nsInitialize falhou. Emulador ou serviço NS indisponível.";
        return titles;
    }

    for (const std::string& titleIdHex : uniqueTitleIds) {
        const std::uint64_t applicationId = std::strtoull(titleIdHex.c_str(), nullptr, 16);
        InstalledTitle title;
        if (TryReadTitleMetadata(applicationId, title)) {
            titles.push_back(title);
        }
    }

    nsExit();
    note = "Títulos detectados por sondagem do catálogo.";
    return titles;
}

}  // namespace

bool TryResolveInstalledTitleBuildId(InstalledTitle& title, bool allowNetworkFallback, std::string& note) {
    note.clear();
    if (!title.buildIdHex.empty() || title.applicationId == 0) {
        return !title.buildIdHex.empty();
    }

    const bool safeConsoleServices = GetRuntimeEnvironment() == RuntimeEnvironment::Console &&
                                     !HasImportedTitlesFile() &&
                                     !HasEdenSdLayoutMarker() &&
                                     !IsLikelyEmulatorLoader() &&
                                     !IsUnsafeForwarderLikeContext();
    if (!safeConsoleServices) {
        note = "Build ID indisponível neste ambiente.";
        return false;
    }

    std::uint32_t installedVersion = 0;
    if (TryGetHighestInstalledVersionRaw(title.applicationId, installedVersion)) {
        if (!allowNetworkFallback) {
            note = "Build ID depende do banco remoto de versões.";
            return false;
        }

        std::string resolvedBuildId;
        if (TryResolveBuildIdFromVersionsDirectory(title.applicationId, installedVersion, resolvedBuildId)) {
            title.buildIdHex = ToLowerAscii(resolvedBuildId);
            note = "Build ID resolvida via banco remoto de versões.";
            return true;
        }
        note = "Build ID não encontrada no banco remoto de versões.";
    } else {
        note = "Versão instalada não pôde ser lida via NS.";
    }

    const bool safeConsoleDmnt = GetRuntimeEnvironment() == RuntimeEnvironment::Console &&
                                 !HasImportedTitlesFile() &&
                                 !HasEdenSdLayoutMarker() &&
                                 !IsLikelyEmulatorLoader() &&
                                 !IsUnsafeForwarderLikeContext();
    if (!safeConsoleDmnt) {
        return false;
    }

    std::string dmntBuildId;
    if (TryReadBuildIdFromDmnt(title.applicationId, dmntBuildId)) {
        title.buildIdHex = ToLowerAscii(dmntBuildId);
        note = "Build ID obtida via dmnt:cht.";
        return true;
    }

    return false;
}

bool InitializePlatform(PlatformSession& session, std::string& note) {
    if (R_SUCCEEDED(appletLockExit())) {
        session.exitLocked = true;
    }

    if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
        session.nifmReady = true;
    }

    socketInitializeDefault();
    session.socketReady = true;

    if (R_SUCCEEDED(framebufferCreate(&session.framebuffer,
                                      nwindowGetDefault(),
                                      kFramebufferWidth,
                                      kFramebufferHeight,
                                      PIXEL_FORMAT_RGBA_8888,
                                      2)) &&
        R_SUCCEEDED(framebufferMakeLinear(&session.framebuffer))) {
        session.framebufferReady = true;
    }

    if (R_SUCCEEDED(romfsInit())) {
        session.romfsReady = true;
    }

#if defined(MIL_ENABLE_NXLINK)
    nxlinkStdio();
#endif

    if (session.nifmReady) {
        note = session.romfsReady ? "Rede, vídeo, sockets e RomFS inicializados."
                                  : "Rede, vídeo e sockets inicializados. RomFS indisponível.";
    } else {
        note = session.romfsReady ? "Vídeo, sockets e RomFS inicializados. nifm indisponível."
                                  : "Vídeo e sockets inicializados. RomFS e nifm indisponíveis.";
    }
    return true;
}

void ShutdownPlatform(PlatformSession& session) {
    if (session.framebufferReady) {
        framebufferClose(&session.framebuffer);
        session.framebufferReady = false;
    }
    if (session.romfsReady) {
        romfsExit();
        session.romfsReady = false;
    }
    if (session.socketReady) {
        socketExit();
        session.socketReady = false;
    }
    if (session.nifmReady) {
        nifmExit();
        session.nifmReady = false;
    }
    if (session.exitLocked) {
        appletUnlockExit();
        session.exitLocked = false;
    }
}

std::vector<InstalledTitle> LoadInstalledTitles(const AppConfig& config, const CatalogIndex* catalog, std::string& note) {
    const RuntimeEnvironment environment = GetRuntimeEnvironment();
    const bool importedTitlesAvailable = HasImportedTitlesFile();
    const bool emulator = environment == RuntimeEnvironment::Emulator;
    const bool loaderInfoEmpty = ToLowerAscii(GetLoaderInfoString()).empty();
    const bool unsafeForwarderContext = IsUnsafeForwarderLikeContext();

    if (unsafeForwarderContext && !importedTitlesAvailable) {
        note = "Forwarder/titulo detectado com loader hbl/vazio. Leitura local por NS foi bloqueada neste contexto; use installed-titles-cache.json.";
        return {};
    }

    if (loaderInfoEmpty && !importedTitlesAvailable) {
        note = "Loader info vazio. Leitura local por NS foi bloqueada em modo seguro.";
        return {};
    }

    if (config.scanMode == InstalledTitleScanMode::Disabled) {
        if (environment == RuntimeEnvironment::Console) {
            note = "Console detectado. Leitura local sempre ativa; ignorando scan_mode=off.";
            return LoadInstalledTitlesFull(note);
        }
        if (environment == RuntimeEnvironment::Unknown) {
            note = "Ambiente não confirmado. Leitura local mantida em modo seguro.";
            return {};
        }
        note = "Leitura de títulos desativada em settings.ini.";
        return {};
    }

    if (config.scanMode == InstalledTitleScanMode::Auto) {
        if (environment == RuntimeEnvironment::Console) {
            note = "Console detectado. Modo automático usando leitura completa.";
            return LoadInstalledTitlesFull(note);
        }
        if (importedTitlesAvailable) {
            return LoadInstalledTitlesFromImportedManifest(note);
        }
        note = environment == RuntimeEnvironment::Emulator
                   ? "Emulador detectado. O homebrew não consegue ler a biblioteca do host diretamente; aguardando lista sincronizada em installed-titles-cache.json."
                   : "Ambiente não confirmado. Modo automático segue em fallback seguro até detectar console com confiança.";
        return {};
    }

    if (environment != RuntimeEnvironment::Console) {
        if (importedTitlesAvailable) {
            return LoadInstalledTitlesFromImportedManifest(note);
        }
        note = environment == RuntimeEnvironment::Emulator
                   ? "Emulador detectado. Servi??os NS do host n??o s??o confi??veis aqui; use lista sincronizada em installed-titles-cache.json."
                   : "Ambiente n??o confirmado. Leitura local por NS foi bloqueada para evitar crash em emuladores.";
        return {};
    }

    if (config.scanMode == InstalledTitleScanMode::Full) {
        return LoadInstalledTitlesFull(note);
    }

    if (config.scanMode == InstalledTitleScanMode::CatalogProbe) {
        if (catalog == nullptr) {
            note = "Catálogo indisponível. Sondagem local ignorada.";
            return {};
        }
        return LoadInstalledTitlesFromCatalog(*catalog, note);
    }

    note = "Leitura local não configurada.";
    return {};
}

std::string GetPreferredLanguageCode() {
    Result rc = setInitialize();
    if (R_FAILED(rc)) {
        return "en-US";
    }

    u64 languageCode = 0;
    rc = setGetSystemLanguage(&languageCode);
    setExit();
    if (R_FAILED(rc)) {
        return "en-US";
    }

    const char* languageChars = reinterpret_cast<const char*>(&languageCode);
    return std::string(languageChars, 4);
}

bool IsEmulatorEnvironment() {
    return GetRuntimeEnvironment() == RuntimeEnvironment::Emulator;
}

RuntimeEnvironment GetRuntimeEnvironment() {
    const std::string loaderInfo = ToLowerAscii(GetLoaderInfoString());

    if (HasImportedTitlesFile() || IsLikelyEmulatorLoader()) {
        return RuntimeEnvironment::Emulator;
    }

    if (IsUnsafeForwarderLikeContext()) {
        return RuntimeEnvironment::Unknown;
    }

    if (loaderInfo.find("hbmenu") != std::string::npos ||
        loaderInfo.find("hbl") != std::string::npos ||
        loaderInfo.find("nx-hbloader") != std::string::npos ||
        loaderInfo.find("sphaira") != std::string::npos) {
        return RuntimeEnvironment::Console;
    }

    return RuntimeEnvironment::Unknown;
}

const char* RuntimeEnvironmentLabel(RuntimeEnvironment environment) {
    switch (environment) {
        case RuntimeEnvironment::Unknown:
            return "unknown";
        case RuntimeEnvironment::Emulator:
            return "emulator";
        case RuntimeEnvironment::Console:
        default:
            return "console";
    }
}

std::string GetLoaderInfoSummary() {
    const std::string info = GetLoaderInfoString();
    if (info.empty()) {
        return "(empty)";
    }
    return info;
}

}  // namespace mil
