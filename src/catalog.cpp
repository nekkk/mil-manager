#include "mil/catalog.hpp"

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

bool GetBool(const picojson::object& object, const std::string& key) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->second.is<bool>()) {
        return false;
    }
    return iterator->second.get<bool>();
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

CompatibilityRule ParseCompatibilityRule(const picojson::object& object) {
    CompatibilityRule rule;
    rule.minGameVersion = GetString(object, "minGameVersion");
    rule.maxGameVersion = GetString(object, "maxGameVersion");
    rule.exactGameVersions = GetStringArray(object, "exactGameVersions");
    return rule;
}

bool ParseVariant(const picojson::object& object, CatalogVariant& variant) {
    variant.id = GetString(object, "id");
    variant.label = GetString(object, "label");
    variant.downloadUrl = GetString(object, "downloadUrl");
    variant.packageVersion = GetString(object, "packageVersion");
    if (variant.packageVersion.empty()) {
        variant.packageVersion = GetString(object, "version");
    }
    variant.contentRevision = GetString(object, "contentRevision");

    const auto compatibilityIt = object.find("compatibility");
    if (compatibilityIt != object.end() && compatibilityIt->second.is<picojson::object>()) {
        variant.compatibility = ParseCompatibilityRule(compatibilityIt->second.get<picojson::object>());
    }

    return !variant.id.empty() && !variant.downloadUrl.empty();
}

bool ParseEntry(const picojson::object& object, CatalogEntry& entry) {
    entry.id = GetString(object, "id");
    entry.titleId = ToLowerAscii(GetString(object, "titleId"));
    entry.name = GetString(object, "name");
    entry.introPtBr = GetString(object, "introPtBr");
    if (entry.introPtBr.empty()) {
        entry.introPtBr = GetString(object, "intro");
    }
    entry.introEnUs = GetString(object, "introEnUs");
    if (entry.introEnUs.empty()) {
        entry.introEnUs = entry.introPtBr;
    }
    entry.intro = entry.introPtBr;
    entry.summaryPtBr = GetString(object, "summaryPtBr");
    if (entry.summaryPtBr.empty()) {
        entry.summaryPtBr = GetString(object, "summary");
    }
    entry.summaryEnUs = GetString(object, "summaryEnUs");
    if (entry.summaryEnUs.empty()) {
        entry.summaryEnUs = entry.summaryPtBr;
    }
    entry.summary = entry.summaryPtBr;
    entry.author = GetString(object, "author");
    entry.packageVersion = GetString(object, "packageVersion");
    if (entry.packageVersion.empty()) {
        entry.packageVersion = GetString(object, "version");
    }
    entry.contentRevision = GetString(object, "contentRevision");
    entry.language = GetString(object, "language");
    entry.contentTypes = GetStringArray(object, "contentTypes");
    entry.downloadUrl = GetString(object, "downloadUrl");
    entry.detailsUrl = GetString(object, "detailsUrl");
    entry.coverUrl = GetString(object, "coverUrl");
    if (entry.coverUrl.empty()) {
        entry.coverUrl = GetString(object, "imageUrl");
    }
    if (entry.coverUrl.empty()) {
        entry.coverUrl = GetString(object, "bannerUrl");
    }
    if (entry.coverUrl.empty()) {
        entry.coverUrl = GetString(object, "iconUrl");
    }
    entry.iconUrl = GetString(object, "iconUrl");
    if (entry.iconUrl.empty()) {
        entry.iconUrl = entry.coverUrl;
    }
    entry.thumbnailUrl = GetString(object, "thumbnailUrl");
    if (entry.thumbnailUrl.empty()) {
        entry.thumbnailUrl = entry.coverUrl;
    }
    entry.tags = GetStringArray(object, "tags");
    entry.section = ParseSection(GetString(object, "section"));
    entry.featured = GetBool(object, "featured");

    const auto compatibilityIt = object.find("compatibility");
    if (compatibilityIt != object.end() && compatibilityIt->second.is<picojson::object>()) {
        entry.compatibility = ParseCompatibilityRule(compatibilityIt->second.get<picojson::object>());
    }

    const auto variantsIt = object.find("variants");
    if (variantsIt != object.end() && variantsIt->second.is<picojson::array>()) {
        for (const auto& item : variantsIt->second.get<picojson::array>()) {
            if (!item.is<picojson::object>()) {
                continue;
            }
            CatalogVariant variant;
            if (ParseVariant(item.get<picojson::object>(), variant)) {
                entry.variants.push_back(variant);
            }
        }
    }

    const bool hasDirectInstallPayload = !entry.downloadUrl.empty() || !entry.variants.empty();
    const bool isCheatAggregate = entry.section == ContentSection::Cheats;
    return !entry.id.empty() && !entry.name.empty() && (hasDirectInstallPayload || isCheatAggregate);
}

}  // namespace

bool LoadCatalogFromJsonString(const std::string& json, CatalogIndex& catalog, std::string& error) {
    picojson::value root;
    const std::string normalizedJson = StripUtf8Bom(json);
    const std::string parseError = picojson::parse(root, normalizedJson);
    if (!parseError.empty()) {
        error = "Erro ao ler catálogo JSON: " + parseError;
        return false;
    }
    if (!root.is<picojson::object>()) {
        error = "Catálogo inválido: raiz não é um objeto.";
        return false;
    }

    const auto& object = root.get<picojson::object>();
    catalog = {};
    catalog.catalogName = GetString(object, "catalogName");
    catalog.catalogRevision = GetString(object, "catalogRevision");
    catalog.channel = GetString(object, "channel");
    catalog.schemaVersion = GetString(object, "schemaVersion");
    catalog.generatedAt = GetString(object, "generatedAt");
    catalog.thumbPackRevision = GetString(object, "thumbPackRevision");
    catalog.thumbPackUrl = GetString(object, "thumbPackUrl");
    catalog.thumbPackSha256 = GetString(object, "thumbPackSha256");

    const auto entriesIt = object.find("entries");
    if (entriesIt == object.end() || !entriesIt->second.is<picojson::array>()) {
        error = "Catálogo inválido: campo entries ausente.";
        return false;
    }

    for (const auto& item : entriesIt->second.get<picojson::array>()) {
        if (!item.is<picojson::object>()) {
            continue;
        }
        CatalogEntry entry;
        if (ParseEntry(item.get<picojson::object>(), entry)) {
            catalog.entries.push_back(entry);
        }
    }

    if (catalog.entries.empty()) {
        error = "Catálogo carregado, mas sem entradas válidas.";
        return false;
    }

    return true;
}

bool LoadCatalogFromFile(const std::string& path, CatalogIndex& catalog, std::string& error) {
    std::ifstream input(path);
    if (!input.good()) {
        error = "Não foi possível abrir " + path;
        return false;
    }

    std::stringstream buffer;
    buffer << input.rdbuf();
    return LoadCatalogFromJsonString(buffer.str(), catalog, error);
}

}  // namespace mil
