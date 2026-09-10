#include "server_downloader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <zlib.h>

#include <curl/curl.h>
#include <mbedtls/sha256.h>

#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <wups/config_api.h>

namespace ServerDownloader {

namespace {

constexpr const char *kClientRoot = "/vol/external01/wiiu/wiiu-clients";
constexpr const char *kDownloadsRoot = "/vol/external01/wiiu/wiiu-clients/downloads";
constexpr const char *kManifestUrl = "https://raw.githubusercontent.com/NoobieDoesModding/wiiu-serverselector/refs/heads/main/servers.manifest";
// Replace kManifestUrl with the URL of the project's maintained manifest.

struct DownloadItem {
    WUPSConfigItemHandle handle{};
    char *identifier = nullptr;
    ServerInfo server;
};

struct ManifestBuffer {
    std::string data;
};

struct ProgressState {
    const char *filename = nullptr;
    curl_off_t total = 0;
    curl_off_t downloaded = 0;
};

static bool gCurlInitialized = false;
static std::vector<ServerInfo> gServers;
static std::vector<DownloadItem *> gItems;

static volatile bool gBusy = false;
static volatile int gProgress = 0;
static char gStatus[128] = "Ready";

static OSThread gWorkerThread;
alignas(16) static uint8_t gWorkerStack[65536]; // 16 bytes alignment for the stack, 64 KB size
static ServerInfo gPendingServer;

void SetStatus(const char *status) {
    if (!status) status = "";
    std::snprintf(gStatus, sizeof(gStatus), "%s", status);
}

bool IsSafeComponent(const std::string &value) {
    if (value.empty() || value == "." || value == "..") return false;
    if (value.find("..") != std::string::npos) return false;

    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ' ') {
            continue;
        }
        return false;
    }
    return true;
}

bool IsSha256(const std::string &hash) {
    if (hash.size() != 64) return false;
    return std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool IsHttpsUrl(const std::string &url) {
    return url.rfind("https://", 0) == 0;
}

bool EnsureDirectory(const std::string &path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    // All parents here are fixed/sanitized, so a single mkdir is sufficient.
    return mkdir(path.c_str(), 0777) == 0;
}

size_t CurlWriteString(void *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *buffer = static_cast<ManifestBuffer *>(userdata);
    const size_t bytes = size * nmemb;
    buffer->data.append(static_cast<const char *>(ptr), bytes);
    return bytes;
}

size_t CurlWriteFile(void *ptr, size_t size, size_t nmemb, void *userdata) {
    FILE *file = static_cast<FILE *>(userdata);
    return std::fwrite(ptr, size, nmemb, file);
}

int CurlProgress(void *userdata,
                 curl_off_t total,
                 curl_off_t now,
                 curl_off_t /*ultotal*/,
                 curl_off_t /*ulnow*/) {
    auto *state = static_cast<ProgressState *>(userdata);
    state->total = total;
    state->downloaded = now;

    if (total > 0) {
        gProgress = static_cast<int>((now * 100) / total);
        if (gProgress > 100) gProgress = 100;
    } else {
        gProgress = 0;
    }
    return 0;
}

bool HttpGetText(const std::string &url, std::string &out) {
    if (!IsHttpsUrl(url) || !Initialize()) return false;

    ManifestBuffer buffer;
    char error[CURL_ERROR_SIZE] = {};

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "WiiU-ServerSelector/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

    const CURLcode result = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK || httpCode < 200 || httpCode >= 300) {
        OSReport("[ServerDownloader] Manifest request failed: %s (HTTP %ld)\n",
                 error[0] ? error : curl_easy_strerror(result), httpCode);
        return false;
    }

    out = std::move(buffer.data);
    return true;
}

bool DownloadToFile(const DownloadFile &file, const std::string &temporaryPath) {
    if (!IsHttpsUrl(file.url)) {
        OSReport("[ServerDownloader] Refusing non-HTTPS URL for %s\n", file.name.c_str());
        return false;
    }

    FILE *output = std::fopen(temporaryPath.c_str(), "wb");
    if (!output) return false;

    ProgressState progress{file.name.c_str(), 0, 0};
    char error[CURL_ERROR_SIZE] = {};

    CURL *curl = curl_easy_init();
    if (!curl) {
        std::fclose(output);
        unlink(temporaryPath.c_str());
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, file.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, output);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "WiiU-ServerSelector/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);

    const CURLcode result = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    std::fflush(output);
    std::fclose(output);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK || httpCode < 200 || httpCode >= 300) {
        OSReport("[ServerDownloader] Download failed for %s: %s (HTTP %ld)\n",
                 file.name.c_str(), error[0] ? error : curl_easy_strerror(result), httpCode);
        unlink(temporaryPath.c_str());
        return false;
    }

    return true;
}

bool VerifySha256(const std::string &path, const std::string &expected) {
    if (!IsSha256(expected)) return false;

    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;

    mbedtls_sha256_context ctx;
    unsigned char digest[32];
    std::vector<unsigned char> buffer(64 * 1024); // 64 KB buffer for reading the file in chunks

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    bool ok = true;
    while (true) {
        const size_t count = std::fread(buffer.data(), 1, buffer.size(), file);
        if (count > 0) {
            mbedtls_sha256_update(&ctx, buffer.data(), count);
        }

        if (count < sizeof(buffer)) {
            if (std::ferror(file)) ok = false;
            break;
        }
    }

    std::fclose(file);

    if (!ok) {
        mbedtls_sha256_free(&ctx);
        return false;
    }

    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    char actual[65];
    for (size_t i = 0; i < sizeof(digest); ++i) {
        std::snprintf(&actual[i * 2], 3, "%02x", digest[i]);
    }
    actual[64] = '\0';

    std::string normalized = expected;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return normalized == actual;
}


uint16_t ReadLe16(const unsigned char *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t ReadLe32(const unsigned char *p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
// Lightweight manifest format. It deliberately avoids adding a large JSON library
// to this plugin:
//
//   server|Name|Version|Description
//   file|Name|filename.wps|https://...|64-char-sha256
//
// Description must not contain '|'. Blank lines and # comments are ignored.
bool ParseManifest(const std::string &text, std::vector<ServerInfo> &servers) {
    servers.clear();

    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();

        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (!line.empty() && line[0] != '#') {
            std::vector<std::string> fields;
            size_t p = 0;
            while (p <= line.size()) {
                size_t q = line.find('|', p);
                if (q == std::string::npos) q = line.size();
                fields.emplace_back(line.substr(p, q - p));
                if (q == line.size()) break;
                p = q + 1;
            }

            if (fields.size() >= 4 && fields[0] == "server") {
                if (!IsSafeComponent(fields[1]) || fields[2].empty()) return false;
                servers.push_back({fields[1], fields[3], fields[2], {}});
            } else if (fields.size() == 5 && fields[0] == "file") {
                auto it = std::find_if(servers.begin(), servers.end(),
                                       [&](const ServerInfo &s) { return s.name == fields[1]; });
                if (it == servers.end() || !IsSafeComponent(fields[2]) ||
                    !IsHttpsUrl(fields[3]) || !IsSha256(fields[4])) {
                    return false;
                }

                it->files.push_back({fields[2], fields[3], fields[4], 0});
            } else {
                return false;
            }
        }

        start = end + 1;
    }

    for (const auto &server : servers) {
        if (server.files.empty()) return false;
    }
    return !servers.empty();
}

bool LoadManifest() {
    SetStatus("Fetching server list...");
    std::string text;
    if (!HttpGetText(kManifestUrl, text)) {
        SetStatus("Manifest download failed");
        return false;
    }

    std::vector<ServerInfo> parsed;
    if (!ParseManifest(text, parsed)) {
        SetStatus("Invalid server manifest");
        OSReport("[ServerDownloader] Manifest validation failed.\n");
        return false;
    }

    gServers = std::move(parsed);

    // Built-in Protarium Network entry, pinned to the published r10 release.
    // Future servers should still be supplied through the remote manifest.
    {
        ServerInfo protarium{
            "Protarium Network",
            "Protarium online services",
            "3.1.0-r10",
            {}
        };

        protarium.files.push_back({
            "Inkay-Pretendo.wps",
            "https://github.com/NoobieDoesModding/wiiu-serverselector/releases/download/Release/Inkay-pretendo.wps",
            "8c0c3d4324e35b685b44acdfd352fd516a5c7aa77bbefc27a4558aac54ad2ed7",
            0
        });

        protarium.files.push_back({
            "Inkay-Pretendo.wms",
            "https://github.com/NoobieDoesModding/wiiu-serverselector/releases/download/Release/Inkay-pretendo.wms",
            "0f01440676e4de04c53086b26616d8d33b665a054bad99c7dd286adfa764968f",
            0
        });

        gServers.erase(
            std::remove_if(gServers.begin(), gServers.end(),
                           [](const ServerInfo &s) { return s.name == "Protarium Network"; }),
            gServers.end()
        );
        gServers.push_back(std::move(protarium));
    }


    // Official Pretendo Network package. The current official release is distributed
    // as a ZIP containing Inkay-pretendo.wps and Inkay-pretendo.wms.
    {
        ServerInfo pretendo{
            "Pretendo Network",
            "Official Pretendo Network Inkay package",
            "3.0.0",
            {}
        };

        pretendo.files.push_back({
            "Inkay-pretendo.wps",
            "https://github.com/NoobieDoesModding/wiiu-serverselector/releases/download/Releas/Inkay-pretendo.wps",
            "e97b27c73b00bd15486f1d32f2c7597fd8109831fd7cf413f5290ac5c0a8df9a",
            0
        });

        pretendo.files.push_back({
            "Inkay-pretendo.wms",
            "https://github.com/NoobieDoesModding/wiiu-serverselector/releases/download/Releas/Inkay-pretendo.wms",
            "5b4d5eb31db1a1b35bf079ba9805acd8c324cc2674ef47ec9a99c88fe53b9369",
            0
        });

        gServers.erase(
            std::remove_if(gServers.begin(), gServers.end(),
                           [](const ServerInfo &s) { return s.name == "Pretendo Network"; }),
            gServers.end()
        );
        gServers.push_back(std::move(pretendo));
    }


        {
        ServerInfo roseverse{
            "Roseverse",
            "Official Roseverse Inkay package",
            "3.0.0",
            {}
        };

        roseverse.files.push_back({
            "Inkay-roseverse.wps",
            "https://github.com/NoobieDoesModding/wiiu-serverselector/releases/download/Roseverse/Inkay-pretendo.wps",
            "d688f00b5dfe257e38d63db0007363f19c7e42203899bda98a15302243d84f50",
            0
        });

        roseverse.files.push_back({
            "Inkay-roseverse.wms",
            "https://github.com/NoobieDoesModding/wiiu-serverselector/releases/download/Roseverse/Inkay-pretendo.wms",
            "27dd33ac9d4122d7a8aa4e55a08727ab143a91dad76457bfd1747626d7af67c5",
            0
        });

        gServers.erase(
            std::remove_if(gServers.begin(), gServers.end(),
                           [](const ServerInfo &s) { return s.name == "Roseverse"; }),
            gServers.end()
        );
        gServers.push_back(std::move(roseverse));
    }

    OSReport("[ServerDownloader] Found %zu servers.\n", gServers.size());
    SetStatus("Server list ready");
    return true;
}

void CleanupPartialInstall(const std::string &stagingPath) {
    DIR *dir = opendir(stagingPath.c_str());
    if (!dir) {
        rmdir(stagingPath.c_str());
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        unlink((stagingPath + "/" + name).c_str());
    }
    closedir(dir);
    rmdir(stagingPath.c_str());
}

bool InstallServerInternal(const ServerInfo &server) {
    if (!IsSafeComponent(server.name) || server.files.empty()) return false;

// Ensure all parent directories exist sequentially
EnsureDirectory("/vol/external01/wiiu");
EnsureDirectory(kClientRoot);
EnsureDirectory(kDownloadsRoot);

const std::string finalPath = std::string(kClientRoot) + "/" + server.name;
const std::string stagingPath = std::string(kDownloadsRoot) + "/" + server.name;

// Clean up any interrupted downloads from before
CleanupPartialInstall(stagingPath);
if (mkdir(stagingPath.c_str(), 0777) != 0 && errno != EEXIST) return false;

// Clear out the existing installed server (if any) before placing new files
CleanupPartialInstall(finalPath); 
if (mkdir(finalPath.c_str(), 0777) != 0 && errno != EEXIST) return false;

for (size_t i = 0; i < server.files.size(); ++i) {
    const auto &file = server.files[i];
    const std::string downloadPath = stagingPath + "/" + file.name;

    char status[128];
    std::snprintf(status, sizeof(status), "Downloading %s", file.name.c_str());
    SetStatus(status);
    gProgress = 0;

    // 1. Download directly to the /downloads/ folder
    if (!DownloadToFile(file, downloadPath)) {
        CleanupPartialInstall(stagingPath);
        SetStatus("Download failed");
        return false;
    }

    // 2. Verify SHA256
    SetStatus("Verifying file...");
    if (!VerifySha256(downloadPath, file.sha256)) {
        OSReport("[ServerDownloader] SHA-256 mismatch for %s\n", file.name.c_str());
        unlink(downloadPath.c_str());
        CleanupPartialInstall(stagingPath);
        SetStatus("Checksum mismatch");
        return false;
    }
        // It's a standard file (.wps or .wms), move it to the final directory
        const std::string destFilePath = finalPath + "/" + file.name;
        if (rename(downloadPath.c_str(), destFilePath.c_str()) != 0) {
            unlink(downloadPath.c_str());
            CleanupPartialInstall(stagingPath);
            CleanupPartialInstall(finalPath);
            SetStatus("SD card move failed");
            return false;
        }
    }


// Remove the temporary downloads folder for this server
CleanupPartialInstall(stagingPath);

SetStatus("Server installed");
gProgress = 100;
return true;
}

static int WorkerProc(int, const char **) {
    const bool success = InstallServerInternal(gPendingServer);
    OSReport("[ServerDownloader] %s: %s\n",
             gPendingServer.name.c_str(), success ? "installation completed" : "installation failed");
    gBusy = false;
    return 0;
}

int32_t GetCurrentValue(void *, char *out, int32_t outSize) {
    if (!out || outSize <= 0) return 0;

    if (gBusy) {
        std::snprintf(out, outSize, "%s %d%%", gStatus, gProgress);
    } else {
        std::snprintf(out, outSize, "%s", gStatus);
    }
    return 0;
}

int32_t GetSelectedValue(void *context, char *out, int32_t outSize) {
    if (!out || outSize <= 0) return 0;
    auto *item = static_cast<DownloadItem *>(context);

    if (gBusy) {
        std::snprintf(out, outSize, "Busy");
    } else {
        std::snprintf(out, outSize, "Download");
    }

    (void)item;
    return 0;
}

void CleanupItem(DownloadItem *item) {
    if (!item) return;
    if (item->identifier) std::free(item->identifier);
    delete item;
}

void OnDelete(void *context) {
    auto *item = static_cast<DownloadItem *>(context);
    CleanupItem(item);
}

void OnInput(void *context, WUPSConfigSimplePadData input) {
    auto *item = static_cast<DownloadItem *>(context);
    if (!item || gBusy || !(input.buttons_d & WUPS_CONFIG_BUTTON_A)) return;

    gPendingServer = item->server;
    gBusy = true;
    gProgress = 0;
    SetStatus("Starting download...");

    OSCreateThread(&gWorkerThread, WorkerProc, 0, nullptr,
                   gWorkerStack + sizeof(gWorkerStack),
                   sizeof(gWorkerStack), 16, 0);
    OSResumeThread(&gWorkerThread);
}

bool AddServerItem(WUPSConfigCategoryHandle root, const ServerInfo &server) {
    auto *item = new (std::nothrow) DownloadItem();
    if (!item) return false;

    item->server = server;

    std::string identifier = "download_" + server.name;
    item->identifier = strdup(identifier.c_str());
    if (!item->identifier) {
        delete item;
        return false;
    }

    std::string display = server.name;
    if (!server.version.empty()) {
        display += " v" + server.version;
    }
    if (!server.description.empty()) {
        display += " - " + server.description;
    }

    WUPSConfigAPIItemCallbacksV2 callbacks = {
        .getCurrentValueDisplay = &GetCurrentValue,
        .getCurrentValueSelectedDisplay = &GetSelectedValue,
        .onSelected = nullptr,
        .restoreDefault = nullptr,
        .isMovementAllowed = nullptr,
        .onCloseCallback = nullptr,
        .onInput = &OnInput,
        .onInputEx = nullptr,
        .onDelete = &OnDelete
    };

    WUPSConfigAPIItemOptionsV2 options = {
        .displayName = display.c_str(),
        .context = item,
        .callbacks = callbacks
    };

    if (WUPSConfigAPI_Item_Create(options, &item->handle) != WUPSCONFIG_API_RESULT_SUCCESS) {
        CleanupItem(item);
        return false;
    }

    gItems.push_back(item);
    WUPSConfigAPI_Category_AddItem(root, item->handle);
    return true;
}

} // namespace

bool Initialize() {
    if (gCurlInitialized) return true;

    const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (result != CURLE_OK) {
        OSReport("[ServerDownloader] curl_global_init failed: %s\n", curl_easy_strerror(result));
        return false;
    }

    gCurlInitialized = true;
    OSReport("[ServerDownloader] CURL initialized.\n");
    return true;
}

bool DownloadServer(const ServerInfo &server) {
    if (gBusy) return false;
    gPendingServer = server;
    gBusy = true;
    const bool result = InstallServerInternal(gPendingServer);
    gBusy = false;
    return result;
}

void AddDownloadServersMenu(WUPSConfigCategoryHandle rootHandle) {
    if (!Initialize()) {
        WUPSConfigItemHandle handle;
        WUPSConfigAPIItemCallbacksV2 callbacks = {};
        WUPSConfigAPIItemOptionsV2 options = {
            .displayName = "Download Servers (network unavailable)",
            .context = nullptr,
            .callbacks = callbacks
        };
        if (WUPSConfigAPI_Item_Create(options, &handle) == WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Category_AddItem(rootHandle, handle);
        }
        return;
    }

    if (!LoadManifest()) {
        WUPSConfigItemHandle handle;
        WUPSConfigAPIItemCallbacksV2 callbacks = {};
        WUPSConfigAPIItemOptionsV2 options = {
            .displayName = "Download Servers (manifest unavailable)",
            .context = nullptr,
            .callbacks = callbacks
        };
        if (WUPSConfigAPI_Item_Create(options, &handle) == WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Category_AddItem(rootHandle, handle);
        }
        return;
    }

    for (const auto &server : gServers) {
        AddServerItem(rootHandle, server);
    }
}

} // namespace ServerDownloader
