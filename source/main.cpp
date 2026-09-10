#include <vector>
#include <string>
#include <memory>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <wups.h>
#include <wups/config_api.h>

#include <mocha/mocha.h>

#include <coreinit/mcp.h>
#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <coreinit/launch.h>
#include <sysapp/launch.h>

#include "server_downloader.hpp"

WUPS_PLUGIN_NAME("Wii U Server Selector/Downloader");
WUPS_PLUGIN_DESCRIPTION("Downloads and automatically selects downloaded Miiverse servers");
WUPS_PLUGIN_VERSION("v1.0.0");
WUPS_PLUGIN_AUTHOR("hadley557 and NoobieDoesModding");
WUPS_PLUGIN_LICENSE("GPLv2");

WUPS_USE_STORAGE("WiiUClientSelector");
WUPS_USE_WUT_DEVOPTAB();

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle);
void ConfigMenuClosedCallback();

#define CLIENTS_DIR "/vol/external01/wiiu/wiiu-clients"
#define MODULES_DIR "/vol/external01/wiiu/environments/aroma/modules"
#define PLUGINS_DIR "/vol/external01/wiiu/environments/aroma/plugins"

struct ClientInfo {
    std::string name;
    std::string path;
    std::string identifier;
};

struct ClientActionItem {
    WUPSConfigItemHandle handle;
    char *identifier;
    std::string clientPath;
    bool isCurrent;
};

static std::vector<ClientInfo>& GetDetectedClients() {
    static std::vector<ClientInfo> instance;
    return instance;
}

static char pendingClientPath[256] = {0};
static OSThread workerThread;
alignas(8) static uint8_t workerStack[4096];

void ScanClients() {
    auto& detectedClients = GetDetectedClients();
    detectedClients.clear();
    DIR* dir = opendir(CLIENTS_DIR);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            std::string name = entry->d_name;
            if (name != "." && name != "..") {
                detectedClients.push_back({
                    name, 
                    std::string(CLIENTS_DIR) + "/" + name,
                    "client_" + name
                });
            }
        }
    }
    closedir(dir);
}

// Check if a client is active by validating that every file inside the client directory 
// matches the corresponding installed file's size exactly.
bool IsClientActive(const std::string& clientPath) {
    DIR* dir = opendir(clientPath.c_str());
    if (!dir) return false;

    bool active = true;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename == "." || filename == "..") continue;

        if (filename.length() > 4) {
            std::string ext = filename.substr(filename.length() - 4);
            std::string targetPath;
            if (ext == ".wms") {
                targetPath = std::string(MODULES_DIR) + "/" + filename;
            } else if (ext == ".wps") {
                targetPath = std::string(PLUGINS_DIR) + "/" + filename;
            } else {
                continue;
            }

            std::string srcPath = clientPath + "/" + filename;
            struct stat srcSt, targetSt;
            
            // Both files must exist and their sizes must match precisely
            if (stat(srcPath.c_str(), &srcSt) != 0 || stat(targetPath.c_str(), &targetSt) != 0 || srcSt.st_size != targetSt.st_size) {
                active = false;
                break;
            }
        }
    }
    closedir(dir);
    return active;
}

bool CopyFile(const std::string& src, const std::string& dest) {
    struct stat st;
    if (stat(src.c_str(), &st) != 0) return false;
    long size = st.st_size;
    if (size <= 0) return false;

    int fdsrc = open(src.c_str(), O_RDONLY);
    if (fdsrc < 0) return false;

    std::vector<char> buffer(size);
    ssize_t readBytes = read(fdsrc, buffer.data(), size);
    close(fdsrc);

    if (readBytes != size) return false;

    unlink(dest.c_str());

    int fddest = open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fddest < 0) return false;

    ssize_t writtenBytes = write(fddest, buffer.data(), size);
    fsync(fddest);
    close(fddest);

    return writtenBytes == size;
}

void WipeAllClientFiles() {
    auto& detectedClients = GetDetectedClients();
    for (const auto& client : detectedClients) {
        DIR* subDir = opendir(client.path.c_str());
        if (!subDir) continue;

        struct dirent* fileEntry;
        while ((fileEntry = readdir(subDir)) != nullptr) {
            std::string filenameStr = fileEntry->d_name;
            if (filenameStr == "." || filenameStr == "..") continue;

            if (filenameStr.length() > 4) {
                std::string ext = filenameStr.substr(filenameStr.length() - 4);
                if (ext == ".wms") {
                    std::string targetPath = std::string(MODULES_DIR) + "/" + filenameStr;
                    unlink(targetPath.c_str());
                } else if (ext == ".wps") {
                    std::string targetPath = std::string(PLUGINS_DIR) + "/" + filenameStr;
                    unlink(targetPath.c_str());
                }
            }
        }
        closedir(subDir);
    }
}

void InstallClient(const std::string& clientPath) {
    ScanClients();
    WipeAllClientFiles();

    DIR* dir = opendir(clientPath.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename == "." || filename == "..") continue;

        std::string srcPath = clientPath + "/" + filename;
        if (filename.length() > 4) {
            std::string ext = filename.substr(filename.length() - 4);
            if (ext == ".wms") {
                std::string destPath = std::string(MODULES_DIR) + "/" + filename;
                CopyFile(srcPath, destPath);
            } else if (ext == ".wps") {
                std::string destPath = std::string(PLUGINS_DIR) + "/" + filename;
                CopyFile(srcPath, destPath);
            }
        }
    }
    closedir(dir);
}

static int WorkerThreadProc(int argc, const char **argv) {
    InstallClient(std::string(pendingClientPath));
    OSSleepTicks(OSMillisecondsToTicks(1500));
    OSLaunchTitlev(OS_TITLE_ID_REBOOT, 0, NULL);
    return 0;
}

static int32_t ClientAction_getCurrentValueDisplay(void *context, char *out_buf, int32_t out_size) {
    if (out_size > 0) out_buf[0] = '\0';
    return 0;
}

static int32_t ClientAction_getCurrentValueSelectedDisplay(void *context, char *out_buf, int32_t out_size) {
    auto *item = (ClientActionItem *) context;
    if (item->isCurrent) {
        if (out_size > 0) out_buf[0] = '\0';
    } else {
        snprintf(out_buf, out_size, "Select \uE000");
    }
    return 0;
}

static void ClientAction_onInput(void *context, WUPSConfigSimplePadData input) {
    auto *item = (ClientActionItem *) context;
    if (item->isCurrent) {
        return; // Do nothing if it's already the active client
    }
    if (input.buttons_d & WUPS_CONFIG_BUTTON_A) {
        strncpy(pendingClientPath, item->clientPath.c_str(), sizeof(pendingClientPath) - 1);
        OSCreateThread(&workerThread, WorkerThreadProc, 0, nullptr, workerStack + sizeof(workerStack), sizeof(workerStack), 16, 0);
        OSResumeThread(&workerThread);
    }
}

static void ClientAction_Cleanup(ClientActionItem *item) {
    if (!item) return;
    free(item->identifier);
    delete item;
}

static void ClientAction_onDelete(void *context) {
    ClientAction_Cleanup((ClientActionItem *) context);
}

WUPSConfigAPIStatus ClientAction_Create(const char *identifier, const char *displayName, const std::string& clientPath, bool isCurrent, WUPSConfigItemHandle *outHandle) {
    if (outHandle == nullptr) return WUPSCONFIG_API_RESULT_INVALID_ARGUMENT;

    auto *item = new (std::nothrow) ClientActionItem();
    if (item == nullptr) return WUPSCONFIG_API_RESULT_OUT_OF_MEMORY;

    item->identifier = identifier ? strdup(identifier) : nullptr;
    item->clientPath = clientPath;
    item->isCurrent = isCurrent;

    std::string finalDisplayName = displayName;
    if (isCurrent) {
        finalDisplayName += " (selected)";
    }

    WUPSConfigAPIItemCallbacksV2 callbacks = {
        .getCurrentValueDisplay         = &ClientAction_getCurrentValueDisplay,
        .getCurrentValueSelectedDisplay = &ClientAction_getCurrentValueSelectedDisplay,
        .onSelected                     = nullptr,
        .restoreDefault                 = nullptr,
        .isMovementAllowed              = nullptr,
        .onCloseCallback                = nullptr,
        .onInput                        = &ClientAction_onInput,
        .onInputEx                      = nullptr,
        .onDelete                       = &ClientAction_onDelete
    };

    WUPSConfigAPIItemOptionsV2 options = {
        .displayName = finalDisplayName.c_str(),
        .context     = item,
        .callbacks   = callbacks
    };

    WUPSConfigAPIStatus err;
    if ((err = WUPSConfigAPI_Item_Create(options, &item->handle)) != WUPSCONFIG_API_RESULT_SUCCESS) {
        ClientAction_Cleanup(item);
        return err;
    }

    *outHandle = item->handle;
    return WUPSCONFIG_API_RESULT_SUCCESS;
}

INITIALIZE_PLUGIN()
{
    ScanClients(); 
    WUPSConfigAPIOptionsV1 configOptions = {.name = "Wii U Client Selector"};
    WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback);
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle)
{
    auto& detectedClients = GetDetectedClients();

    // Small integration point: the downloader is implemented entirely in
    // server_downloader.cpp and only receives the existing config root here.
    ServerDownloader::AddDownloadServersMenu(rootHandle);

    if (detectedClients.empty()) {
        WUPSConfigItemHandle noneHandle;
        if (ClientAction_Create("client_none", "No clients found", "", false, &noneHandle) == WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Category_AddItem(rootHandle, noneHandle);
        }
    } else {
        for (size_t i = 0; i < detectedClients.size(); i++) {
            bool isCurrent = IsClientActive(detectedClients[i].path);
            WUPSConfigItemHandle itemHandle;
            if (ClientAction_Create(detectedClients[i].identifier.c_str(), detectedClients[i].name.c_str(), detectedClients[i].path, isCurrent, &itemHandle) == WUPSCONFIG_API_RESULT_SUCCESS) {
                WUPSConfigAPI_Category_AddItem(rootHandle, itemHandle);
            }
        }
    }

    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback()
{
    WUPSStorageAPI::SaveStorage(false);
}