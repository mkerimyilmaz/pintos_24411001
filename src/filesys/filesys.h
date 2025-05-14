#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#define MAX_OPEN_FILES 128

#define  FREE_MAP_SECTOR 0   
#define ROOT_DIR_SECTOR 1


void filesys_init(bool format);
void filesys_done(void);
bool filesys_create(const char *path, off_t initial_size);
struct file *filesys_open(const char *path);
bool filesys_remove(const char *path);
bool filesys_chdir(const char *path);
bool filesys_mkdir(const char *path, off_t initial_size);
struct block * get_fs_device(void);
extern struct block *fs_device;
int fd_open(const char *path, bool deny_write);
off_t fd_size(int fd);
off_t fd_read(int fd, void *buffer, off_t size);
off_t fd_write(int fd, const void *buffer, off_t size);
void fd_seek(int fd, off_t new_pos);
off_t fd_tell(int fd);
void fd_close(int fd);
bool fd_readdir(int fd, char *name);
bool fd_is_dir(int fd);
block_sector_t fd_inumber(int fd);
struct file *fd_get_file(int fd);
#endif /* filesys/filesys.h */
