/***************************************************************************
 * Copyright (C) 2015
 * by Dimok
 *
 * This software is provided 'as-is', without any express or implied
 * warranty. In no event will the authors be held liable for any
 * damages arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any
 * purpose, including commercial applications, and to alter it and
 * redistribute it freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you
 * must not claim that you wrote the original software. If you use
 * this software in a product, an acknowledgment in the product
 * documentation would be appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and
 * must not be misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source
 * distribution.
 ***************************************************************************/
#include "iosuhax.h"
#include "os_functions.h"
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/dirent.h>
#include <sys/iosupport.h>
#include <sys/statvfs.h>

#define IOS_ERROR_UNKNOWN_VALUE 0xFFFFFFD6
#define IOS_ERROR_INVALID_ARG   0xFFFFFFE3
#define IOS_ERROR_INVALID_SIZE  0xFFFFFFE9
#define IOS_ERROR_UNKNOWN       0xFFFFFFF7
#define IOS_ERROR_NOEXISTS      0xFFFFFFFA

typedef struct _fs_dev_private_t {
    char *mount_path;
    int fsaFd;
    int mounted;
    void *pMutex;
} fs_dev_private_t;

typedef struct _fs_dev_file_state_t {
    fs_dev_private_t *dev;
    int fd;                                    /* File descriptor */
    int flags;                                 /* Opening flags */
    char path[PATH_MAX];                       /* Path from opened file */
    bool read;                                 /* True if allowed to read from file */
    bool write;                                /* True if allowed to write to file */
    bool append;                               /* True if allowed to append to file */
    uint32_t pos;                              /* Current position within the file (in bytes) */
    uint32_t len;                              /* Total length of the file (in bytes) */
    struct _fs_dev_file_state_t *prevOpenFile; /* The previous entry in a double-linked FILO list of open files */
    struct _fs_dev_file_state_t *nextOpenFile; /* The next entry in a double-linked FILO list of open files */
} fs_dev_file_state_t;

typedef struct _fs_dev_dir_entry_t {
    fs_dev_private_t *dev;
    int dirHandle;
} fs_dev_dir_entry_t;

static fs_dev_private_t *fs_dev_get_device_data(const char *path) {
    const devoptab_t *devoptab = NULL;
    char name[128]             = {0};
    int i;

    // Get the device name from the path
    strncpy(name, path, 127);
    strtok(name, ":/");

    // Search the devoptab table for the specified device name
    // NOTE: We do this manually due to a 'bug' in GetDeviceOpTab
    //       which ignores names with suffixes and causes names
    //       like "ntfs" and "ntfs1" to be seen as equals
    for (i = 3; i < STD_MAX; i++) {
        devoptab = devoptab_list[i];
        if (devoptab && devoptab->name) {
            if (strcmp(name, devoptab->name) == 0) {
                return (fs_dev_private_t *) devoptab->deviceData;
            }
        }
    }

    return NULL;
}

static char *fs_dev_real_path(const char *path, fs_dev_private_t *dev) {
    // Sanity check
    if (!path)
        return NULL;

    // Move the path pointer to the start of the actual path
    if (strchr(path, ':') != NULL) {
        path = strchr(path, ':') + 1;
    }

    int mount_len = strlen(dev->mount_path);

    char *new_name = (char *) malloc(mount_len + strlen(path) + 1);
    if (new_name) {
        strcpy(new_name, dev->mount_path);
        strcpy(new_name + mount_len, path);
        return new_name;
    }
    return new_name;
}

static int fs_dev_translate_error(FSStatus error) {
    switch ((int) error) {
        case FS_STATUS_END:
            return ENOENT;
        case FS_STATUS_CANCELLED:
            return ECANCELED;
        case FS_STATUS_EXISTS:
            return EEXIST;
        case FS_STATUS_MEDIA_ERROR:
            return EIO;
        case FS_STATUS_NOT_FOUND:
            return ENOENT;
        case FS_STATUS_PERMISSION_ERROR:
            return EPERM;
        case FS_STATUS_STORAGE_FULL:
            return ENOSPC;
        case FS_STATUS_FILE_TOO_BIG:
            return EFBIG;
        case FS_STATUS_NOT_DIR:
            return ENOTDIR;
        case FS_STATUS_NOT_FILE:
            return EISDIR;
        case FS_STATUS_MAX:
            return ENFILE;
        case FS_STATUS_ACCESS_ERROR:
            return EACCES;
        case FS_STATUS_JOURNAL_FULL:
            return ENOSPC;
        case FS_STATUS_UNSUPPORTED_CMD:
            return ENOTSUP;
        case FS_STATUS_MEDIA_NOT_READY:
            return EOWNERDEAD;
        case FS_STATUS_ALREADY_OPEN:
        case FS_STATUS_CORRUPTED:
        case FS_STATUS_FATAL_ERROR:
            return EIO;
    }
    return (int) error;
}

static mode_t fs_dev_translate_mode(FSStat fileStat, bool followLinks, bool isRootDirectory) {
    mode_t retMode = 0;

    // Convert file types
    if (isRootDirectory) {
        retMode |= S_IFDIR;
    } else if ((fileStat.flags & FS_STAT_LINK) == FS_STAT_LINK) {
        retMode |= S_IFLNK;
    } else if ((fileStat.flags & FS_STAT_DIRECTORY) == FS_STAT_DIRECTORY) {
        retMode |= S_IFDIR;
    } else if ((fileStat.flags & FS_STAT_FILE) == FS_STAT_FILE) {
        retMode |= S_IFREG;
    }

    // Convert file mode
    if ((fileStat.mode & FS_MODE_READ_OWNER) == FS_MODE_READ_OWNER) {
        retMode |= S_IRUSR;
    }
    if ((fileStat.mode & FS_MODE_WRITE_OWNER) == FS_MODE_WRITE_OWNER) {
        retMode |= S_IWUSR;
    }
    if ((fileStat.mode & FS_MODE_EXEC_OWNER) == FS_MODE_EXEC_OWNER) {
        retMode |= S_IXUSR;
    }

    if ((fileStat.mode & FS_MODE_READ_GROUP) == FS_MODE_READ_GROUP) {
        retMode |= S_IRGRP;
    }
    if ((fileStat.mode & FS_MODE_WRITE_GROUP) == FS_MODE_WRITE_GROUP) {
        retMode |= S_IWGRP;
    }
    if ((fileStat.mode & FS_MODE_EXEC_GROUP) == FS_MODE_EXEC_GROUP) {
        retMode |= S_IXGRP;
    }

    if ((fileStat.mode & FS_MODE_READ_OTHER) == FS_MODE_READ_OTHER) {
        retMode |= S_IROTH;
    }
    if ((fileStat.mode & FS_MODE_WRITE_OTHER) == FS_MODE_WRITE_OTHER) {
        retMode |= S_IWOTH;
    }
    if ((fileStat.mode & FS_MODE_EXEC_OTHER) == FS_MODE_EXEC_OTHER) {
        retMode |= S_IXOTH;
    }

    return retMode;
}

static time_t fs_dev_translate_time(FSTime timeValue) {
    OSCalendarTime fileTime;
    FSTimeToCalendarTime(timeValue, &fileTime);
    struct tm posixTime = {0};
    posixTime.tm_year   = fileTime.tm_year - 1900;
    posixTime.tm_mon    = fileTime.tm_mon;
    posixTime.tm_mday   = fileTime.tm_mday;
    posixTime.tm_hour   = fileTime.tm_hour;
    posixTime.tm_min    = fileTime.tm_min;
    posixTime.tm_sec    = fileTime.tm_sec;
    posixTime.tm_yday   = fileTime.tm_yday;
    posixTime.tm_wday   = fileTime.tm_wday;
    return mktime(&posixTime);
}

static int fs_dev_open_r(struct _reent *r, void *fileStruct, const char *path, int flags, int mode) {
    // Get device data from path
    fs_dev_private_t *dev = fs_dev_get_device_data(path);
    if (!dev) {
        r->_errno = ENODEV;
        return -1;
    }

    fs_dev_file_state_t *file = (fs_dev_file_state_t *) fileStruct;

    file->dev = dev;
    // Determine which mode the file is opened for
    file->flags = flags;

    // Map flags to open modes
    const char *fsMode;
    if ((flags & 0x03) == O_RDONLY) {
        file->read   = true;
        file->write  = false;
        file->append = false;
        fsMode       = "r";
    } else if ((flags & 0x03) == O_WRONLY) {
        file->read   = false;
        file->write  = true;
        file->append = (flags & O_APPEND);
        fsMode       = file->append ? "a" : "w";
    } else if ((flags & 0x03) == O_RDWR) {
        file->read   = true;
        file->write  = true;
        file->append = (flags & O_APPEND);
        fsMode       = file->append ? "a+" : "w+";
    } else {
        r->_errno = EINVAL;
        return -1;
    }

    OSLockMutex(dev->pMutex);

    char *real_path = fs_dev_real_path(path, dev);
    if (!path) {
        r->_errno = ENOMEM;
        OSUnlockMutex(dev->pMutex);
        return -1;
    }
    strcpy(file->path, real_path);

    int fd = -1;
    int result;
    if ((flags & O_OPEN_ENCRYPTED) == O_OPEN_ENCRYPTED) {
        result = IOSUHAX_FSA_OpenFileEx(dev->fsaFd, real_path, fsMode, &fd, FSA_OPENFLAGS_OPEN_ENCRYPTED, NULL, NULL);
    }
    else {
        result = IOSUHAX_FSA_OpenFile(dev->fsaFd, real_path, fsMode, &fd);
    }
    free(real_path);

    if (result == 0) {
        FSStat stats;
        result = IOSUHAX_FSA_GetStatFile(dev->fsaFd, fd, &stats);
        if (result != 0) {
            IOSUHAX_FSA_CloseFile(dev->fsaFd, fd);
            r->_errno = fs_dev_translate_error(result);
            OSUnlockMutex(dev->pMutex);
            return -1;
        }
        file->fd  = fd;
        file->pos = 0;
        file->len = stats.size;
        OSUnlockMutex(dev->pMutex);
        return (int) file;
    }

    r->_errno = fs_dev_translate_error(result);
    OSUnlockMutex(dev->pMutex);
    return -1;
}


static int fs_dev_close_r(struct _reent *r, void *fd) {
    fs_dev_file_state_t *file = (fs_dev_file_state_t *) fd;
    if (!file->dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(file->dev->pMutex);
    int result = IOSUHAX_FSA_CloseFile(file->dev->fsaFd, file->fd);
    OSUnlockMutex(file->dev->pMutex);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        return -1;
    }

    return 0;
}

static off_t fs_dev_seek_r(struct _reent *r, void *fd, off_t pos, int dir) {
    fs_dev_file_state_t *file = (fs_dev_file_state_t *) fd;
    if (!file->dev) {
        r->_errno = ENODEV;
        return 0;
    }

    OSLockMutex(file->dev->pMutex);

    switch (dir) {
        case SEEK_SET:
            file->pos = pos;
            break;
        case SEEK_CUR:
            file->pos += pos;
            break;
        case SEEK_END:
            file->pos = file->len + pos;
            break;
        default:
            r->_errno = EINVAL;
            return -1;
    }

    int result = IOSUHAX_FSA_SetPosFile(file->dev->fsaFd, file->fd, file->pos);
    OSUnlockMutex(file->dev->pMutex);

    if (result == 0) {
        return file->pos;
    }

    return result;
}

static ssize_t fs_dev_write_r(struct _reent *r, void *fd, const char *ptr, size_t len) {
    fs_dev_file_state_t *file = (fs_dev_file_state_t *) fd;
    if (!file->dev) {
        r->_errno = ENODEV;
        return 0;
    }

    if (!file->write) {
        r->_errno = EACCES;
        return 0;
    }

    OSLockMutex(file->dev->pMutex);

    size_t done = 0;

    while (done < len) {
        size_t write_size = len - done;

        int result = IOSUHAX_FSA_WriteFile(file->dev->fsaFd, ptr + done, 0x01, write_size, file->fd, 0);
        if (result < 0) {
            r->_errno = fs_dev_translate_error(result);
            break;
        } else if (result == 0) {
            if (write_size > 0)
                done = 0;
            break;
        } else {
            done += result;
            file->pos += result;
        }
    }

    OSUnlockMutex(file->dev->pMutex);
    return done;
}

static ssize_t fs_dev_read_r(struct _reent *r, void *fd, char *ptr, size_t len) {
    fs_dev_file_state_t *file = (fs_dev_file_state_t *) fd;
    if (!file->dev) {
        r->_errno = ENODEV;
        return 0;
    }

    if (!file->read) {
        r->_errno = EACCES;
        return 0;
    }

    OSLockMutex(file->dev->pMutex);

    size_t done = 0;
    while (done < len) {
        size_t read_size = len - done;

        int result = IOSUHAX_FSA_ReadFile(file->dev->fsaFd, ptr + done, 0x01, read_size, file->fd, 0);
        if (result < 0) {
            r->_errno = fs_dev_translate_error(result);
            done      = 0;
            break;
        } else if (result == 0) {
            //! TODO: error on read_size > 0
            break;
        } else {
            done += result;
            file->pos += result;
        }
    }

    OSUnlockMutex(file->dev->pMutex);
    return done;
}

static int fs_dev_ftruncate_r(struct _reent *r, void *fd, off_t len) {
    fs_dev_file_state_t *file = (fs_dev_file_state_t *) fd;
    if (!file->dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(file->dev->pMutex);
    int result = IOSUHAX_FSA_TruncateFile(file->dev->fsaFd, file->fd);
    if (result != 0) {
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(file->dev->pMutex);
        return -1;
    }

    OSUnlockMutex(file->dev->pMutex);
    return 0;
}

static int fs_dev_fsync_r(struct _reent *r, void *fd) {
    fs_dev_file_state_t *file = (fs_dev_file_state_t *) fd;
    if (!file->dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(file->dev->pMutex);
    int result = IOSUHAX_FSA_FlushFile(file->dev->fsaFd, file->fd);
    if (result != 0) {
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(file->dev->pMutex);
        return -1;
    }

    OSUnlockMutex(file->dev->pMutex);
    return 0;
}

static int fs_dev_fstat_r(struct _reent *r, void *fd, struct stat *st) {
    fs_dev_file_state_t *file = (fs_dev_file_state_t *) fd;
    if (!file->dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(file->dev->pMutex);

    // Zero out the stat buffer
    memset(st, 0, sizeof(struct stat));

    FSStat stats;
    int result = IOSUHAX_FSA_GetStatFile(file->dev->fsaFd, (int) fd, &stats);
    if (result != 0) {
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(file->dev->pMutex);
        return -1;
    }

    // Convert fields to posix stat
    st->st_dev     = (dev_t) file->dev;
    st->st_ino     = stats.entryId;
    st->st_mode    = fs_dev_translate_mode(stats, true, false);
    st->st_nlink   = 1;
    st->st_uid     = stats.owner;
    st->st_gid     = stats.group;
    st->st_rdev    = st->st_dev;
    st->st_size    = stats.size;
    st->st_blksize = 512;
    st->st_blocks  = (st->st_size + st->st_blksize - 1) / st->st_blksize;
    st->st_atime   = fs_dev_translate_time(stats.modified);
    st->st_ctime   = fs_dev_translate_time(stats.created);
    st->st_mtime   = fs_dev_translate_time(stats.modified);
    OSUnlockMutex(file->dev->pMutex);
    return 0;
}

static int fs_dev_stat_r(struct _reent *r, const char *path, struct stat *st) {
    fs_dev_private_t *dev = fs_dev_get_device_data(path);
    if (!dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dev->pMutex);

    // Zero out the stat buffer
    memset(st, 0, sizeof(struct stat));

    char *real_path = fs_dev_real_path(path, dev);
    if (!real_path) {
        r->_errno = ENOMEM;
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    FSStat stats;
    int result = IOSUHAX_FSA_GetStat(dev->fsaFd, real_path, &stats);
    free(real_path);
    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    // Convert fields to posix stat
    st->st_dev     = (dev_t) dev;
    st->st_ino     = stats.entryId;
    st->st_mode    = fs_dev_translate_mode(stats, true, (strlen(dev->mount_path) + 1 == strlen(real_path)));
    st->st_nlink   = 1;
    st->st_uid     = stats.owner;
    st->st_gid     = stats.group;
    st->st_rdev    = st->st_dev;
    st->st_size    = stats.size;
    st->st_blksize = 512;
    st->st_blocks  = (st->st_size + st->st_blksize - 1) / st->st_blksize;
    st->st_atime   = fs_dev_translate_time(stats.modified);
    st->st_ctime   = fs_dev_translate_time(stats.created);
    st->st_mtime   = fs_dev_translate_time(stats.modified);

    OSUnlockMutex(dev->pMutex);
    return 0;
}

static int fs_dev_lstat_r(struct _reent *r, const char *path, struct stat *st) {
    fs_dev_private_t *dev = fs_dev_get_device_data(path);
    if (!dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dev->pMutex);

    // Zero out the stat buffer
    memset(st, 0, sizeof(struct stat));

    char *real_path = fs_dev_real_path(path, dev);
    if (!real_path) {
        r->_errno = ENOMEM;
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    FSStat stats;
    int result = IOSUHAX_FSA_GetStat(dev->fsaFd, real_path, &stats);
    free(real_path);
    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    // Convert fields to posix stat
    st->st_dev     = (dev_t) dev;
    st->st_ino     = stats.entryId;
    st->st_mode    = fs_dev_translate_mode(stats, false, (strlen(dev->mount_path) + 1 == strlen(real_path)));
    st->st_nlink   = 1;
    st->st_uid     = stats.owner;
    st->st_gid     = stats.group;
    st->st_rdev    = st->st_dev;
    st->st_size    = stats.size;
    st->st_blksize = 512;
    st->st_blocks  = (st->st_size + st->st_blksize - 1) / st->st_blksize;
    st->st_atime   = fs_dev_translate_time(stats.modified);
    st->st_ctime   = fs_dev_translate_time(stats.created);
    st->st_mtime   = fs_dev_translate_time(stats.modified);

    OSUnlockMutex(dev->pMutex);
    return 0;
}

static int fs_dev_link_r(struct _reent *r, const char *existing, const char *newLink) {
    r->_errno = ENOTSUP;
    return -1;
}

static int fs_dev_unlink_r(struct _reent *r, const char *name) {
    fs_dev_private_t *dev = fs_dev_get_device_data(name);
    if (!dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dev->pMutex);

    char *real_path = fs_dev_real_path(name, dev);
    if (!real_path) {
        r->_errno = ENOMEM;
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    int result = IOSUHAX_FSA_Remove(dev->fsaFd, real_path);
    free(real_path);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    OSUnlockMutex(dev->pMutex);
    return 0;
}

static int fs_dev_rmdir_r(struct _reent *r, const char *name) {
    fs_dev_private_t *dev = fs_dev_get_device_data(name);
    if (!dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dev->pMutex);

    char *real_path = fs_dev_real_path(name, dev);
    if (!real_path) {
        r->_errno = ENOMEM;
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    // Check if directory still has files in which case return error
    int dirHandle = 0;
    int result    = 0;
    result        = IOSUHAX_FSA_OpenDir(dev->fsaFd, real_path, &dirHandle);
    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        free(real_path);
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    FSDirectoryEntry *dir_entry = malloc(sizeof(FSDirectoryEntry));
    if (dir_entry == NULL) {
        IOSUHAX_FSA_CloseDir(dev->fsaFd, dirHandle);
        free(real_path);
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    result = IOSUHAX_FSA_ReadDir(dev->fsaFd, dirHandle, dir_entry);
    IOSUHAX_FSA_CloseDir(dev->fsaFd, dirHandle);
    free(dir_entry);
    if (result == FS_STATUS_OK) {
        free(real_path);
        r->_errno = ENOTEMPTY;
        OSUnlockMutex(dev->pMutex);
        return -1;
    } else if (result != FS_STATUS_END) {
        free(real_path);
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    // Delete folder
    result = IOSUHAX_FSA_Remove(dev->fsaFd, real_path);
    free(real_path);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    OSUnlockMutex(dev->pMutex);
    return 0;
}

static int fs_dev_chdir_r(struct _reent *r, const char *name) {
    fs_dev_private_t *dev = fs_dev_get_device_data(name);
    if (!dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dev->pMutex);

    char *real_path = fs_dev_real_path(name, dev);
    if (!real_path) {
        r->_errno = ENOMEM;
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    int result = IOSUHAX_FSA_ChangeDir(dev->fsaFd, real_path);
    free(real_path);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    OSUnlockMutex(dev->pMutex);
    return 0;
}

static int fs_dev_rename_r(struct _reent *r, const char *oldName, const char *newName) {
    fs_dev_private_t *dev = fs_dev_get_device_data(oldName);
    if (!dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dev->pMutex);

    char *real_oldpath = fs_dev_real_path(oldName, dev);
    if (!real_oldpath) {
        r->_errno = ENOMEM;
        OSUnlockMutex(dev->pMutex);
        return -1;
    }
    char *real_newpath = fs_dev_real_path(newName, dev);
    if (!real_newpath) {
        r->_errno = ENOMEM;
        free(real_oldpath);
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    int result = IOSUHAX_FSA_Rename(dev->fsaFd, real_oldpath, real_newpath);
    free(real_oldpath);
    free(real_newpath);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    OSUnlockMutex(dev->pMutex);
    return 0;
}

static int fs_dev_mkdir_r(struct _reent *r, const char *path, int mode) {
    fs_dev_private_t *dev = fs_dev_get_device_data(path);
    if (!dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dev->pMutex);

    char *real_path = fs_dev_real_path(path, dev);
    if (!real_path) {
        r->_errno = ENOMEM;
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    int result = IOSUHAX_FSA_MakeDir(dev->fsaFd, real_path, mode);
    free(real_path);
    OSUnlockMutex(dev->pMutex);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        return -1;
    }

    return 0;
}

static int fs_dev_chmod_r(struct _reent *r, const char *path, mode_t mode) {
    fs_dev_private_t *dev = fs_dev_get_device_data(path);
    if (!dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dev->pMutex);

    char *real_path = fs_dev_real_path(path, dev);
    if (!real_path) {
        r->_errno = ENOMEM;
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    int result = IOSUHAX_FSA_ChangeMode(dev->fsaFd, real_path, mode);
    free(real_path);
    OSUnlockMutex(dev->pMutex);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        return -1;
    }

    return 0;
}

static int fs_dev_fchmod_r(struct _reent *r, void *fd, mode_t mode) {
    fs_dev_file_state_t *file = (fs_dev_file_state_t *) fd;
    if (!file->dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(file->dev->pMutex);

    char *real_path = fs_dev_real_path(file->path, file->dev);
    if (!real_path) {
        r->_errno = ENOMEM;
        OSUnlockMutex(file->dev->pMutex);
        return -1;
    }

    int result = IOSUHAX_FSA_ChangeMode(file->dev->fsaFd, real_path, mode);
    free(real_path);
    OSUnlockMutex(file->dev->pMutex);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        return -1;
    }

    return 0;
}

static int fs_dev_statvfs_r(struct _reent *r, const char *path, struct statvfs *buf) {
    fs_dev_private_t *dev = fs_dev_get_device_data(path);
    if (!dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dev->pMutex);

    // Zero out the stat buffer
    memset(buf, 0, sizeof(struct statvfs));

    char *real_path = fs_dev_real_path(path, dev);
    if (!real_path) {
        r->_errno = ENOMEM;
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    uint64_t deviceSize;
    int result = IOSUHAX_FSA_GetFreeSpaceSize(dev->fsaFd, real_path, &deviceSize);
    free(real_path);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(dev->pMutex);
        return -1;
    }

    // File system block size
    buf->f_bsize = 512;
    // Fundamental file system block size
    buf->f_frsize = 512;
    // Total number of blocks on file system in units of f_frsize
    buf->f_blocks = deviceSize >> 9; // this is unknown
    // Free blocks available for all and for non-privileged processes
    buf->f_bfree = buf->f_bavail = deviceSize >> 9;
    // Number of inodes at this point in time
    buf->f_files = 0xffffffff;
    // Free inodes available for all and for non-privileged processes
    buf->f_ffree = 0xffffffff;
    // File system id
    buf->f_fsid = (int) dev;
    // Bit mask of f_flag values.
    buf->f_flag = 0;
    // Maximum length of filenames
    buf->f_namemax = 255;

    OSUnlockMutex(dev->pMutex);
    return 0;
}

static DIR_ITER *fs_dev_diropen_r(struct _reent *r, DIR_ITER *dirState, const char *path) {
    fs_dev_private_t *dev = fs_dev_get_device_data(path);
    if (!dev) {
        r->_errno = ENODEV;
        return NULL;
    }

    fs_dev_dir_entry_t *dirIter = (fs_dev_dir_entry_t *) dirState->dirStruct;

    OSLockMutex(dev->pMutex);

    char *real_path = fs_dev_real_path(path, dev);
    if (!real_path) {
        r->_errno = ENOMEM;
        OSUnlockMutex(dev->pMutex);
        return NULL;
    }

    int dirHandle;
    int result = IOSUHAX_FSA_OpenDir(dev->fsaFd, real_path, &dirHandle);
    free(real_path);
    OSUnlockMutex(dev->pMutex);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        return NULL;
    }

    dirIter->dev       = dev;
    dirIter->dirHandle = dirHandle;

    return dirState;
}

static int fs_dev_dirclose_r(struct _reent *r, DIR_ITER *dirState) {
    fs_dev_dir_entry_t *dirIter = (fs_dev_dir_entry_t *) dirState->dirStruct;
    if (!dirIter->dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dirIter->dev->pMutex);
    int result = IOSUHAX_FSA_CloseDir(dirIter->dev->fsaFd, dirIter->dirHandle);
    OSUnlockMutex(dirIter->dev->pMutex);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        return -1;
    }
    return 0;
}

static int fs_dev_dirreset_r(struct _reent *r, DIR_ITER *dirState) {
    fs_dev_dir_entry_t *dirIter = (fs_dev_dir_entry_t *) dirState->dirStruct;
    if (!dirIter->dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dirIter->dev->pMutex);
    int result = IOSUHAX_FSA_RewindDir(dirIter->dev->fsaFd, dirIter->dirHandle);
    OSUnlockMutex(dirIter->dev->pMutex);

    if (result < 0) {
        r->_errno = fs_dev_translate_error(result);
        return -1;
    }
    return 0;
}

static int fs_dev_dirnext_r(struct _reent *r, DIR_ITER *dirState, char *filename, struct stat *st) {
    fs_dev_dir_entry_t *dirIter = (fs_dev_dir_entry_t *) dirState->dirStruct;
    if (!dirIter->dev) {
        r->_errno = ENODEV;
        return -1;
    }

    OSLockMutex(dirIter->dev->pMutex);

    FSDirectoryEntry *dir_entry = malloc(sizeof(FSDirectoryEntry));

    int result = IOSUHAX_FSA_ReadDir(dirIter->dev->fsaFd, dirIter->dirHandle, dir_entry);
    if (result < 0) {
        free(dir_entry);
        r->_errno = fs_dev_translate_error(result);
        OSUnlockMutex(dirIter->dev->pMutex);
        return -1;
    }

    // Fetch the current entry
    strcpy(filename, dir_entry->name);

    if (st) {
        memset(st, 0, sizeof(struct stat));

        // Convert fields to posix stat
        st->st_dev     = (dev_t) dirIter->dev;
        st->st_ino     = dir_entry->info.entryId;
        st->st_mode    = fs_dev_translate_mode(dir_entry->info, true, false);
        st->st_nlink   = 1;
        st->st_uid     = dir_entry->info.owner;
        st->st_gid     = dir_entry->info.group;
        st->st_rdev    = st->st_dev;
        st->st_size    = dir_entry->info.size;
        st->st_blksize = 512;
        st->st_blocks  = (st->st_size + st->st_blksize - 1) / st->st_blksize;
        st->st_atime   = fs_dev_translate_time(dir_entry->info.modified);
        st->st_ctime   = fs_dev_translate_time(dir_entry->info.created);
        st->st_mtime   = fs_dev_translate_time(dir_entry->info.modified);
    }

    free(dir_entry);
    OSUnlockMutex(dirIter->dev->pMutex);
    return 0;
}

static const devoptab_t devops_fs = {
        .name         = NULL, /* Device name */
        .structSize   = sizeof(fs_dev_file_state_t),
        .open_r       = fs_dev_open_r,
        .close_r      = fs_dev_close_r,
        .write_r      = fs_dev_write_r,
        .read_r       = fs_dev_read_r,
        .seek_r       = fs_dev_seek_r,
        .fstat_r      = fs_dev_fstat_r,
        .stat_r       = fs_dev_stat_r,
        .link_r       = fs_dev_link_r,
        .unlink_r     = fs_dev_unlink_r,
        .chdir_r      = fs_dev_chdir_r,
        .rename_r     = fs_dev_rename_r,
        .mkdir_r      = fs_dev_mkdir_r,
        .dirStateSize = sizeof(fs_dev_dir_entry_t),
        .diropen_r    = fs_dev_diropen_r,
        .dirreset_r   = fs_dev_dirreset_r,
        .dirnext_r    = fs_dev_dirnext_r,
        .dirclose_r   = fs_dev_dirclose_r,
        .statvfs_r    = fs_dev_statvfs_r,
        .ftruncate_r  = fs_dev_ftruncate_r,
        .fsync_r      = fs_dev_fsync_r,
        .deviceData   = NULL,
        .chmod_r      = fs_dev_chmod_r,
        .fchmod_r     = fs_dev_fchmod_r,
        .rmdir_r      = fs_dev_rmdir_r,
        .lstat_r      = fs_dev_lstat_r,
        .utimes_r     = NULL,
};

static int fs_dev_add_device(const char *name, const char *mount_path, int fsaFd, int isMounted) {
    devoptab_t *dev = NULL;
    char *devname   = NULL;
    char *devpath   = NULL;
    int i;

    // Sanity check
    if (!name) {
        errno = EINVAL;
        return -1;
    }

    // Allocate a devoptab for this device
    dev = (devoptab_t *) malloc(sizeof(devoptab_t) + strlen(name) + 1);
    if (!dev) {
        errno = ENOMEM;
        return -1;
    }

    // Use the space allocated at the end of the devoptab for storing the device name
    devname = (char *) (dev + 1);
    strcpy(devname, name);

    // create private data
    fs_dev_private_t *priv = (fs_dev_private_t *) malloc(sizeof(fs_dev_private_t) + strlen(mount_path) + 1);
    if (!priv) {
        free(dev);
        errno = ENOMEM;
        return -1;
    }

    devpath = (char *) (priv + 1);
    strcpy(devpath, mount_path);

    // setup private data
    priv->mount_path = devpath;
    priv->fsaFd      = fsaFd;
    priv->mounted    = isMounted;
    priv->pMutex     = malloc(OS_MUTEX_SIZE);

    if (!priv->pMutex) {
        free(dev);
        free(priv);
        errno = ENOMEM;
        return -1;
    }

    OSInitMutex(priv->pMutex);

    // Setup the devoptab
    memcpy(dev, &devops_fs, sizeof(devoptab_t));
    dev->name       = devname;
    dev->deviceData = priv;

    // Add the device to the devoptab table (if there is a free slot)
    for (i = 3; i < STD_MAX; i++) {
        if (devoptab_list[i] == devoptab_list[0]) {
            devoptab_list[i] = dev;
            return 0;
        }
    }

    // failure, free all memory
    free(priv);
    free(dev);

    // If we reach here then there are no free slots in the devoptab table for this device
    errno = EADDRNOTAVAIL;
    return -1;
}

static int fs_dev_remove_device(const char *path) {
    const devoptab_t *devoptab = NULL;
    char name[128]             = {0};
    int i;

    // Get the device name from the path
    strncpy(name, path, 127);
    strtok(name, ":/");

    // Find and remove the specified device from the devoptab table
    // NOTE: We do this manually due to a 'bug' in RemoveDevice
    //       which ignores names with suffixes and causes names
    //       like "ntfs" and "ntfs1" to be seen as equals
    for (i = 3; i < STD_MAX; i++) {
        devoptab = devoptab_list[i];
        if (devoptab && devoptab->name) {
            if (strcmp(name, devoptab->name) == 0) {
                devoptab_list[i] = devoptab_list[0];

                if (devoptab->deviceData) {
                    fs_dev_private_t *priv = (fs_dev_private_t *) devoptab->deviceData;

                    if (priv->mounted)
                        IOSUHAX_FSA_Unmount(priv->fsaFd, priv->mount_path, 2);

                    if (priv->pMutex)
                        free(priv->pMutex);
                    free(devoptab->deviceData);
                }

                free((devoptab_t *) devoptab);
                return 0;
            }
        }
    }

    return -1;
}

int mount_fs(const char *virt_name, int fsaFd, const char *dev_path, const char *mount_path) {
    int isMounted = 0;

    if (dev_path) {
        isMounted = 1;

        int res = IOSUHAX_FSA_Mount(fsaFd, dev_path, mount_path, 2, 0, 0);
        if (res != 0) {
            return res;
        }
    }

    return fs_dev_add_device(virt_name, mount_path, fsaFd, isMounted);
}

int unmount_fs(const char *virt_name) {
    return fs_dev_remove_device(virt_name);
}
