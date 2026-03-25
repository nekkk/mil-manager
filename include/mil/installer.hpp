#pragma once

#include <string>
#include <vector>

#include "mil/http.hpp"
#include "mil/models.hpp"

namespace mil {

std::vector<InstallReceipt> LoadInstallReceipts(std::string& note);
bool InstallPackage(const CatalogEntry& entry,
                    const InstalledTitle* installedTitle,
                    InstallReceipt& receipt,
                    std::string& error,
                    HttpProgressCallback progressCallback = nullptr,
                    void* progressUserData = nullptr);
bool InstallCheatText(const CatalogEntry& entry,
                      const std::string& buildId,
                      const std::string& downloadUrl,
                      const InstalledTitle* installedTitle,
                      InstallReceipt& receipt,
                      std::string& error,
                      HttpProgressCallback progressCallback = nullptr,
                      void* progressUserData = nullptr);
bool InstallCheatTextFromFile(const CatalogEntry& entry,
                              const std::string& buildId,
                              const std::string& sourcePath,
                              const InstalledTitle* installedTitle,
                              InstallReceipt& receipt,
                              std::string& error);
bool UninstallPackage(const InstallReceipt& receipt, std::string& error);
bool FindReceiptForPackage(const std::vector<InstallReceipt>& receipts, const std::string& packageId, InstallReceipt* receipt);
bool ExtractZipToDirectory(const std::string& zipPath,
                           const std::string& destinationDir,
                           std::vector<std::string>* extractedFiles,
                           std::string& error);

}  // namespace mil
