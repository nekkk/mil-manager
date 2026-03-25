#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mil {

using HttpProgressCallback = void (*)(std::uint64_t downloadedBytes,
                                      std::uint64_t totalBytes,
                                      bool totalKnown,
                                      void* userData);

struct HttpResponse {
    long statusCode = 0;
    std::string body;
};

struct HttpDownloadInfo {
    bool sizeKnown = false;
    std::uint64_t contentLength = 0;
    bool acceptRanges = false;
};

struct HttpDownloadOptions {
    long connectTimeoutMs = 12000;
    long requestTimeoutMs = 120000;
    int retryCount = 3;
    bool probeDownloadInfo = true;
    bool allowResume = true;
    HttpProgressCallback progressCallback = nullptr;
    void* progressUserData = nullptr;
};

bool HttpGetToString(const std::string& url, HttpResponse& response, std::string& error);
bool HttpGetDownloadInfo(const std::string& url, HttpDownloadInfo& info, std::string& error);
bool HttpDownloadToFile(const std::string& url, const std::string& outputPath, std::size_t* downloadedBytes, std::string& error);
bool HttpDownloadToFileWithOptions(const std::string& url,
                                   const std::string& outputPath,
                                   const HttpDownloadOptions& options,
                                   std::size_t* downloadedBytes,
                                   std::string& error);
bool HttpGetWithCache(const std::vector<std::string>& urls,
                      const std::string& cachePath,
                      HttpResponse& response,
                      std::string& source,
                      std::string& error);

}  // namespace mil
