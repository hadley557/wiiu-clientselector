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

WUPS_PLUGIN_NAME("Wii U Server Selector");
WUPS_PLUGIN_DESCRIPTION("Automatically select and switch server environments");
WUPS_PLUGIN_VERSION("v1.0.2.2");
WUPS_PLUGIN_AUTHOR("hadley557");
WUPS_PLUGIN_LICENSE("GPLv2");

WUPS_USE_STORAGE("WiiUServerSelector");
WUPS_USE_WUT_DEVOPTAB();

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle);
void ConfigMenuClosedCallback();

#define SERVERS_DIR "/vol/external01/wiiu-servers"
#define MODULES_DIR "/vol/external01/wiiu/environments/aroma/modules"
#define PLUGINS_DIR "/vol/external01/wiiu/environments/aroma/plugins"

struct ServerInfo {
    std::string name;
    std::string path;
    std::string identifier;
};

struct ServerActionItem {
    WUPSConfigItemHandle handle;
    char *identifier;
    std::string serverPath;
    bool isCurrent;
};

static std::vector<ServerInfo>& GetDetectedServers() {
    static std::vector<ServerInfo> instance;
    return instance;
}

static char pendingServerPath[256] = {0};
static OSThread workerThread;
alignas(8) static uint8_t workerStack[8192];

void ScanServers() {
    auto& detectedServers = GetDetectedServers();
    detectedServers.clear();
    DIR* dir = opendir(SERVERS_DIR);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            std::string name = entry->d_name;
            if (name != "." && name != "..") {
                detectedServers.push_back({
                    name, 
                    std::string(SERVERS_DIR) + "/" + name,
                    "server_" + name
                });
            }
        }
    }
    closedir(dir);
}

bool IsServerActive(const std::string& serverPath) {
    DIR* dir = opendir(serverPath.c_str());
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

            std::string srcPath = serverPath + "/" + filename;
            struct stat srcSt, targetSt;
            
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

void WipeAllServerFiles() {
    auto detectedServersCopy = GetDetectedServers();
    for (const auto& server : detectedServersCopy) {
        DIR* subDir = opendir(server.path.c_str());
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

void InstallServer(const std::string& serverPath) {
    WipeAllServerFiles();

    DIR* dir = opendir(serverPath.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename == "." || filename == "..") continue;

        std::string srcPath = serverPath + "/" + filename;
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
    InstallServer(std::string(pendingServerPath));
    OSSleepTicks(OSMillisecondsToTicks(1500));
    OSLaunchTitlev(OS_TITLE_ID_REBOOT, 0, NULL);
    return 0;
}

static int32_t ServerAction_getCurrentValueDisplay(void *context, char *out_buf, int32_t out_size) {
    if (out_size > 0) out_buf[0] = '\0';
    return 0;
}

static int32_t ServerAction_getCurrentValueSelectedDisplay(void *context, char *out_buf, int32_t out_size) {
    auto *item = (ServerActionItem *) context;
    if (item->isCurrent) {
        if (out_size > 0) out_buf[0] = '\0';
    } else {
        snprintf(out_buf, out_size, "Select \uE000");
    }
    return 0;
}

static void ServerAction_onInput(void *context, WUPSConfigSimplePadData input) {
    auto *item = (ServerActionItem *) context;
    if (item->isCurrent) {
        return; 
    }
    if (input.buttons_d & WUPS_CONFIG_BUTTON_A) {
        strncpy(pendingServerPath, item->serverPath.c_str(), sizeof(pendingServerPath) - 1);
        if (OSCreateThread(&workerThread, WorkerThreadProc, 0, nullptr, workerStack + sizeof(workerStack), sizeof(workerStack), 16, OS_THREAD_ATTRIB_DETACHED)) {
            OSResumeThread(&workerThread);
        }
    }
}

static void ServerAction_Cleanup(ServerActionItem *item) {
    if (!item) return;
    free(item->identifier);
    delete item;
}

static void ServerAction_onDelete(void *context) {
    ServerAction_Cleanup((ServerActionItem *) context);
}

WUPSConfigAPIStatus ServerAction_Create(const char *identifier, const char *displayName, const std::string& serverPath, bool isCurrent, WUPSConfigItemHandle *outHandle) {
    if (outHandle == nullptr) return WUPSCONFIG_API_RESULT_INVALID_ARGUMENT;

    auto *item = new (std::nothrow) ServerActionItem();
    if (item == nullptr) return WUPSCONFIG_API_RESULT_OUT_OF_MEMORY;

    item->identifier = identifier ? strdup(identifier) : nullptr;
    item->serverPath = serverPath;
    item->isCurrent = isCurrent;

    std::string finalDisplayName = displayName;
    if (isCurrent) {
        finalDisplayName += " (selected)";
    }

    WUPSConfigAPIItemCallbacksV2 callbacks = {
        .getCurrentValueDisplay         = &ServerAction_getCurrentValueDisplay,
        .getCurrentValueSelectedDisplay = &ServerAction_getCurrentValueSelectedDisplay,
        .onSelected                     = nullptr,
        .restoreDefault                 = nullptr,
        .isMovementAllowed              = nullptr,
        .onCloseCallback                = nullptr,
        .onInput                        = &ServerAction_onInput,
        .onInputEx                      = nullptr,
        .onDelete                       = &ServerAction_onDelete
    };

    WUPSConfigAPIItemOptionsV2 options = {
        .displayName = finalDisplayName.c_str(),
        .context     = item,
        .callbacks   = callbacks
    };

    WUPSConfigAPIStatus err;
    if ((err = WUPSConfigAPI_Item_Create(options, &item->handle)) != WUPSCONFIG_API_RESULT_SUCCESS) {
        ServerAction_Cleanup(item);
        return err;
    }

    *outHandle = item->handle;
    return WUPSCONFIG_API_RESULT_SUCCESS;
}

INITIALIZE_PLUGIN()
{
    ScanServers(); 
    WUPSConfigAPIOptionsV1 configOptions = {.name = "Wii U Server Selector"};
    WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback);
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle)
{
    auto& detectedServers = GetDetectedServers();

    if (detectedServers.empty()) {
        WUPSConfigItemHandle noneHandle;
        if (ServerAction_Create("server_none", "No servers found", "", false, &noneHandle) == WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Category_AddItem(rootHandle, noneHandle);
        }
    } else {
        for (size_t i = 0; i < detectedServers.size(); i++) {
            bool isCurrent = IsServerActive(detectedServers[i].path);
            WUPSConfigItemHandle itemHandle;
            if (ServerAction_Create(detectedServers[i].identifier.c_str(), detectedServers[i].name.c_str(), detectedServers[i].path, isCurrent, &itemHandle) == WUPSCONFIG_API_RESULT_SUCCESS) {
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