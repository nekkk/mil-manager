#include "mil/cheats.hpp"

#include <fstream>
#include <sstream>

#include "picojson.h"

namespace mil {

namespace {

std::string StripUtf8Bom(std::string value) {
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        value.erase(0, 3);
    }
    return value;
}

std::string GetString(const picojson::object& object, const std::string& key) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->second.is<std::string>()) {
        return {};
    }
    return iterator->second.get<std::string>();
}

int GetInt(const picojson::object& object, const std::string& key) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return 0;
    }
    if (iterator->second.is<double>()) {
        return static_cast<int>(iterator->second.get<double>());
    }
    if (iterator->second.is<std::string>()) {
        return std::atoi(iterator->second.get<std::string>().c_str());
    }
    return 0;
}

std::vector<std::string> GetStringArray(const picojson::object& object, const std::string& key) {
    std::vector<std::string> result;
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->second.is<picojson::array>()) {
        return result;
    }
    for (const auto& item : iterator->second.get<picojson::array>()) {
        if (item.is<std::string>()) {
            result.push_back(item.get<std::string>());
        }
    }
    return result;
}

bool ParseCheatEntry(const picojson::object& object, CheatEntryRecord& entry) {
    entry.id = GetString(object, "id");
    entry.title = GetString(object, "title");
    entry.primarySource = GetString(object, "primarySource");
    entry.sources = GetStringArray(object, "sources");
    entry.categories = GetStringArray(object, "categories");
    entry.contentHash = GetString(object, "contentHash");
    entry.cheatCount = GetInt(object, "cheatCount");
    entry.lineCount = GetInt(object, "lineCount");
    entry.relativePath = GetString(object, "relativePath");
    entry.downloadUrl = GetString(object, "downloadUrl");
    entry.originUrls = GetStringArray(object, "originUrls");
    entry.priorityRank = GetInt(object, "priorityRank");
    return !entry.id.empty() && (!entry.relativePath.empty() || !entry.downloadUrl.empty());
}

bool ParseCheatBuild(const picojson::object& object, CheatBuildRecord& build) {
    build.buildId = ToLowerAscii(GetString(object, "buildId"));
    build.categories = GetStringArray(object, "categories");
    const auto entriesIt = object.find("entries");
    if (entriesIt != object.end() && entriesIt->second.is<picojson::array>()) {
        for (const auto& item : entriesIt->second.get<picojson::array>()) {
            if (!item.is<picojson::object>()) {
                continue;
            }
            CheatEntryRecord entry;
            if (ParseCheatEntry(item.get<picojson::object>(), entry)) {
                build.entries.push_back(entry);
            }
        }
    }
    return !build.buildId.empty() && !build.entries.empty();
}

bool ParseCheatTitle(const picojson::object& object, CheatTitleRecord& title) {
    title.titleId = ToLowerAscii(GetString(object, "titleId"));
    title.name = GetString(object, "name");
    const auto buildsIt = object.find("builds");
    if (buildsIt != object.end() && buildsIt->second.is<picojson::array>()) {
        for (const auto& item : buildsIt->second.get<picojson::array>()) {
            if (!item.is<picojson::object>()) {
                continue;
            }
            CheatBuildRecord build;
            if (ParseCheatBuild(item.get<picojson::object>(), build)) {
                title.builds.push_back(build);
            }
        }
    }
    return !title.titleId.empty() && !title.builds.empty();
}

}  // namespace

bool LoadCheatsIndexFromJsonString(const std::string& json, CheatsIndex& index, std::string& error) {
    picojson::value root;
    const std::string normalizedJson = StripUtf8Bom(json);
    const std::string parseError = picojson::parse(root, normalizedJson);
    if (!parseError.empty()) {
        error = "Erro ao ler cheats-index JSON: " + parseError;
        return false;
    }
    if (!root.is<picojson::object>()) {
        error = "Cheats-index invalido: raiz nao e um objeto.";
        return false;
    }

    const auto& object = root.get<picojson::object>();
    index = {};
    index.schemaVersion = GetString(object, "schemaVersion");
    index.generatedAt = GetString(object, "generatedAt");
    index.generator = GetString(object, "generator");
    index.catalogRevision = GetString(object, "catalogRevision");
    index.cheatsPackRevision = GetString(object, "cheatsPackRevision");
    index.cheatsPackUrl = GetString(object, "cheatsPackUrl");
    index.cheatsPackSha256 = GetString(object, "cheatsPackSha256");
    index.watchedTitleIds = GetStringArray(object, "watchedTitleIds");

    const auto titlesIt = object.find("titles");
    if (titlesIt == object.end() || !titlesIt->second.is<picojson::array>()) {
        error = "Cheats-index invalido: campo titles ausente.";
        return false;
    }

    for (const auto& item : titlesIt->second.get<picojson::array>()) {
        if (!item.is<picojson::object>()) {
            continue;
        }
        CheatTitleRecord title;
        if (ParseCheatTitle(item.get<picojson::object>(), title)) {
            index.titles.push_back(title);
        }
    }

    if (index.titles.empty()) {
        error = "Cheats-index carregado, mas sem titulos validos.";
        return false;
    }

    return true;
}

bool LoadCheatsIndexFromFile(const std::string& path, CheatsIndex& index, std::string& error) {
    std::ifstream input(path);
    if (!input.good()) {
        error = "Nao foi possivel abrir " + path;
        return false;
    }

    std::stringstream buffer;
    buffer << input.rdbuf();
    return LoadCheatsIndexFromJsonString(buffer.str(), index, error);
}

}  // namespace mil
