#include "mil/installer.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
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

bool PathExists(const std::string& path) {
    struct stat fileStat = {};
    return stat(path.c_str(), &fileStat) == 0;
}

bool IsDirectoryPath(const std::string& path) {
    struct stat fileStat = {};
    return stat(path.c_str(), &fileStat) == 0 && S_ISDIR(fileStat.st_mode);
}

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    if (left.back() == '/') {
        return left + right;
    }
    return left + "/" + right;
}

std::vector<std::string> ListDirectoryEntries(const std::string& path) {
    std::vector<std::string> entries;
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        return entries;
    }

    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        entries.push_back(name);
    }

    closedir(dir);
    std::sort(entries.begin(), entries.end());
    return entries;
}

bool RemovePathRecursively(const std::string& path, std::string& error) {
    if (!PathExists(path)) {
        return true;
    }

    if (IsDirectoryPath(path)) {
        for (const std::string& name : ListDirectoryEntries(path)) {
            if (!RemovePathRecursively(JoinPath(path, name), error)) {
                return false;
            }
        }
        if (rmdir(path.c_str()) != 0) {
            error = "Nao foi possivel remover o diretorio " + path;
            return false;
        }
        return true;
    }

    if (remove(path.c_str()) != 0) {
        error = "Nao foi possivel remover o arquivo " + path;
        return false;
    }
    return true;
}

bool ClearDirectoryContents(const std::string& path, std::string& error) {
    EnsureDirectory(path);
    for (const std::string& name : ListDirectoryEntries(path)) {
        if (!RemovePathRecursively(JoinPath(path, name), error)) {
            return false;
        }
    }
    return true;
}

std::uint64_t DirectorySizeBytes(const std::string& path) {
    struct stat fileStat = {};
    if (stat(path.c_str(), &fileStat) != 0) {
        return 0;
    }
    if (!S_ISDIR(fileStat.st_mode)) {
        return static_cast<std::uint64_t>(fileStat.st_size);
    }

    std::uint64_t total = 0;
    for (const std::string& name : ListDirectoryEntries(path)) {
        total += DirectorySizeBytes(JoinPath(path, name));
    }
    return total;
}

bool CopyFileBinary(const std::string& sourcePath,
                    const std::string& destinationPath,
                    std::string& error,
                    HttpProgressCallback progressCallback,
                    void* progressUserData,
                    std::uint64_t& completedBytes,
                    std::uint64_t totalBytes) {
    std::ifstream input(sourcePath, std::ios::binary);
    if (!input.good()) {
        error = "Nao foi possivel abrir " + sourcePath;
        return false;
    }

    EnsureDirectory(ParentDirectory(destinationPath));
    std::ofstream output(destinationPath, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        error = "Nao foi possivel criar " + destinationPath;
        return false;
    }

    char buffer[64 * 1024];
    while (input.good()) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize chunk = input.gcount();
        if (chunk <= 0) {
            break;
        }
        output.write(buffer, chunk);
        if (!output.good()) {
            error = "Falha ao gravar " + destinationPath;
            return false;
        }
        completedBytes += static_cast<std::uint64_t>(chunk);
        if (progressCallback != nullptr) {
            progressCallback(completedBytes, totalBytes, true, progressUserData);
        }
    }

    return true;
}

bool CopyDirectoryTree(const std::string& sourcePath,
                       const std::string& destinationPath,
                       std::vector<std::string>* copiedFiles,
                       std::string& error,
                       HttpProgressCallback progressCallback,
                       void* progressUserData,
                       std::uint64_t& completedBytes,
                       std::uint64_t totalBytes) {
    struct stat fileStat = {};
    if (stat(sourcePath.c_str(), &fileStat) != 0) {
        error = "Nao foi possivel acessar " + sourcePath;
        return false;
    }

    if (S_ISDIR(fileStat.st_mode)) {
        EnsureDirectory(destinationPath);
        for (const std::string& name : ListDirectoryEntries(sourcePath)) {
            if (!CopyDirectoryTree(JoinPath(sourcePath, name),
                                   JoinPath(destinationPath, name),
                                   copiedFiles,
                                   error,
                                   progressCallback,
                                   progressUserData,
                                   completedBytes,
                                   totalBytes)) {
                return false;
            }
        }
        return true;
    }

    if (!CopyFileBinary(sourcePath,
                        destinationPath,
                        error,
                        progressCallback,
                        progressUserData,
                        completedBytes,
                        totalBytes)) {
        return false;
    }

    if (copiedFiles != nullptr) {
        copiedFiles->push_back(destinationPath);
    }
    return true;
}

std::string AccountUidToString(const AccountUid& uid) {
    std::ostringstream stream;
    stream.setf(std::ios::hex, std::ios::basefield);
    stream.setf(std::ios::uppercase);
    stream.width(16);
    stream.fill('0');
    stream << uid.uid[0];
    stream.width(16);
    stream.fill('0');
    stream << uid.uid[1];
    return stream.str();
}

bool ParseAccountUid(const std::string& value, AccountUid& uid) {
    const std::string trimmed = Trim(value);
    if (trimmed.size() != 32) {
        return false;
    }
    uid = {};
    uid.uid[0] = std::strtoull(trimmed.substr(0, 16).c_str(), nullptr, 16);
    uid.uid[1] = std::strtoull(trimmed.substr(16, 16).c_str(), nullptr, 16);
    return accountUidIsValid(&uid);
}

bool TryGetActiveAccountUid(AccountUid& uid) {
    if (AccountUid* stored = envGetUserIdStorage(); stored != nullptr && accountUidIsValid(stored)) {
        uid = *stored;
        return true;
    }

    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) {
        return false;
    }

    bool found = false;
    if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid)) {
        found = true;
    } else if (R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid)) {
        found = true;
    } else if (R_SUCCEEDED(accountTrySelectUserWithoutInteraction(&uid, false)) && accountUidIsValid(&uid)) {
        found = true;
    } else {
        AccountUid users[ACC_USER_LIST_SIZE] = {};
        s32 actualTotal = 0;
        if (R_SUCCEEDED(accountListAllUsers(users, ACC_USER_LIST_SIZE, &actualTotal))) {
            for (s32 index = 0; index < actualTotal && index < ACC_USER_LIST_SIZE; ++index) {
                if (accountUidIsValid(&users[index])) {
                    uid = users[index];
                    found = true;
                    break;
                }
            }
        }
    }

    accountExit();
    return found;
}

bool TryFindExistingSaveDataInfo(std::uint64_t applicationId,
                                 const std::string& saveKind,
                                 FsSaveDataInfo& infoOut) {
    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User))) {
        return false;
    }

    const u8 desiredType = (saveKind == "device") ? static_cast<u8>(FsSaveDataType_Device)
                                                   : static_cast<u8>(FsSaveDataType_Account);
    bool found = false;
    FsSaveDataInfo entries[16];
    s64 readCount = 0;
    do {
        if (R_FAILED(fsSaveDataInfoReaderRead(&reader, entries, 16, &readCount))) {
            break;
        }
        for (s64 index = 0; index < readCount; ++index) {
            if (entries[index].application_id == applicationId && entries[index].save_data_type == desiredType) {
                infoOut = entries[index];
                found = true;
                break;
            }
        }
    } while (!found && readCount > 0);

    fsSaveDataInfoReaderClose(&reader);
    return found;
}

bool TryFindAnyAccountSaveUid(AccountUid& uidOut) {
    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User))) {
        return false;
    }

    bool found = false;
    FsSaveDataInfo entries[16];
    s64 readCount = 0;
    do {
        if (R_FAILED(fsSaveDataInfoReaderRead(&reader, entries, 16, &readCount))) {
            break;
        }
        for (s64 index = 0; index < readCount; ++index) {
            if (entries[index].save_data_type == static_cast<u8>(FsSaveDataType_Account) &&
                accountUidIsValid(&entries[index].uid)) {
                uidOut = entries[index].uid;
                found = true;
                break;
            }
        }
    } while (!found && readCount > 0);

    fsSaveDataInfoReaderClose(&reader);
    return found;
}

bool TryCreateMissingSaveData(std::uint64_t applicationId,
                              const SaveVariantRecord& variant,
                              const AccountUid& uid,
                              std::string& error) {
    FsSaveDataAttribute attr{};
    attr.application_id = applicationId;
    attr.uid = uid;
    attr.save_data_type =
        variant.saveKind == "device" ? static_cast<u8>(FsSaveDataType_Device) : static_cast<u8>(FsSaveDataType_Account);
    attr.save_data_rank = 0;
    attr.save_data_index = 0;

    FsSaveDataCreationInfo creation{};
    NsApplicationControlData controlData{};
    u64 actualSize = 0;
    const Result controlResult = nsGetApplicationControlData(NsApplicationControlSource_Storage,
                                                             applicationId,
                                                             &controlData,
                                                             sizeof(controlData),
                                                             &actualSize);
    if (R_SUCCEEDED(controlResult) && actualSize >= sizeof(controlData.nacp)) {
        if (variant.saveKind == "device") {
            creation.save_data_size = static_cast<s64>(controlData.nacp.device_save_data_size);
            creation.journal_size = static_cast<s64>(controlData.nacp.device_save_data_journal_size);
            if (creation.save_data_size <= 0) {
                creation.save_data_size = static_cast<s64>(controlData.nacp.device_save_data_size_max);
            }
            if (creation.journal_size <= 0) {
                creation.journal_size = static_cast<s64>(controlData.nacp.device_save_data_journal_size_max);
            }
        } else {
            creation.save_data_size = static_cast<s64>(controlData.nacp.user_account_save_data_size);
            creation.journal_size = static_cast<s64>(controlData.nacp.user_account_save_data_journal_size);
            if (creation.save_data_size <= 0) {
                creation.save_data_size = static_cast<s64>(controlData.nacp.user_account_save_data_size_max);
            }
            if (creation.journal_size <= 0) {
                creation.journal_size = static_cast<s64>(controlData.nacp.user_account_save_data_journal_size_max);
            }
        }
    }

    if (creation.save_data_size <= 0) {
        creation.save_data_size = 64LL * 1024LL * 1024LL;
    }
    if (creation.journal_size < 0) {
        creation.journal_size = 0;
    }
    if (creation.journal_size == 0) {
        creation.journal_size = 16LL * 1024LL * 1024LL;
    }
    creation.available_size = static_cast<u64>(creation.save_data_size + creation.journal_size);
    creation.owner_id = applicationId;
    creation.flags = 0;
    creation.save_data_space_id = static_cast<u8>(FsSaveDataSpaceId_User);
    creation.unk = 0;

    FsSaveDataMetaInfo meta{};
    meta.size = 0;
    meta.type = static_cast<u8>(FsSaveDataMetaType_None);

    Result rc = fsCreateSaveDataFileSystem(&attr, &creation, &meta);
    if (R_FAILED(rc)) {
        error = "Nao foi possivel criar o save do titulo para aplicar a variante.";
        return false;
    }
    return true;
}

std::string SaveBackupRootPath(const std::string& packageId) {
    return std::string(kCacheDir) + "/save-backups/" + packageId;
}

std::string SaveStageRootPath(const std::string& packageId) {
    return std::string(kCacheDir) + "/save-stage/" + packageId;
}

std::string SaveOpsRootPath() {
    return std::string(kSaveOpsDir);
}

std::string JsonEscape(const std::string& value) {
    std::ostringstream stream;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\"':
                stream << "\\\"";
                break;
            case '\\':
                stream << "\\\\";
                break;
            case '\b':
                stream << "\\b";
                break;
            case '\f':
                stream << "\\f";
                break;
            case '\n':
                stream << "\\n";
                break;
            case '\r':
                stream << "\\r";
                break;
            case '\t':
                stream << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    stream << "\\u";
                    stream.setf(std::ios::hex, std::ios::basefield);
                    stream.setf(std::ios::uppercase);
                    stream.width(4);
                    stream.fill('0');
                    stream << static_cast<int>(ch);
                    stream.setf(std::ios::dec, std::ios::basefield);
                } else {
                    stream << static_cast<char>(ch);
                }
                break;
        }
    }
    return stream.str();
}

std::string SaveOpPath(const std::string& packageId) {
    return JoinPath(SaveOpsRootPath(), packageId + ".json");
}

bool HasEdenSdLayoutMarker();

bool QueueEmulatorSaveInstallOperation(const CatalogEntry& entry,
                                       const SaveVariantRecord& variant,
                                       const InstalledTitle* installedTitle,
                                       const AccountUid& saveUid,
                                       const std::string& payloadRoot,
                                       const std::string& backupRoot,
                                       std::string& error) {
    std::string emulatorName;
    if (installedTitle != nullptr && !installedTitle->emulatorName.empty()) {
        emulatorName = ToLowerAscii(installedTitle->emulatorName);
    } else if (HasEdenSdLayoutMarker()) {
        emulatorName = "eden";
    } else if (PathExists("sdmc:/Nintendo/Contents") || PathExists("sdmc:/saveMeta") || PathExists("sdmc:/bis")) {
        emulatorName = "ryujinx";
    }

    if (emulatorName.empty()) {
        error = "Nao foi possivel enfileirar a operacao de save para o emulador.";
        return false;
    }

    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kConfigRootDir);
    EnsureDirectory(kCacheDir);
    EnsureDirectory(kSaveOpsDir);

    const std::string operationPath = SaveOpPath(entry.id);
    std::ofstream output(operationPath, std::ios::trunc);
    if (!output.good()) {
        error = "Nao foi possivel gravar a operacao pendente de save.";
        return false;
    }

    output << "{\n";
    output << "  \"schemaVersion\": \"1\",\n";
    output << "  \"action\": \"install_save\",\n";
    output << "  \"createdAt\": \"" << JsonEscape(CurrentTimestamp()) << "\",\n";
    output << "  \"emulatorName\": \"" << JsonEscape(emulatorName) << "\",\n";
    output << "  \"packageId\": \"" << JsonEscape(entry.id) << "\",\n";
    output << "  \"packageVersion\": \"" << JsonEscape(entry.packageVersion) << "\",\n";
    output << "  \"titleId\": \"" << JsonEscape(ToLowerAscii(entry.titleId)) << "\",\n";
    output << "  \"variantId\": \"" << JsonEscape(variant.id) << "\",\n";
    output << "  \"saveKind\": \"" << JsonEscape(variant.saveKind.empty() ? "account" : variant.saveKind) << "\",\n";
    output << "  \"saveUserId\": \"" << JsonEscape(accountUidIsValid(&saveUid) ? AccountUidToString(saveUid) : std::string()) << "\",\n";
    output << "  \"payloadRoot\": \"" << JsonEscape(payloadRoot) << "\",\n";
    output << "  \"backupRoot\": \"" << JsonEscape(backupRoot) << "\",\n";
    output << "  \"sourceUrl\": \"" << JsonEscape(variant.downloadUrl) << "\",\n";
    output << "  \"gameVersion\": \"" << JsonEscape(installedTitle ? installedTitle->displayVersion : std::string()) << "\"\n";
    output << "}\n";

    if (!output.good()) {
        error = "Nao foi possivel finalizar a operacao pendente de save.";
        return false;
    }

    return true;
}

std::string SaveMountName() {
    return "milmgrsave";
}

std::string SaveMountRoot() {
    return SaveMountName() + ":/";
}

std::string BisUserMountName() {
    return "milmgrusr";
}

std::string BisUserMountRoot() {
    return BisUserMountName() + ":/";
}

std::string UpperHex64(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(value));
    return buffer;
}

bool MountBisUserFileSystem(std::string& error) {
    const std::string mountName = BisUserMountName();
    fsdevUnmountDevice(mountName.c_str());

    FsFileSystem bisFs{};
    Result rc = fsOpenBisFileSystem(&bisFs, FsBisPartitionId_User, "");
    if (R_FAILED(rc)) {
        error = "Nao foi possivel abrir a particao de usuario para o fallback do emulador.";
        return false;
    }

    if (fsdevMountDevice(mountName.c_str(), bisFs) != 0) {
        error = "Nao foi possivel montar a particao de usuario para o fallback do emulador.";
        return false;
    }

    return true;
}

void UnmountBisUserFileSystem() {
    fsdevUnmountDevice(BisUserMountName().c_str());
}

bool LooksLikeRyujinxSaveRoot(const std::string& path) {
    return PathExists(JoinPath(path, ".lock")) ||
           PathExists(JoinPath(path, "ExtraData0")) ||
           PathExists(JoinPath(path, "ExtraData1")) ||
           IsDirectoryPath(JoinPath(path, "0")) ||
           IsDirectoryPath(JoinPath(path, "1"));
}

bool HasEdenSdLayoutMarker() {
    return IsDirectoryPath("sdmc:/.get") || IsDirectoryPath("sdmc:/.get/packages") || IsDirectoryPath("sdmc:/.get/tmp");
}

std::string BuildEdenLegacySaveRoot(const AccountUid& uid, std::uint64_t applicationId) {
    return JoinPath(JoinPath(JoinPath(JoinPath(BisUserMountRoot(), "save"), "0000000000000000"),
                             AccountUidToString(uid)),
                    UpperHex64(applicationId));
}

bool ResolveBisFallbackTargetRoot(const SaveVariantRecord& variant,
                                  std::uint64_t applicationId,
                                  const std::string& savedUid,
                                  AccountUid& resolvedUid,
                                  std::string& targetRoot,
                                  bool& preserveRootMetadata,
                                  std::string& error) {
    preserveRootMetadata = false;
    targetRoot.clear();
    const bool hasEdenLayout = HasEdenSdLayoutMarker();

    if (!MountBisUserFileSystem(error)) {
        return false;
    }

    const bool wantsAccount = variant.saveKind.empty() || variant.saveKind == "account";
    FsSaveDataInfo saveInfo{};
    const bool hadExistingInfo = TryFindExistingSaveDataInfo(applicationId, wantsAccount ? "account" : "device", saveInfo);

    if (wantsAccount) {
        if (!savedUid.empty() && ParseAccountUid(savedUid, resolvedUid)) {
            // Receipt-provided uid.
        } else if (hadExistingInfo && accountUidIsValid(&saveInfo.uid)) {
            resolvedUid = saveInfo.uid;
        } else if (TryFindAnyAccountSaveUid(resolvedUid)) {
            // Reuse first known uid from the environment.
        } else if (!TryGetActiveAccountUid(resolvedUid)) {
            error = "Nao foi possivel resolver um usuario para acessar o save.";
            UnmountBisUserFileSystem();
            return false;
        }
    } else {
        resolvedUid = {};
    }

    FsSaveDataInfo resolvedInfo = saveInfo;
    bool hasInfo = hadExistingInfo;
    if (!hasInfo && !hasEdenLayout) {
        const AccountUid createUid = wantsAccount ? resolvedUid : AccountUid{};
        std::string createError;
        if (TryCreateMissingSaveData(applicationId, variant, createUid, createError)) {
            hasInfo = TryFindExistingSaveDataInfo(applicationId, wantsAccount ? "account" : "device", resolvedInfo);
        }
    }

    if (hasInfo) {
        const std::string ryujinxRoot =
            JoinPath(JoinPath(BisUserMountRoot(), "save"), UpperHex64(resolvedInfo.save_data_id));
        if (LooksLikeRyujinxSaveRoot(ryujinxRoot)) {
            targetRoot = ryujinxRoot;
            preserveRootMetadata = true;
            return true;
        }
    }

    if (wantsAccount && accountUidIsValid(&resolvedUid) && hasEdenLayout) {
        targetRoot = BuildEdenLegacySaveRoot(resolvedUid, applicationId);
        preserveRootMetadata = false;
        return true;
    }

    UnmountBisUserFileSystem();
    error = "Nao foi possivel localizar um diretorio de save compativel no emulador.";
    return false;
}

bool CreateEmptyBackupMarker(const std::string& backupRoot);

bool BackupDirectoryForSave(const std::string& sourceRoot,
                           const std::string& backupRoot,
                           std::string& error,
                           HttpProgressCallback progressCallback,
                           void* progressUserData) {
    std::string cleanupError;
    RemovePathRecursively(backupRoot, cleanupError);
    EnsureDirectory(backupRoot);

    if (!PathExists(sourceRoot)) {
        return CreateEmptyBackupMarker(backupRoot);
    }

    std::uint64_t progressBytes = 0;
    const std::uint64_t backupBytesTotal = std::max<std::uint64_t>(1, DirectorySizeBytes(sourceRoot));
    std::vector<std::string> backupFiles;
    if (!CopyDirectoryTree(sourceRoot,
                           backupRoot,
                           &backupFiles,
                           error,
                           progressCallback,
                           progressUserData,
                           progressBytes,
                           backupBytesTotal)) {
        return false;
    }
    if (backupFiles.empty()) {
        return CreateEmptyBackupMarker(backupRoot);
    }
    return true;
}

bool ApplyPayloadToEmulatorSaveRoot(const std::string& targetRoot,
                                    bool preserveRootMetadata,
                                    const std::string& payloadRoot,
                                    std::vector<std::string>& appliedFiles,
                                    std::string& error,
                                    HttpProgressCallback progressCallback,
                                    void* progressUserData) {
    EnsureDirectory(targetRoot);

    if (preserveRootMetadata) {
        const std::string slot0 = JoinPath(targetRoot, "0");
        const std::string slot1 = JoinPath(targetRoot, "1");
        EnsureDirectory(slot0);
        EnsureDirectory(slot1);
        if (!ClearDirectoryContents(slot0, error) || !ClearDirectoryContents(slot1, error)) {
            return false;
        }

        std::uint64_t progressBytes = 0;
        const std::uint64_t applyBytesTotal = std::max<std::uint64_t>(1, DirectorySizeBytes(payloadRoot)) * 2;
        if (!CopyDirectoryTree(payloadRoot,
                               slot0,
                               &appliedFiles,
                               error,
                               progressCallback,
                               progressUserData,
                               progressBytes,
                               applyBytesTotal)) {
            return false;
        }
        if (!CopyDirectoryTree(payloadRoot,
                               slot1,
                               &appliedFiles,
                               error,
                               progressCallback,
                               progressUserData,
                               progressBytes,
                               applyBytesTotal)) {
            return false;
        }
        return true;
    }

    if (PathExists(targetRoot) && !ClearDirectoryContents(targetRoot, error)) {
        return false;
    }

    std::uint64_t progressBytes = 0;
    const std::uint64_t applyBytesTotal = std::max<std::uint64_t>(1, DirectorySizeBytes(payloadRoot));
    return CopyDirectoryTree(payloadRoot,
                             targetRoot,
                             &appliedFiles,
                             error,
                             progressCallback,
                             progressUserData,
                             progressBytes,
                             applyBytesTotal);
}

bool RestoreEmulatorSaveRoot(const std::string& targetRoot,
                             const std::string& backupRoot,
                             std::string& error,
                             HttpProgressCallback progressCallback,
                             void* progressUserData) {
    std::string cleanupError;
    RemovePathRecursively(targetRoot, cleanupError);
    EnsureDirectory(targetRoot);

    if (!backupRoot.empty() && PathExists(backupRoot)) {
        std::uint64_t restoreBytes = 0;
        const std::uint64_t restoreTotal = std::max<std::uint64_t>(1, DirectorySizeBytes(backupRoot));
        for (const std::string& name : ListDirectoryEntries(backupRoot)) {
            if (name == ".mil-empty") {
                continue;
            }
            if (!CopyDirectoryTree(JoinPath(backupRoot, name),
                                   JoinPath(targetRoot, name),
                                   nullptr,
                                   error,
                                   progressCallback,
                                   progressUserData,
                                   restoreBytes,
                                   restoreTotal)) {
                return false;
            }
        }
    }

    fsdevCommitDevice(BisUserMountName().c_str());
    return true;
}

bool CreateEmptyBackupMarker(const std::string& backupRoot) {
    EnsureDirectory(backupRoot);
    std::ofstream output(JoinPath(backupRoot, ".mil-empty"), std::ios::trunc);
    return output.good();
}

std::string JoinFsPath(const std::string& base, const std::string& child) {
    if (base.empty() || base == "/") {
        return "/" + child;
    }
    if (base.back() == '/') {
        return base + child;
    }
    return base + "/" + child;
}

std::string NormalizeFsPath(const std::string& path) {
    if (path.empty() || path == "/") {
        return {};
    }
    if (!path.empty() && path.front() == '/') {
        return path.substr(1);
    }
    return path;
}

bool FsPathExists(FsFileSystem* fs, const std::string& path, FsDirEntryType* typeOut = nullptr) {
    FsDirEntryType type{};
    Result rc = fsFsGetEntryType(fs, path.c_str(), &type);
    if (R_FAILED(rc)) {
        const std::string normalized = NormalizeFsPath(path);
        if (normalized == path) {
            return false;
        }
        rc = fsFsGetEntryType(fs, normalized.c_str(), &type);
        if (R_FAILED(rc)) {
            return false;
        }
    }
    if (typeOut != nullptr) {
        *typeOut = type;
    }
    return true;
}

bool EnsureFsDirectory(FsFileSystem* fs, const std::string& path, std::string& error) {
    if (path.empty() || path == "/") {
        return true;
    }

    std::string current;
    for (char ch : path) {
        current.push_back(ch);
        if (ch == '/') {
            continue;
        }
        const bool isLast = current.size() == path.size();
        const bool nextIsSeparator = !isLast && path[current.size()] == '/';
        if (!isLast && !nextIsSeparator) {
            continue;
        }

        FsDirEntryType entryType{};
        if (FsPathExists(fs, current, &entryType)) {
            continue;
        }
        Result rc = fsFsCreateDirectory(fs, current.c_str());
        if (R_FAILED(rc)) {
            const std::string normalized = NormalizeFsPath(current);
            if (normalized != current) {
                rc = fsFsCreateDirectory(fs, normalized.c_str());
            }
        }
        if (R_FAILED(rc) && !FsPathExists(fs, current, nullptr)) {
            error = "Nao foi possivel criar diretorio no save.";
            return false;
        }
    }
    return true;
}

std::vector<FsDirectoryEntry> ListFsDirectoryEntries(FsFileSystem* fs, const std::string& path) {
    std::vector<FsDirectoryEntry> entries;
    FsDir dir{};
    Result rc = fsFsOpenDirectory(fs,
                                  path.c_str(),
                                  FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles,
                                  &dir);
    if (R_FAILED(rc)) {
        const std::string normalized = NormalizeFsPath(path);
        if (normalized == path ||
            R_FAILED(fsFsOpenDirectory(fs,
                                       normalized.c_str(),
                                       FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles,
                                       &dir))) {
            return entries;
        }
    }

    while (true) {
        FsDirectoryEntry chunk[16]{};
        s64 readCount = 0;
        if (R_FAILED(fsDirRead(&dir, &readCount, 16, chunk)) || readCount <= 0) {
            break;
        }
        for (s64 i = 0; i < readCount; ++i) {
            entries.push_back(chunk[i]);
        }
    }

    fsDirClose(&dir);
    return entries;
}

bool ClearFsDirectoryContents(FsFileSystem* fs, const std::string& path, std::string& error) {
    for (const auto& entry : ListFsDirectoryEntries(fs, path)) {
        const std::string child = JoinFsPath(path, entry.name);
        Result rc = 0;
        if (entry.type == FsDirEntryType_Dir) {
            rc = fsFsDeleteDirectoryRecursively(fs, child.c_str());
        } else {
            rc = fsFsDeleteFile(fs, child.c_str());
        }
        if (R_FAILED(rc) && FsPathExists(fs, child, nullptr)) {
            error = "Nao foi possivel limpar o save montado.";
            return false;
        }
    }
    return true;
}

bool CopyFsFileToLocal(FsFileSystem* fs,
                       const std::string& fsPath,
                       const std::string& localPath,
                       std::string& error,
                       HttpProgressCallback progressCallback,
                       void* progressUserData,
                       std::uint64_t& completedBytes,
                       std::uint64_t totalBytes) {
    FsFile file{};
    Result rc = fsFsOpenFile(fs, fsPath.c_str(), FsOpenMode_Read, &file);
    if (R_FAILED(rc)) {
        const std::string normalized = NormalizeFsPath(fsPath);
        if (normalized == fsPath || R_FAILED(fsFsOpenFile(fs, normalized.c_str(), FsOpenMode_Read, &file))) {
            error = "Nao foi possivel abrir arquivo do save.";
            return false;
        }
    }

    s64 size = 0;
    if (R_FAILED(fsFileGetSize(&file, &size))) {
        fsFileClose(&file);
        error = "Nao foi possivel ler o tamanho do arquivo do save.";
        return false;
    }

    EnsureDirectory(ParentDirectory(localPath));
    std::ofstream output(localPath, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        fsFileClose(&file);
        error = "Nao foi possivel criar backup local do save.";
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    s64 offset = 0;
    while (offset < size) {
        const u64 chunkSize = static_cast<u64>(std::min<s64>(static_cast<s64>(buffer.size()), size - offset));
        u64 bytesRead = 0;
        if (R_FAILED(fsFileRead(&file, offset, buffer.data(), chunkSize, 0, &bytesRead)) || bytesRead == 0) {
            fsFileClose(&file);
            error = "Falha ao ler arquivo do save.";
            return false;
        }
        output.write(buffer.data(), static_cast<std::streamsize>(bytesRead));
        if (!output.good()) {
            fsFileClose(&file);
            error = "Falha ao gravar backup local do save.";
            return false;
        }
        offset += static_cast<s64>(bytesRead);
        completedBytes += bytesRead;
        if (progressCallback != nullptr) {
            progressCallback(completedBytes, totalBytes, true, progressUserData);
        }
    }

    fsFileClose(&file);
    return true;
}

bool CopyFsTreeToLocal(FsFileSystem* fs,
                       const std::string& fsPath,
                       const std::string& localPath,
                       std::string& error,
                       HttpProgressCallback progressCallback,
                       void* progressUserData,
                       std::uint64_t& completedBytes,
                       std::uint64_t totalBytes) {
    FsDirEntryType entryType{};
    if (!FsPathExists(fs, fsPath, &entryType)) {
        return true;
    }

    if (entryType == FsDirEntryType_Dir) {
        EnsureDirectory(localPath);
        for (const auto& entry : ListFsDirectoryEntries(fs, fsPath)) {
            if (!CopyFsTreeToLocal(fs,
                                   JoinFsPath(fsPath, entry.name),
                                   JoinPath(localPath, entry.name),
                                   error,
                                   progressCallback,
                                   progressUserData,
                                   completedBytes,
                                   totalBytes)) {
                return false;
            }
        }
        return true;
    }

    return CopyFsFileToLocal(fs, fsPath, localPath, error, progressCallback, progressUserData, completedBytes, totalBytes);
}

bool BackupSaveFileSystem(FsFileSystem* fs,
                          const std::string& backupRoot,
                          std::string& error,
                          HttpProgressCallback progressCallback,
                          void* progressUserData) {
    std::string cleanupError;
    RemovePathRecursively(backupRoot, cleanupError);
    EnsureDirectory(backupRoot);

    std::vector<FsDirectoryEntry> rootEntries = ListFsDirectoryEntries(fs, "/");
    std::string effectiveFsRoot = "/";
    if (rootEntries.size() == 1 && rootEntries.front().type == FsDirEntryType_Dir) {
        effectiveFsRoot = JoinFsPath("/", rootEntries.front().name);
        rootEntries = ListFsDirectoryEntries(fs, effectiveFsRoot);
    }

    std::uint64_t totalBytes = 1;
    for (const auto& entry : rootEntries) {
        if (entry.type != FsDirEntryType_Dir && entry.file_size > 0) {
            totalBytes += static_cast<std::uint64_t>(entry.file_size);
        }
    }

    std::uint64_t completedBytes = 0;
    bool copiedAnything = false;
    for (const auto& entry : rootEntries) {
        copiedAnything = true;
        if (!CopyFsTreeToLocal(fs,
                               JoinFsPath(effectiveFsRoot, entry.name),
                               JoinPath(backupRoot, entry.name),
                               error,
                               progressCallback,
                               progressUserData,
                               completedBytes,
                               totalBytes)) {
            return false;
        }
    }

    if (!copiedAnything) {
        return CreateEmptyBackupMarker(backupRoot);
    }
    return true;
}

bool CopyLocalFileToFs(const std::string& localPath,
                       FsFileSystem* fs,
                       const std::string& fsPath,
                       std::string& error,
                       HttpProgressCallback progressCallback,
                       void* progressUserData,
                       std::uint64_t& completedBytes,
                       std::uint64_t totalBytes) {
    std::ifstream input(localPath, std::ios::binary);
    if (!input.good()) {
        error = "Nao foi possivel abrir arquivo local do save.";
        return false;
    }

    std::uint64_t localSize = 0;
    TryGetFileSize(localPath, localSize);
    if (!EnsureFsDirectory(fs, ParentDirectory(fsPath), error)) {
        return false;
    }

    fsFsDeleteFile(fs, fsPath.c_str());
    const std::string normalized = NormalizeFsPath(fsPath);
    if (normalized != fsPath) {
        fsFsDeleteFile(fs, normalized.c_str());
    }
    FsFile file{};
    bool openedExistingFile = false;
    Result rc = fsFsCreateFile(fs, fsPath.c_str(), static_cast<s64>(localSize), 0);
    if (R_FAILED(rc) && normalized != fsPath) {
        rc = fsFsCreateFile(fs, normalized.c_str(), static_cast<s64>(localSize), 0);
    }
    if (R_FAILED(rc)) {
        rc = fsFsOpenFile(fs, fsPath.c_str(), FsOpenMode_Write, &file);
        if (R_FAILED(rc) && normalized != fsPath) {
            rc = fsFsOpenFile(fs, normalized.c_str(), FsOpenMode_Write, &file);
        }
        if (R_SUCCEEDED(rc)) {
            openedExistingFile = true;
        }
    } else {
        rc = fsFsOpenFile(fs, fsPath.c_str(), FsOpenMode_Write, &file);
        if (R_FAILED(rc) && normalized != fsPath) {
            rc = fsFsOpenFile(fs, normalized.c_str(), FsOpenMode_Write, &file);
        }
    }
    if (R_FAILED(rc)) {
        error = "Nao foi possivel criar arquivo no save.";
        return false;
    }

    if (openedExistingFile && R_FAILED(fsFileSetSize(&file, static_cast<s64>(localSize)))) {
        fsFileClose(&file);
        error = "Nao foi possivel redimensionar arquivo no save.";
        return false;
    }

    std::vector<char> buffer(64 * 1024);
    s64 offset = 0;
    while (input.good()) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize chunk = input.gcount();
        if (chunk <= 0) {
            break;
        }
        if (R_FAILED(fsFileWrite(&file,
                                 offset,
                                 buffer.data(),
                                 static_cast<u64>(chunk),
                                 FsWriteOption_None))) {
            fsFileClose(&file);
            error = "Falha ao gravar arquivo no save.";
            return false;
        }
        offset += chunk;
        completedBytes += static_cast<std::uint64_t>(chunk);
        if (progressCallback != nullptr) {
            progressCallback(completedBytes, totalBytes, true, progressUserData);
        }
    }

    fsFileFlush(&file);
    fsFileClose(&file);
    return true;
}

bool CopyLocalTreeToFs(const std::string& localPath,
                       FsFileSystem* fs,
                       const std::string& fsPath,
                       std::vector<std::string>* appliedFiles,
                       std::string& error,
                       HttpProgressCallback progressCallback,
                       void* progressUserData,
                       std::uint64_t& completedBytes,
                       std::uint64_t totalBytes) {
    struct stat fileStat = {};
    if (stat(localPath.c_str(), &fileStat) != 0) {
        error = "Nao foi possivel acessar o payload local do save.";
        return false;
    }

    if (S_ISDIR(fileStat.st_mode)) {
        if (!EnsureFsDirectory(fs, fsPath, error)) {
            return false;
        }
        for (const std::string& name : ListDirectoryEntries(localPath)) {
            if (!CopyLocalTreeToFs(JoinPath(localPath, name),
                                   fs,
                                   JoinFsPath(fsPath, name),
                                   appliedFiles,
                                   error,
                                   progressCallback,
                                   progressUserData,
                                   completedBytes,
                                   totalBytes)) {
                return false;
            }
        }
        return true;
    }

    if (!CopyLocalFileToFs(localPath, fs, fsPath, error, progressCallback, progressUserData, completedBytes, totalBytes)) {
        return false;
    }
    if (appliedFiles != nullptr) {
        appliedFiles->push_back(fsPath);
    }
    return true;
}

bool ApplyLocalPayloadToSaveFileSystem(FsFileSystem* fs,
                                       const std::string& payloadRoot,
                                       std::vector<std::string>& appliedFiles,
                                       std::string& error,
                                       HttpProgressCallback progressCallback,
                                       void* progressUserData) {
    std::vector<FsDirectoryEntry> rootEntries = ListFsDirectoryEntries(fs, "/");
    std::string effectiveFsRoot = "/";
    if (rootEntries.size() == 1 && rootEntries.front().type == FsDirEntryType_Dir) {
        effectiveFsRoot = JoinFsPath("/", rootEntries.front().name);
    }

    if (!ClearFsDirectoryContents(fs, effectiveFsRoot, error)) {
        return false;
    }
    std::uint64_t progressBytes = 0;
    const std::uint64_t totalBytes = std::max<std::uint64_t>(1, DirectorySizeBytes(payloadRoot));
    for (const std::string& name : ListDirectoryEntries(payloadRoot)) {
        if (!CopyLocalTreeToFs(JoinPath(payloadRoot, name),
                               fs,
                               JoinFsPath(effectiveFsRoot, name),
                               &appliedFiles,
                               error,
                               progressCallback,
                               progressUserData,
                               progressBytes,
                               totalBytes)) {
            return false;
        }
    }
    return true;
}

bool RestoreLocalBackupToSaveFileSystem(FsFileSystem* fs,
                                        const std::string& backupRoot,
                                        std::string& error,
                                        HttpProgressCallback progressCallback,
                                        void* progressUserData) {
    std::vector<FsDirectoryEntry> rootEntries = ListFsDirectoryEntries(fs, "/");
    std::string effectiveFsRoot = "/";
    if (rootEntries.size() == 1 && rootEntries.front().type == FsDirEntryType_Dir) {
        effectiveFsRoot = JoinFsPath("/", rootEntries.front().name);
    }

    if (!ClearFsDirectoryContents(fs, effectiveFsRoot, error)) {
        return false;
    }
    if (backupRoot.empty() || !PathExists(backupRoot)) {
        return true;
    }

    std::string backupContentRoot = backupRoot;
    const auto backupEntries = ListDirectoryEntries(backupRoot);
    if (effectiveFsRoot != "/" && backupEntries.size() == 1) {
        const std::string candidate = JoinPath(backupRoot, backupEntries.front());
        if (IsDirectoryPath(candidate)) {
            backupContentRoot = candidate;
        }
    }

    std::uint64_t progressBytes = 0;
    const std::uint64_t totalBytes = std::max<std::uint64_t>(1, DirectorySizeBytes(backupContentRoot));
    for (const std::string& name : ListDirectoryEntries(backupContentRoot)) {
        if (name == ".mil-empty") {
            continue;
        }
        if (!CopyLocalTreeToFs(JoinPath(backupContentRoot, name),
                               fs,
                               JoinFsPath(effectiveFsRoot, name),
                               nullptr,
                               error,
                               progressCallback,
                               progressUserData,
                               progressBytes,
                               totalBytes)) {
            return false;
        }
    }
    return true;
}

bool ResolveSaveVariantUser(const SaveVariantRecord& variant,
                            std::uint64_t applicationId,
                            const std::string& savedUid,
                            AccountUid& resolvedUid,
                            std::string& error) {
    if (variant.saveKind == "account" || variant.saveKind.empty()) {
        FsSaveDataInfo saveInfo{};
        if (!savedUid.empty() && ParseAccountUid(savedUid, resolvedUid)) {
            // Use receipt/user-specific uid.
        } else if (TryFindExistingSaveDataInfo(applicationId, "account", saveInfo) &&
                   accountUidIsValid(&saveInfo.uid)) {
            resolvedUid = saveInfo.uid;
        } else if (TryFindAnyAccountSaveUid(resolvedUid)) {
            // Reuse emulator/console default account uid.
        } else if (!TryGetActiveAccountUid(resolvedUid)) {
            error = "Nao foi possivel resolver um usuario para acessar o save.";
            return false;
        }
        return true;
    }

    resolvedUid = {};
    return true;
}

bool OpenSaveDataFileSystemForVariant(const SaveVariantRecord& variant,
                                      std::uint64_t applicationId,
                                      const std::string& savedUid,
                                      AccountUid& resolvedUid,
                                      FsFileSystem& saveFs,
                                      std::string& error) {
    const bool hasEdenLayout = HasEdenSdLayoutMarker();

    if (!ResolveSaveVariantUser(variant, applicationId, savedUid, resolvedUid, error)) {
        return false;
    }

    if (variant.saveKind == "account" || variant.saveKind.empty()) {

        Result rc = fsOpen_SaveData(&saveFs, applicationId, resolvedUid);
        if (R_FAILED(rc)) {
            std::string createError;
            if (TryCreateMissingSaveData(applicationId, variant, resolvedUid, createError)) {
                rc = fsOpen_SaveData(&saveFs, applicationId, resolvedUid);
            }
        }
        if (R_FAILED(rc)) {
            error = hasEdenLayout
                        ? "Nao foi possivel abrir o save do titulo neste emulador. Inicie o jogo uma vez para criar o save e tente novamente."
                        : "Nao foi possivel montar o save de conta do titulo.";
            return false;
        }
        return true;
    }

    if (variant.saveKind == "device") {
        FsSaveDataInfo saveInfo{};
        if (TryFindExistingSaveDataInfo(applicationId, "device", saveInfo)) {
            // Existing device save found; proceed with device mount below.
        }
        Result rc = fsOpen_DeviceSaveData(&saveFs, applicationId);
        if (R_FAILED(rc)) {
            AccountUid dummyUid{};
            std::string createError;
            if (TryCreateMissingSaveData(applicationId, variant, dummyUid, createError)) {
                rc = fsOpen_DeviceSaveData(&saveFs, applicationId);
            }
        }
        if (R_FAILED(rc)) {
            error = hasEdenLayout
                        ? "Nao foi possivel abrir o save do titulo neste emulador. Inicie o jogo uma vez para criar o save e tente novamente."
                        : "Nao foi possivel montar o save do dispositivo do titulo.";
            return false;
        }
        resolvedUid = {};
        return true;
    }

    error = "Tipo de save nao suportado: " + variant.saveKind;
    return false;
}

bool MountSaveDataForVariant(const SaveVariantRecord& variant,
                             std::uint64_t applicationId,
                             const std::string& savedUid,
                             AccountUid& resolvedUid,
                             std::string& error) {
    const std::string mountName = SaveMountName();
    fsdevUnmountDevice(mountName.c_str());

    if (!ResolveSaveVariantUser(variant, applicationId, savedUid, resolvedUid, error)) {
        return false;
    }

    Result rc = 0;
    if (variant.saveKind == "device") {
        rc = fsdevMountDeviceSaveData(mountName.c_str(), applicationId);
    } else {
        rc = fsdevMountSaveData(mountName.c_str(), applicationId, resolvedUid);
    }
    if (R_FAILED(rc)) {
        error = "Nao foi possivel montar o save do titulo.";
        return false;
    }
    return true;
}

void UnmountSaveData() {
    fsdevUnmountDevice(SaveMountName().c_str());
}

std::string DetectJksvPayloadRoot(const std::string& extractedRoot) {
    const std::string jksvRoot = JoinPath(extractedRoot, "JKSV");
    const auto gameFolders = ListDirectoryEntries(jksvRoot);
    if (gameFolders.empty()) {
        return {};
    }
    const std::string firstGameRoot = JoinPath(jksvRoot, gameFolders.front());
    const auto variantFolders = ListDirectoryEntries(firstGameRoot);
    if (variantFolders.empty()) {
        return {};
    }
    return JoinPath(firstGameRoot, variantFolders.front());
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
    output << "install_type=" << receipt.installType << '\n';
    output << "backup_path=" << receipt.backupPath << '\n';
    output << "save_kind=" << receipt.saveKind << '\n';
    output << "save_user_id=" << receipt.saveUserId << '\n';
    output << "variant_id=" << receipt.variantId << '\n';
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
        } else if (key == "install_type") {
            receipt.installType = value;
        } else if (key == "backup_path") {
            receipt.backupPath = value;
        } else if (key == "save_kind") {
            receipt.saveKind = value;
        } else if (key == "save_user_id") {
            receipt.saveUserId = value;
        } else if (key == "variant_id") {
            receipt.variantId = value;
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

bool InstallPackage(const CatalogEntry& entry,
                    const InstalledTitle* installedTitle,
                    InstallReceipt& receipt,
                    std::string& error,
                    HttpProgressCallback progressCallback,
                    void* progressUserData) {
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
    HttpDownloadOptions downloadOptions;
    downloadOptions.progressCallback = progressCallback;
    downloadOptions.progressUserData = progressUserData;
    if (!HttpDownloadToFileWithOptions(entry.downloadUrl, zipPath, downloadOptions, &bytesDownloaded, error)) {
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
                      std::string& error,
                      HttpProgressCallback progressCallback,
                      void* progressUserData) {
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
    options.progressCallback = progressCallback;
    options.progressUserData = progressUserData;
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

bool InstallSaveData(const CatalogEntry& entry,
                     const SaveVariantRecord& variant,
                     const InstalledTitle* installedTitle,
                     InstallReceipt& receipt,
                     std::string& error,
                     HttpProgressCallback progressCallback,
                     void* progressUserData) {
    EnsureDirectory("sdmc:/switch");
    EnsureDirectory(kConfigRootDir);
    EnsureDirectory(kCacheDir);

    if (entry.titleId.empty()) {
        error = "Title ID ausente para instalacao do save.";
        return false;
    }

    const std::uint64_t applicationId = std::strtoull(entry.titleId.c_str(), nullptr, 16);
    if (applicationId == 0) {
        error = "Title ID invalido para instalacao do save.";
        return false;
    }

    const std::string zipPath = std::string(kCacheDir) + "/" + entry.id + "-" + variant.id + ".zip";
    std::size_t bytesDownloaded = 0;
    HttpDownloadOptions downloadOptions;
    downloadOptions.progressCallback = progressCallback;
    downloadOptions.progressUserData = progressUserData;
    downloadOptions.requestTimeoutMs = 60000;
    if (!HttpDownloadToFileWithOptions(variant.downloadUrl, zipPath, downloadOptions, &bytesDownloaded, error)) {
        return false;
    }
    if (bytesDownloaded == 0) {
        remove(zipPath.c_str());
        error = "Download do save concluido, mas o arquivo estava vazio.";
        return false;
    }

    const std::string stageRoot = SaveStageRootPath(entry.id);
    std::string cleanupError;
    RemovePathRecursively(stageRoot, cleanupError);
    EnsureDirectory(stageRoot);

    std::vector<std::string> stagedFiles;
    if (!ExtractZipToDirectory(zipPath, stageRoot, &stagedFiles, error)) {
        remove(zipPath.c_str());
        RemovePathRecursively(stageRoot, cleanupError);
        return false;
    }
    remove(zipPath.c_str());

    std::string payloadRoot;
    if (variant.layoutType == "jksv-backup") {
        payloadRoot = DetectJksvPayloadRoot(stageRoot);
    }
    if (payloadRoot.empty()) {
        payloadRoot = stageRoot;
    }
    if (!PathExists(payloadRoot)) {
        RemovePathRecursively(stageRoot, cleanupError);
        error = "Nao foi possivel localizar o conteudo do save no pacote.";
        return false;
    }

    const std::string backupRoot = SaveBackupRootPath(entry.id);
    AccountUid saveUid = {};
    FsFileSystem directSaveFs{};
    bool directSaveFsOpen = false;
    bool mountedSaveFs = false;
    std::vector<std::string> appliedFiles;
    std::string installRoot;
    Result commitRc = 0;

    std::string mountError;
    if (MountSaveDataForVariant(variant, applicationId, std::string(), saveUid, mountError)) {
        mountedSaveFs = true;
    } else if (OpenSaveDataFileSystemForVariant(variant, applicationId, std::string(), saveUid, directSaveFs, error)) {
        directSaveFsOpen = true;
        if (fsdevMountDevice(SaveMountName().c_str(), directSaveFs) == 0) {
            directSaveFsOpen = false;
            mountedSaveFs = true;
        }
    }

    if (mountedSaveFs) {
        const std::string mountRoot = SaveMountRoot();
        installRoot = mountRoot;

        if (!BackupDirectoryForSave(mountRoot, backupRoot, error, progressCallback, progressUserData)) {
            UnmountSaveData();
            RemovePathRecursively(stageRoot, cleanupError);
            RemovePathRecursively(backupRoot, cleanupError);
            return false;
        }

        if (!ClearDirectoryContents(mountRoot, error)) {
            UnmountSaveData();
            RemovePathRecursively(stageRoot, cleanupError);
            return false;
        }

        std::uint64_t progressBytes = 0;
        const std::uint64_t applyBytesTotal = std::max<std::uint64_t>(1, DirectorySizeBytes(payloadRoot));
        if (!CopyDirectoryTree(payloadRoot,
                               mountRoot,
                               &appliedFiles,
                               error,
                               progressCallback,
                               progressUserData,
                               progressBytes,
                               applyBytesTotal)) {
            std::string restoreError;
            ClearDirectoryContents(mountRoot, restoreError);
            std::uint64_t restoreBytes = 0;
            for (const std::string& name : ListDirectoryEntries(backupRoot)) {
                if (name == ".mil-empty") {
                    continue;
                }
                CopyDirectoryTree(JoinPath(backupRoot, name),
                                  JoinPath(mountRoot, name),
                                  nullptr,
                                  restoreError,
                                  nullptr,
                                  nullptr,
                                  restoreBytes,
                                  1);
            }
            fsdevCommitDevice(SaveMountName().c_str());
            UnmountSaveData();
            RemovePathRecursively(stageRoot, cleanupError);
            RemovePathRecursively(backupRoot, cleanupError);
            return false;
        }

        commitRc = fsdevCommitDevice(SaveMountName().c_str());
        UnmountSaveData();
    } else if (directSaveFsOpen) {
        installRoot = "savefs:/";

        if (!BackupSaveFileSystem(&directSaveFs, backupRoot, error, progressCallback, progressUserData)) {
            fsFsClose(&directSaveFs);
            RemovePathRecursively(stageRoot, cleanupError);
            RemovePathRecursively(backupRoot, cleanupError);
            return false;
        }

        if (!ApplyLocalPayloadToSaveFileSystem(&directSaveFs,
                                               payloadRoot,
                                               appliedFiles,
                                               error,
                                               progressCallback,
                                               progressUserData)) {
            fsFsClose(&directSaveFs);
            RemovePathRecursively(stageRoot, cleanupError);
            return false;
        }

        commitRc = fsFsCommit(&directSaveFs);
        if (R_FAILED(commitRc)) {
            commitRc = 0;
        }
        fsFsClose(&directSaveFs);
    } else {
        std::string fallbackError = error;
        if (HasEdenSdLayoutMarker()) {
            RemovePathRecursively(stageRoot, cleanupError);
            error = fallbackError.empty()
                        ? "Nao foi possivel abrir o save do titulo neste emulador. Inicie o jogo uma vez para criar o save e tente novamente."
                        : fallbackError;
            return false;
        }
        bool preserveRootMetadata = false;
        if (!ResolveBisFallbackTargetRoot(variant,
                                          applicationId,
                                          std::string(),
                                          saveUid,
                                          installRoot,
                                          preserveRootMetadata,
                                          fallbackError)) {
            RemovePathRecursively(stageRoot, cleanupError);
            error = fallbackError;
            return false;
        }

        if (!BackupDirectoryForSave(installRoot, backupRoot, fallbackError, progressCallback, progressUserData)) {
            UnmountBisUserFileSystem();
            RemovePathRecursively(stageRoot, cleanupError);
            RemovePathRecursively(backupRoot, cleanupError);
            error = fallbackError;
            return false;
        }

        if (!ApplyPayloadToEmulatorSaveRoot(installRoot,
                                            preserveRootMetadata,
                                            payloadRoot,
                                            appliedFiles,
                                            fallbackError,
                                            progressCallback,
                                            progressUserData)) {
            UnmountBisUserFileSystem();
            RemovePathRecursively(stageRoot, cleanupError);
            error = fallbackError;
            return false;
        }

        fsdevCommitDevice(BisUserMountName().c_str());
        commitRc = 0;
        UnmountBisUserFileSystem();
    }

    RemovePathRecursively(stageRoot, cleanupError);

    if (R_FAILED(commitRc)) {
        RemovePathRecursively(backupRoot, cleanupError);
        error = "Falha ao confirmar a gravacao do save.";
        return false;
    }

    receipt = {};
    receipt.packageId = entry.id;
    receipt.packageVersion = entry.packageVersion;
    receipt.titleId = ToLowerAscii(entry.titleId);
    receipt.installRoot = installRoot;
    receipt.sourceUrl = variant.downloadUrl;
    receipt.installedAt = CurrentTimestamp();
    receipt.gameVersion = installedTitle ? installedTitle->displayVersion : std::string();
    receipt.installType = "save";
    receipt.backupPath = backupRoot;
    receipt.saveKind = variant.saveKind;
    receipt.saveUserId = accountUidIsValid(&saveUid) ? AccountUidToString(saveUid) : std::string();
    receipt.variantId = variant.id;
    receipt.files = std::move(appliedFiles);

    if (!SaveReceipt(receipt, error)) {
        RemovePathRecursively(backupRoot, cleanupError);
        return false;
    }

    return true;
}

bool UninstallPackage(const InstallReceipt& receipt,
                      std::string& error,
                      HttpProgressCallback progressCallback,
                      void* progressUserData) {
    if (receipt.installType == "save" && !receipt.titleId.empty()) {
        SaveVariantRecord variant;
        variant.id = receipt.variantId;
        variant.saveKind = receipt.saveKind.empty() ? "account" : receipt.saveKind;

        const std::uint64_t applicationId = std::strtoull(receipt.titleId.c_str(), nullptr, 16);
        if (applicationId == 0) {
            error = "Title ID invalido para restaurar o save.";
            return false;
        }

        Result commitRc = 0;
        AccountUid saveUid = {};
        FsFileSystem directSaveFs{};
        bool directSaveFsOpen = false;
        bool mountedSaveFs = false;
        std::string mountError;
        if (MountSaveDataForVariant(variant, applicationId, receipt.saveUserId, saveUid, mountError)) {
            mountedSaveFs = true;
        } else if (OpenSaveDataFileSystemForVariant(variant, applicationId, receipt.saveUserId, saveUid, directSaveFs, error)) {
            directSaveFsOpen = true;
            if (fsdevMountDevice(SaveMountName().c_str(), directSaveFs) == 0) {
                directSaveFsOpen = false;
                mountedSaveFs = true;
            }
        }

        if (mountedSaveFs) {
            const std::string mountRoot = SaveMountRoot();
            if (!ClearDirectoryContents(mountRoot, error)) {
                UnmountSaveData();
                return false;
            }

            if (!receipt.backupPath.empty() && PathExists(receipt.backupPath)) {
                std::uint64_t restoreBytes = 0;
                const std::uint64_t restoreTotal = std::max<std::uint64_t>(1, DirectorySizeBytes(receipt.backupPath));
                for (const std::string& name : ListDirectoryEntries(receipt.backupPath)) {
                    if (name == ".mil-empty") {
                        continue;
                    }
                    if (!CopyDirectoryTree(JoinPath(receipt.backupPath, name),
                                           JoinPath(mountRoot, name),
                                           nullptr,
                                           error,
                                           progressCallback,
                                           progressUserData,
                                           restoreBytes,
                                           restoreTotal)) {
                        UnmountSaveData();
                        return false;
                    }
                }
            }

            commitRc = fsdevCommitDevice(SaveMountName().c_str());
            UnmountSaveData();
        } else if (directSaveFsOpen) {
            if (!RestoreLocalBackupToSaveFileSystem(&directSaveFs,
                                                    receipt.backupPath,
                                                    error,
                                                    progressCallback,
                                                    progressUserData)) {
                fsFsClose(&directSaveFs);
                return false;
            }
            commitRc = fsFsCommit(&directSaveFs);
            if (R_FAILED(commitRc)) {
                commitRc = 0;
            }
            fsFsClose(&directSaveFs);
        } else {
            std::string fallbackError = error;
            if (receipt.installRoot.empty() || !StartsWith(receipt.installRoot, BisUserMountRoot())) {
                return false;
            }
            if (!MountBisUserFileSystem(fallbackError)) {
                error = fallbackError;
                return false;
            }
            if (!RestoreEmulatorSaveRoot(receipt.installRoot, receipt.backupPath, fallbackError, progressCallback, progressUserData)) {
                UnmountBisUserFileSystem();
                error = fallbackError.empty() ? "Falha ao restaurar o save no emulador." : fallbackError;
                return false;
            }
            commitRc = 0;
            UnmountBisUserFileSystem();
        }

        if (R_FAILED(commitRc)) {
            error = "Falha ao confirmar a restauracao do save.";
            return false;
        }

        std::string cleanupError;
        if (!receipt.backupPath.empty()) {
            RemovePathRecursively(receipt.backupPath, cleanupError);
        }
        if (remove(ReceiptPath(receipt.packageId).c_str()) != 0) {
            error = "Save restaurado, mas o recibo nao foi apagado.";
            return false;
        }
        return true;
    }

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
