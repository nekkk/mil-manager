#pragma once

#include <string>
#include <vector>

#include <switch.h>

#include "mil/models.hpp"

namespace mil {

enum class RuntimeEnvironment {
    Unknown,
    Console,
    Emulator,
};

struct PlatformSession {
    Framebuffer framebuffer{};
    bool framebufferReady = false;
    bool nifmReady = false;
    bool socketReady = false;
    bool romfsReady = false;
    bool exitLocked = false;
};

bool InitializePlatform(PlatformSession& session, std::string& note);
void ShutdownPlatform(PlatformSession& session);
std::vector<InstalledTitle> LoadInstalledTitles(const AppConfig& config, const CatalogIndex* catalog, std::string& note);
bool TryResolveInstalledTitleBuildId(InstalledTitle& title, bool allowNetworkFallback, std::string& note);
std::string GetPreferredLanguageCode();
bool IsEmulatorEnvironment();
RuntimeEnvironment GetRuntimeEnvironment();
const char* RuntimeEnvironmentLabel(RuntimeEnvironment environment);
std::string GetLoaderInfoSummary();

}  // namespace mil
