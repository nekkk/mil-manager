#include "mil/savegames.hpp"

#include <cstdlib>
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

std::uint64_t GetUint64(const picojson::object& object, const std::string& key) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return 0;
    }
    if (iterator->second.is<double>()) {
        const double value = iterator->second.get<double>();
        return value > 0.0 ? static_cast<std::uint64_t>(value) : 0;
    }
    if (iterator->second.is<std::string>()) {
        return static_cast<std::uint64_t>(std::strtoull(iterator->second.get<std::string>().c_str(), nullptr, 10));
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

bool ParseSaveVariant(const picojson::object& object, SaveVariantRecord& variant) {
    variant.id = GetString(object, "id");
    variant.label = GetString(object, "label");
    variant.category = GetString(object, "category");
    variant.saveKind = GetString(object, "saveKind");
    variant.layoutType = GetString(object, "layoutType");
    variant.platform = GetString(object, "platform");
    variant.author = GetString(object, "author");
    variant.language = GetString(object, "language");
    variant.updatedAt = GetString(object, "updatedAt");
    variant.assetId = GetString(object, "assetId");
    variant.assetType = GetString(object, "assetType");
    variant.relativePath = GetString(object, "relativePath");
    variant.downloadUrl = GetString(object, "downloadUrl");
    variant.contentHash = GetString(object, "contentHash");
    variant.sha256 = GetString(object, "sha256");
    variant.size = GetUint64(object, "size");
    variant.origins = GetStringArray(object, "origins");
    return !variant.id.empty() && (!variant.downloadUrl.empty() || !variant.relativePath.empty());
}

bool ParseSaveTitle(const picojson::object& object, SaveTitleRecord& title) {
    title.titleId = ToLowerAscii(GetString(object, "titleId"));
    title.name = GetString(object, "name");
    title.categories = GetStringArray(object, "categories");

    const auto variantsIt = object.find("variants");
    if (variantsIt != object.end() && variantsIt->second.is<picojson::array>()) {
        for (const auto& item : variantsIt->second.get<picojson::array>()) {
            if (!item.is<picojson::object>()) {
                continue;
            }
            SaveVariantRecord variant;
            if (ParseSaveVariant(item.get<picojson::object>(), variant)) {
                title.variants.push_back(variant);
            }
        }
    }

    return !title.titleId.empty() && !title.variants.empty();
}

}  // namespace

bool LoadSavesIndexFromJsonString(const std::string& json, SavesIndex& index, std::string& error) {
    picojson::value root;
    const std::string normalizedJson = StripUtf8Bom(json);
    const std::string parseError = picojson::parse(root, normalizedJson);
    if (!parseError.empty()) {
        error = "Erro ao ler saves-index JSON: " + parseError;
        return false;
    }
    if (!root.is<picojson::object>()) {
        error = "Saves-index invalido: raiz nao e um objeto.";
        return false;
    }

    const auto& object = root.get<picojson::object>();
    index = {};
    index.schemaVersion = GetString(object, "schemaVersion");
    index.generatedAt = GetString(object, "generatedAt");
    index.generator = GetString(object, "generator");
    index.catalogRevision = GetString(object, "catalogRevision");
    index.deliveryBaseUrl = GetString(object, "deliveryBaseUrl");

    const auto titlesIt = object.find("titles");
    if (titlesIt == object.end() || !titlesIt->second.is<picojson::array>()) {
        error = "Saves-index invalido: campo titles ausente.";
        return false;
    }

    for (const auto& item : titlesIt->second.get<picojson::array>()) {
        if (!item.is<picojson::object>()) {
            continue;
        }
        SaveTitleRecord title;
        if (ParseSaveTitle(item.get<picojson::object>(), title)) {
            index.titles.push_back(title);
        }
    }

    if (index.titles.empty()) {
        error = "Saves-index carregado, mas sem titulos validos.";
        return false;
    }

    return true;
}

bool LoadSavesIndexFromFile(const std::string& path, SavesIndex& index, std::string& error) {
    std::ifstream input(path);
    if (!input.good()) {
        error = "Nao foi possivel abrir " + path;
        return false;
    }

    std::stringstream buffer;
    buffer << input.rdbuf();
    return LoadSavesIndexFromJsonString(buffer.str(), index, error);
}

}  // namespace mil
