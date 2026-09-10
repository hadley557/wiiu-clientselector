#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <wups/config_api.h>

namespace ServerDownloader {

struct DownloadFile {
    std::string name;
    std::string url;
    std::string sha256;
    uint64_t size = 0;
};

struct ServerInfo {
    std::string name;
    std::string description;
    std::string version;
    std::vector<DownloadFile> files;
};

// Adds the downloader entries to the existing Server Selector config category.
// This is intentionally a small integration surface for main.cpp.
void AddDownloadServersMenu(WUPSConfigCategoryHandle rootHandle);

// Initializes networking used by the downloader. Safe to call more than once.
bool Initialize();

// Downloads and atomically installs one complete server configuration.
bool DownloadServer(const ServerInfo& server);

} // namespace ServerDownloader
