#include "include/opl.h"
#include "include/lang.h"
#include "include/util.h"
#include "include/iosupport.h"
#include "include/system.h"
#include "include/supportbase.h"
#include "include/ioman.h"
#include "modules/iopcore/common/cdvd_config.h"
#include "include/cheatman.h"
#include "include/pggsm.h"
#include "include/cheatman.h"
#include "include/ps2cnf.h"

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioMount("iso:", ***), fileXioUmount("iso:")
#include <io_common.h>   // FIO_MT_RDONLY
#include <ps2sdkapi.h>   // lseek64
#include <sys/stat.h>    // stat

#include "../modules/isofs/zso.h"

/// internal linked list used to populate the list from directory listing
struct game_list_t
{
    base_game_info_t gameinfo;
    struct game_list_t *next;
};

struct game_cache_list
{
    unsigned int count;
    base_game_info_t *games;
};

int sbIsSameSize(const char *prefix, int prevSize)
{
    int size = -1;
    char path[256];
    snprintf(path, sizeof(path), "%sul.cfg", prefix);

    int fd = openFile(path, O_RDONLY);
    if (fd >= 0) {
        size = getFileSize(fd);
        close(fd);
    }

    return size == prevSize;
}

int sbCreateSemaphore(void)
{
    ee_sema_t sema;

    sema.option = sema.attr = 0;
    sema.init_count = 1;
    sema.max_count = 1;
    return CreateSema(&sema);
}

// 0 = Not ISO disc image, GAME_FORMAT_OLD_ISO = legacy ISO disc image (filename follows old naming requirement), GAME_FORMAT_ISO = plain ISO image.
int isValidIsoName(char *name, int *pNameLen)
{
    // Old ISO image naming format: SCUS_XXX.XX.ABCDEFGHIJKLMNOP.iso

    // Minimum is 17 char, GameID (11) + "." (1) + filename (1 min.) + ".iso" (4)
    int size = strlen(name);
    if (strcasecmp(&name[size - 4], ".iso") == 0 || strcasecmp(&name[size - 4], ".zso") == 0) {
        if ((size >= 17) && (name[4] == '_') && (name[8] == '.') && (name[11] == '.')) {
            *pNameLen = size - 16;
            return GAME_FORMAT_OLD_ISO;
        } else if (size >= 5) {
            *pNameLen = size - 4;
            return GAME_FORMAT_ISO;
        }
    }

    return 0;
}

static int GetStartupExecName(const char *path, char *filename, int maxlength)
{
    char ps2disc_boot[CNF_PATH_LEN_MAX] = "";
    const char *key;
    int ret;

    if ((ret = ps2cnfGetBootFile(path, ps2disc_boot)) == 0) {
        /* Skip the device name part of the path ("cdrom0:\"). */
        key = ps2disc_boot;

        for (; *key != ':'; key++) {
            if (*key == '\0') {
                LOG("GetStartupExecName: missing ':' (%s).\n", ps2disc_boot);
                return -1;
            }
        }

        ++key;
        if (*key == '\\')
            key++;

        strncpy(filename, key, maxlength);
        filename[maxlength] = '\0';

        return 0;
    } else {
        LOG("GetStartupExecName: Could not get BOOT2 parameter.\n");
        return ret;
    }
}

static void freeISOGameListCache(struct game_cache_list *cache);

static int loadISOGameListCache(const char *path, struct game_cache_list *cache)
{
    char filename[256];
    FILE *file;
    base_game_info_t *games;
    int result, size, count;

    freeISOGameListCache(cache);

    sprintf(filename, "%s/games.bin", path);
    file = fopen(filename, "rb");
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        rewind(file);

        count = size / sizeof(base_game_info_t);
        if (count > 0) {
            games = memalign(64, count * sizeof(base_game_info_t));
            if (games != NULL) {
                if (fread(games, sizeof(base_game_info_t), count, file) == count) {
                    LOG("loadISOGameListCache: %d games loaded.\n", count);
                    cache->count = count;
                    cache->games = games;
                    result = 0;
                } else {
                    LOG("loadISOGameListCache: I/O error.\n");
                    free(games);
                    result = EIO;
                }
            } else {
                LOG("loadISOGameListCache: failed to allocate memory.\n");
                result = ENOMEM;
            }
        } else {
            result = -1; // Empty file (should not happen)
        }

        fclose(file);
    } else {
        result = ENOENT;
    }

    return result;
}

static void freeISOGameListCache(struct game_cache_list *cache)
{
    if (cache->games != NULL) {
        free(cache->games);
        cache->games = NULL;
        cache->count = 0;
    }
}

static int updateISOGameList(const char *path, const struct game_cache_list *cache, const struct game_list_t *head, int count)
{
    char filename[256];
    FILE *file;
    const struct game_list_t *game;
    int result, i, j, modified;
    base_game_info_t *list;

    modified = 0;
    if (cache != NULL) {
        if ((head != NULL) && (count > 0)) {
            game = head;

            for (i = 0; i < count; i++) {
                for (j = 0; j < cache->count; j++) {
                    if (strncmp(cache->games[i].name, game->gameinfo.name, ISO_GAME_NAME_MAX + 1) == 0 && strncmp(cache->games[i].extension, game->gameinfo.extension, ISO_GAME_EXTENSION_MAX + 1) == 0)
                        break;
                }

                if (j == cache->count) {
                    LOG("updateISOGameList: game added.\n");
                    modified = 1;
                    break;
                }

                game = game->next;
            }

            if ((!modified) && (count != cache->count)) {
                LOG("updateISOGameList: game removed.\n");
                modified = 1;
            }
        } else {
            modified = 0;
        }
    } else {
        modified = ((head != NULL) && (count > 0)) ? 1 : 0;
    }

    if (!modified)
        return 0;
    LOG("updateISOGameList: caching new game list.\n");

    result = 0;
    sprintf(filename, "%s/games.bin", path);
    if ((head != NULL) && (count > 0)) {
        list = (base_game_info_t *)memalign(64, sizeof(base_game_info_t) * count);

        if (list != NULL) {
            // Convert the linked list into a flat array, for writing performance.
            game = head;
            for (i = 0; (i < count) && (game != NULL); i++, game = game->next) {
                // copy one game, advance
                memcpy(&list[i], &game->gameinfo, sizeof(base_game_info_t));
            }

            file = fopen(filename, "wb");
            if (file != NULL) {
                result = fwrite(list, sizeof(base_game_info_t), count, file) == count ? 0 : EIO;

                fclose(file);

                if (result != 0)
                    remove(filename);
            } else
                result = EIO;

            free(list);
        } else
            result = ENOMEM;
    } else {
        // Last game deleted.
        remove(filename);
    }

    return result;
}

// Queries for the game entry, based on filename. Only the new filename format is supported (filename.ext).
static int queryISOGameListCache(const struct game_cache_list *cache, base_game_info_t *ginfo, const char *filename)
{
    char isoname[ISO_GAME_FNAME_MAX + 1];
    int i;

    for (i = 0; i < cache->count; i++) {
        snprintf(isoname, sizeof(isoname), "%s%s", cache->games[i].name, cache->games[i].extension);

        if (strcmp(filename, isoname) == 0) {
            memcpy(ginfo, &cache->games[i], sizeof(base_game_info_t));
            return 0;
        }
    }

    return ENOENT;
}

static int scanForISO(char *path, char type, struct game_list_t **glist)
{
    int NameLen, count = 0, format, MountFD, cacheLoaded;
    struct game_cache_list cache;
    base_game_info_t cachedGInfo;
    char fullpath[512], startup[GAME_STARTUP_MAX];
    struct dirent *dirent;
    DIR *dir;
    struct stat st;

    cache.games = NULL;
    cache.count = 0;
    cacheLoaded = loadISOGameListCache(path, &cache) == 0;

    if ((dir = opendir(path)) != NULL) {
        while ((dirent = readdir(dir)) != NULL) {
            if ((format = isValidIsoName(dirent->d_name, &NameLen)) > 0) {
                base_game_info_t *game;

                if (NameLen > ISO_GAME_NAME_MAX)
                    continue; // Skip files that cannot be supported properly.

                if (format == GAME_FORMAT_OLD_ISO) {
                    struct game_list_t *next = (struct game_list_t *)malloc(sizeof(struct game_list_t));

                    if (next != NULL) {
                        next->next = *glist;
                        *glist = next;

                        game = &(*glist)->gameinfo;
                        memset(game, 0, sizeof(base_game_info_t));

                        strncpy(game->name, &dirent->d_name[GAME_STARTUP_MAX], NameLen);
                        game->name[NameLen] = '\0';
                        strncpy(game->startup, dirent->d_name, GAME_STARTUP_MAX - 1);
                        game->startup[GAME_STARTUP_MAX - 1] = '\0';
                        strncpy(game->extension, &dirent->d_name[GAME_STARTUP_MAX + NameLen], sizeof(game->extension));
                        game->extension[sizeof(game->extension) - 1] = '\0';
                    } else {
                        // Out of memory.
                        break;
                    }
                } else {
                    if (queryISOGameListCache(&cache, &cachedGInfo, dirent->d_name) != 0) {
                        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, dirent->d_name);

                        if ((MountFD = fileXioMount("iso:", fullpath, FIO_MT_RDONLY)) >= 0) {
                            if (GetStartupExecName("iso:/SYSTEM.CNF;1", startup, GAME_STARTUP_MAX - 1) == 0) {
                                struct game_list_t *next = (struct game_list_t *)malloc(sizeof(struct game_list_t));

                                if (next != NULL) {
                                    next->next = *glist;
                                    *glist = next;

                                    game = &(*glist)->gameinfo;
                                    memset(game, 0, sizeof(base_game_info_t));

                                    strcpy(game->startup, startup);
                                    strncpy(game->name, dirent->d_name, NameLen);
                                    game->name[NameLen] = '\0';
                                    strncpy(game->extension, &dirent->d_name[NameLen], sizeof(game->extension));
                                    game->extension[sizeof(game->extension) - 1] = '\0';
                                } else {
                                    // Out of memory.
                                    fileXioUmount("iso:");
                                    break;
                                }
                            } else {
                                // Unable to parse SYSTEM.CNF.
                                fileXioUmount("iso:");
                                continue;
                            }

                            fileXioUmount("iso:");
                        } else {
                            // Unable to mount game.
                            continue;
                        }
                    } else {
                        // Entry was found in cache.
                        struct game_list_t *next = (struct game_list_t *)malloc(sizeof(struct game_list_t));

                        if (next != NULL) {
                            next->next = *glist;
                            *glist = next;

                            game = &(*glist)->gameinfo;
                            memcpy(game, &cachedGInfo, sizeof(base_game_info_t));
                        } else {
                            // Out of memory.
                            break;
                        }
                    }
                }

                // FIXED: use stat() instead of dirent->d_stat
                snprintf(fullpath, sizeof(fullpath), "%s/%s", path, dirent->d_name);
                if (stat(fullpath, &st) == 0) {
                    game->sizeMB = st.st_size >> 20;
                } else {
                    game->sizeMB = 0;
                }

                game->parts = 1;
                game->media = type;
                game->format = format;

                count++;
            }
        }
        closedir(dir);
    } else {
        count = 0;
    }

    if (cacheLoaded) {
        updateISOGameList(path, &cache, *glist, count);
        freeISOGameListCache(&cache);
    } else {
        updateISOGameList(path, NULL, *glist, count);
    }

    return count;
}

// ... Rest of file unchanged
