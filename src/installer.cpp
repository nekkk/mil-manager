#include "mil/installer.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cstdio>
#include <cstdint>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "mil/config.hpp"
#include "mil/http.hpp"

namespace mil {

namespace {

constexpr std::uint64_t kDownloadFreeSpaceMarginBytes = 64ULL * 1024ULL * 1024ULL;

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

bool TryGetFileSize(const std::string& path, std::uint64_t& sizeOut) {
    struct stat fileStat = {};
    if (stat(path.c_str(), &fileStat) != 0) {
        return false;
    }
    sizeOut = static_cast<std::uint64_t>(fileStat.st_size);
    return true;
}

bool TryGetFreeSpaceBytes(const std::string& path, std::uint64_t& freeBytesOut) {
    struct statvfs fileSystemStat = {};
    if (statvfs(path.c_str(), &fileSystemStat) != 0) {
        return false;
    }

    freeBytesOut = static_cast<std::uint64_t>(fileSystemStat.f_bavail) *
                   static_cast<std::uint64_t>(fileSystemStat.f_frsize);
    return true;
}

std::string ParentDirectory(std::string path) {
    const std::size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return {};
    }
    return path.substr(0, pos);
}

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string SanitizeArchivePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (StartsWith(path, "./")) {
        path.erase(0, 2);
    }
    if (path.empty() || StartsWith(path, "/") || path.find("..") != std::string::npos) {
        return {};
    }
    return path;
}

std::string ReceiptPath(const std::string& packageId) {
    return std::string(kReceiptsDir) + "/" + packageId + ".ini";
}

std::string CurrentTimestamp() {
    const std::time_t now = std::time(nullptr);
    char buffer[64] = {};
    std::tm* timeInfo = std::gmtime(&now);
    if (!timeInfo) {
        return {};
    }
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", timeInfo);
    return buffer;
}

bool SaveReceipt(const InstallReceipt& receipt, std::string& error) {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kConfigRootDir);
    EnsureDirectory(kReceiptsDir);

    std::ofstream output(ReceiptPath(receipt.packageId), std::ios::trunc);
    if (!output.good()) {
        error = "Não foi possível gravar o recibo de instalação.";
        return false;
    }

    output << "package_id=" << receipt.packageId << '\n';
    output << "package_version=" << receipt.packageVersion << '\n';
    output << "title_id=" << receipt.titleId << '\n';
    output << "install_root=" << receipt.installRoot << '\n';
    output << "source_url=" << receipt.sourceUrl << '\n';
    output << "installed_at=" << receipt.installedAt << '\n';
    output << "game_version=" << receipt.gameVersion << '\n';
    for (const std::string& file : receipt.files) {
        output << "file=" << file << '\n';
    }

    return output.good();
}

InstallReceipt ParseReceiptFile(const std::string& path) {
    InstallReceipt receipt;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }
        const std::string key = ToLowerAscii(Trim(line.substr(0, equalsPos)));
        const std::string value = Trim(line.substr(equalsPos + 1));
        if (key == "package_id") {
            receipt.packageId = value;
        } else if (key == "package_version") {
            receipt.packageVersion = value;
        } else if (key == "title_id") {
            receipt.titleId = value;
        } else if (key == "install_root") {
            receipt.installRoot = value;
        } else if (key == "source_url") {
            receipt.sourceUrl = value;
        } else if (key == "installed_at") {
            receipt.installedAt = value;
        } else if (key == "game_version") {
            receipt.gameVersion = value;
        } else if (key == "file") {
            receipt.files.push_back(value);
        }
    }
    return receipt;
}

bool RemoveEmptyParents(const std::string& path) {
    std::string current = ParentDirectory(path);
    while (!current.empty() && current != "sdmc:" && current != "sdmc:/") {
        DIR* dir = opendir(current.c_str());
        if (!dir) {
            return false;
        }
        int count = 0;
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") {
                ++count;
                break;
            }
        }
        closedir(dir);
        if (count > 0) {
            break;
        }
        rmdir(current.c_str());
        current = ParentDirectory(current);
    }
    return true;
}

bool ShouldInstallUnderAtmosphereContents(const CatalogEntry& entry) {
    return entry.section != ContentSection::SaveGames;
}

std::string InstallRootForEntry(const CatalogEntry& entry) {
    if (ShouldInstallUnderAtmosphereContents(entry)) {
        return "sdmc:/atmosphere/contents/";
    }
    return "sdmc:/";
}

std::string ResolveInstallPath(const CatalogEntry& entry, const std::string& relativePath) {
    if (!ShouldInstallUnderAtmosphereContents(entry)) {
        return "sdmc:/" + relativePath;
    }

    const std::string normalized = ToLowerAscii(relativePath);
    if (normalized.rfind("atmosphere/contents/", 0) == 0) {
        return "sdmc:/" + relativePath;
    }
    if (normalized.rfind("contents/", 0) == 0) {
        return "sdmc:/atmosphere/" + relativePath;
    }
    return "sdmc:/atmosphere/contents/" + relativePath;
}

bool ExtractZip(const CatalogEntry& catalogEntry, const std::string& zipPath, std::vector<std::string>& extractedFiles, std::string& error) {
    extractedFiles.clear();

    struct archive* reader = archive_read_new();
    archive_read_support_filter_all(reader);
    archive_read_support_format_zip(reader);

    if (archive_read_open_filename(reader, zipPath.c_str(), 10240) != ARCHIVE_OK) {
        error = archive_error_string(reader);
        archive_read_free(reader);
        return false;
    }

    struct archive_entry* archiveEntry = nullptr;
    while (archive_read_next_header(reader, &archiveEntry) == ARCHIVE_OK) {
        const char* rawPath = archive_entry_pathname(archiveEntry);
        const std::string relativePath = rawPath ? SanitizeArchivePath(rawPath) : std::string();
        if (relativePath.empty()) {
            archive_read_data_skip(reader);
            continue;
        }

        const std::string fullPath = ResolveInstallPath(catalogEntry, relativePath);
        if (archive_entry_filetype(archiveEntry) == AE_IFDIR) {
            EnsureDirectory(fullPath);
            archive_read_data_skip(reader);
            continue;
        }

        EnsureDirectory(ParentDirectory(fullPath));
        FILE* output = fopen(fullPath.c_str(), "wb");
        if (!output) {
            error = "Não foi possível criar " + fullPath;
            archive_read_free(reader);
            return false;
        }

        const void* buffer = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        while (archive_read_data_block(reader, &buffer, &size, &offset) == ARCHIVE_OK) {
            if (fwrite(buffer, 1, size, output) != size) {
                fclose(output);
                error = "Falha ao escrever arquivo extraido.";
                archive_read_free(reader);
                return false;
            }
        }

        fclose(output);
        extractedFiles.push_back(fullPath);
    }

    archive_read_free(reader);
    return true;
}

}  // namespace

bool ExtractZipToDirectory(const std::string& zipPath,
                           const std::string& destinationDir,
                           std::vector<std::string>* extractedFiles,
                           std::string& error) {
    if (extractedFiles != nullptr) {
        extractedFiles->clear();
    }

    EnsureDirectory(destinationDir);

    struct archive* reader = archive_read_new();
    archive_read_support_filter_all(reader);
    archive_read_support_format_zip(reader);

    if (archive_read_open_filename(reader, zipPath.c_str(), 10240) != ARCHIVE_OK) {
        error = archive_error_string(reader);
        archive_read_free(reader);
        return false;
    }

    struct archive_entry* archiveEntry = nullptr;
    while (archive_read_next_header(reader, &archiveEntry) == ARCHIVE_OK) {
        const char* rawPath = archive_entry_pathname(archiveEntry);
        const std::string relativePath = rawPath ? SanitizeArchivePath(rawPath) : std::string();
        if (relativePath.empty()) {
            archive_read_data_skip(reader);
            continue;
        }

        const std::string fullPath = destinationDir + "/" + relativePath;
        if (archive_entry_filetype(archiveEntry) == AE_IFDIR) {
            EnsureDirectory(fullPath);
            archive_read_data_skip(reader);
            continue;
        }

        EnsureDirectory(ParentDirectory(fullPath));
        FILE* output = fopen(fullPath.c_str(), "wb");
        if (!output) {
            error = "NÃ£o foi possÃ­vel criar " + fullPath;
            archive_read_free(reader);
            return false;
        }

        const void* buffer = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        while (archive_read_data_block(reader, &buffer, &size, &offset) == ARCHIVE_OK) {
            if (fwrite(buffer, 1, size, output) != size) {
                fclose(output);
                error = "Falha ao escrever arquivo extraido.";
                archive_read_free(reader);
                return false;
            }
        }

        fclose(output);
        if (extractedFiles != nullptr) {
            extractedFiles->push_back(fullPath);
        }
    }

    archive_read_free(reader);
    return true;
}

std::vector<InstallReceipt> LoadInstallReceipts(std::string& note) {
    std::vector<InstallReceipt> receipts;
    DIR* dir = opendir(kReceiptsDir);
    if (!dir) {
        note = "Nenhum recibo de instalação encontrado ainda.";
        return receipts;
    }

    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (name.size() < 5 || name.substr(name.size() - 4) != ".ini") {
            continue;
        }
        receipts.push_back(ParseReceiptFile(std::string(kReceiptsDir) + "/" + name));
    }

    closedir(dir);
    note = "Recibos de instalação carregados.";
    return receipts;
}

bool FindReceiptForPackage(const std::vector<InstallReceipt>& receipts, const std::string& packageId, InstallReceipt* receipt) {
    for (const InstallReceipt& item : receipts) {
        if (item.packageId == packageId) {
            if (receipt != nullptr) {
                *receipt = item;
            }
            return true;
        }
    }
    return false;
}

bool InstallPackage(const CatalogEntry& entry, const InstalledTitle* installedTitle, InstallReceipt& receipt, std::string& error) {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kConfigRootDir);
    EnsureDirectory(kCacheDir);

    const std::string zipPath = std::string(kCacheDir) + "/" + entry.id + ".zip";
    HttpDownloadInfo downloadInfo;
    std::string downloadInfoError;
    const bool hasDownloadInfo = HttpGetDownloadInfo(entry.downloadUrl, downloadInfo, downloadInfoError);

    if (hasDownloadInfo && downloadInfo.sizeKnown) {
        std::uint64_t partialSize = 0;
        if (!TryGetFileSize(zipPath, partialSize)) {
            partialSize = 0;
        }

        std::uint64_t freeBytes = 0;
        if (TryGetFreeSpaceBytes("sdmc:/", freeBytes)) {
            const std::uint64_t remainingBytes = downloadInfo.contentLength > partialSize
                                               ? (downloadInfo.contentLength - partialSize)
                                               : 0;
            const std::uint64_t requiredBytes = remainingBytes + kDownloadFreeSpaceMarginBytes;
            if (freeBytes < requiredBytes) {
                error = "Espaco livre insuficiente para o download e instalacao do pacote.";
                return false;
            }
        }
    }

    std::size_t bytesDownloaded = 0;
    if (!HttpDownloadToFile(entry.downloadUrl, zipPath, &bytesDownloaded, error)) {
        return false;
    }
    if (bytesDownloaded == 0) {
        remove(zipPath.c_str());
        error = "Download concluído, mas o arquivo estava vazio.";
        return false;
    }

    std::vector<std::string> extractedFiles;
    if (!ExtractZip(entry, zipPath, extractedFiles, error)) {
        remove(zipPath.c_str());
        return false;
    }

    remove(zipPath.c_str());

    receipt = {};
    receipt.packageId = entry.id;
    receipt.packageVersion = entry.packageVersion;
    receipt.titleId = ToLowerAscii(entry.titleId);
    receipt.installRoot = InstallRootForEntry(entry);
    receipt.sourceUrl = entry.downloadUrl;
    receipt.installedAt = CurrentTimestamp();
    receipt.gameVersion = installedTitle ? installedTitle->displayVersion : std::string();
    receipt.files = std::move(extractedFiles);

    if (!SaveReceipt(receipt, error)) {
        for (const std::string& file : receipt.files) {
            remove(file.c_str());
            RemoveEmptyParents(file);
        }
        return false;
    }

    return true;
}

bool InstallCheatText(const CatalogEntry& entry,
                      const std::string& buildId,
                      const std::string& downloadUrl,
                      const InstalledTitle* installedTitle,
                      InstallReceipt& receipt,
                      std::string& error) {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kConfigRootDir);
    EnsureDirectory(kCacheDir);
    EnsureDirectory("sdmc:/atmosphere");
    EnsureDirectory("sdmc:/atmosphere/contents");

    if (entry.titleId.empty()) {
        error = "Title ID ausente para instalacao de cheat.";
        return false;
    }

    const std::string normalizedTitleId = ToLowerAscii(entry.titleId);
    const std::string normalizedBuildId = ToLowerAscii(buildId);
    if (normalizedBuildId.empty()) {
        error = "Build ID ausente para instalacao de cheat.";
        return false;
    }

    const std::string tempPath = std::string(kCacheDir) + "/" + entry.id + "-" + normalizedBuildId + ".txt";
    std::size_t bytesDownloaded = 0;
    HttpDownloadOptions options;
    options.probeDownloadInfo = false;
    options.allowResume = false;
    options.requestTimeoutMs = 60000;
    if (!HttpDownloadToFileWithOptions(downloadUrl, tempPath, options, &bytesDownloaded, error)) {
        return false;
    }
    if (bytesDownloaded == 0) {
        remove(tempPath.c_str());
        error = "Download do cheat concluido, mas o arquivo estava vazio.";
        return false;
    }

    const std::string cheatDir = "sdmc:/atmosphere/contents/" + normalizedTitleId + "/cheats";
    const std::string destinationPath = cheatDir + "/" + normalizedBuildId + ".txt";
    EnsureDirectory(cheatDir);

    std::ifstream input(tempPath, std::ios::binary);
    if (!input.good()) {
        remove(tempPath.c_str());
        error = "Nao foi possivel abrir o cheat baixado.";
        return false;
    }
    std::ofstream output(destinationPath, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        input.close();
        remove(tempPath.c_str());
        error = "Nao foi possivel gravar o cheat no destino.";
        return false;
    }

    output << input.rdbuf();
    input.close();
    output.close();
    remove(tempPath.c_str());

    if (!output.good()) {
        error = "Falha ao salvar o cheat no destino.";
        return false;
    }

    receipt = {};
    receipt.packageId = entry.id;
    receipt.packageVersion = entry.packageVersion.empty() ? normalizedBuildId : entry.packageVersion;
    receipt.titleId = normalizedTitleId;
    receipt.installRoot = cheatDir;
    receipt.sourceUrl = downloadUrl;
    receipt.installedAt = CurrentTimestamp();
    receipt.gameVersion = installedTitle ? installedTitle->displayVersion : std::string();
    receipt.files = {destinationPath};

    if (!SaveReceipt(receipt, error)) {
        remove(destinationPath.c_str());
        RemoveEmptyParents(destinationPath);
        return false;
    }

    return true;
}

bool InstallCheatTextFromFile(const CatalogEntry& entry,
                              const std::string& buildId,
                              const std::string& sourcePath,
                              const InstalledTitle* installedTitle,
                              InstallReceipt& receipt,
                              std::string& error) {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kConfigRootDir);
    EnsureDirectory(kCacheDir);
    EnsureDirectory("sdmc:/atmosphere");
    EnsureDirectory("sdmc:/atmosphere/contents");

    if (entry.titleId.empty()) {
        error = "Title ID ausente para instalacao de cheat.";
        return false;
    }

    const std::string normalizedTitleId = ToLowerAscii(entry.titleId);
    const std::string normalizedBuildId = ToLowerAscii(buildId);
    if (normalizedBuildId.empty()) {
        error = "Build ID ausente para instalacao de cheat.";
        return false;
    }

    std::ifstream input(sourcePath, std::ios::binary);
    if (!input.good()) {
        error = "Nao foi possivel abrir o arquivo local do cheat.";
        return false;
    }

    const std::string cheatDir = "sdmc:/atmosphere/contents/" + normalizedTitleId + "/cheats";
    const std::string destinationPath = cheatDir + "/" + normalizedBuildId + ".txt";
    EnsureDirectory(cheatDir);

    std::ofstream output(destinationPath, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        input.close();
        error = "Nao foi possivel gravar o cheat no destino.";
        return false;
    }

    output << input.rdbuf();
    input.close();
    output.close();

    if (!output.good()) {
        error = "Falha ao salvar o cheat no destino.";
        return false;
    }

    receipt = {};
    receipt.packageId = entry.id;
    receipt.packageVersion = entry.packageVersion.empty() ? normalizedBuildId : entry.packageVersion;
    receipt.titleId = normalizedTitleId;
    receipt.installRoot = cheatDir;
    receipt.sourceUrl = sourcePath;
    receipt.installedAt = CurrentTimestamp();
    receipt.gameVersion = installedTitle ? installedTitle->displayVersion : std::string();
    receipt.files = {destinationPath};

    if (!SaveReceipt(receipt, error)) {
        remove(destinationPath.c_str());
        RemoveEmptyParents(destinationPath);
        return false;
    }

    return true;
}

bool UninstallPackage(const InstallReceipt& receipt, std::string& error) {
    for (const std::string& file : receipt.files) {
        remove(file.c_str());
        RemoveEmptyParents(file);
    }

    if (remove(ReceiptPath(receipt.packageId).c_str()) != 0) {
        error = "Arquivos removidos, mas o recibo não foi apagado.";
        return false;
    }

    return true;
}

}  // namespace mil
