#ifndef RVOS_RVFS_H
#define RVOS_RVFS_H

#include "types.h"
#include "disk.h"

#define RVFS_NAME_MAX 63

/* Error codes (all negative). */
#define RVFS_OK         0
#define RVFS_ENOENT    -1
#define RVFS_EIO       -2
#define RVFS_ENOSPC    -3
#define RVFS_EEXIST    -4
#define RVFS_ENOTDIR   -5
#define RVFS_EISDIR    -6
#define RVFS_EFBIG     -7
#define RVFS_EINVAL    -8
#define RVFS_ENOTEMPTY -9
#define RVFS_EROFS    -10

typedef struct {
    char name[RVFS_NAME_MAX + 1];
    u32 ino;
    u32 size;
    u8 type;            /* 1 = file, 2 = directory */
} RvfsEntry;

typedef struct {
    u8 type;
    u32 ino;
    u32 size;
    u32 blocks;
    u32 links;
} RvfsStat;

typedef struct {
    char label[32];
    u32 block_size;
    u32 total_blocks;
    u32 free_blocks;
    u32 inode_count;
    u32 free_inodes;
} RvfsVolume;

int  rvfs_mount(const DiskPart *part);
void rvfs_unmount(void);
int  rvfs_mounted(void);
int  rvfs_mounted_on(const DiskPart *part);
const RvfsVolume *rvfs_volume(void);

int  rvfs_format(const DiskPart *part, const char *label);

int  rvfs_stat(const char *path, RvfsStat *out);
int  rvfs_readdir(const char *path, u32 index, RvfsEntry *out);
int  rvfs_read(const char *path, u32 offset, void *buf, u32 len);
int  rvfs_write(const char *path, u32 offset, const void *buf, u32 len);
int  rvfs_create(const char *path);
int  rvfs_mkdir(const char *path);
int  rvfs_unlink(const char *path);
int  rvfs_sync(void);

#endif
