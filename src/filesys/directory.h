/* directory.h */
#ifndef FILESYS_DIRECTORY_H
#define FILESYS_DIRECTORY_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"

#define NAME_MAX 14   /* Maksimum dosya adı uzunluğu (14 karakter + '\0') */

struct inode;
struct file;

bool dir_create(block_sector_t sector, size_t entry_cnt);

bool dir_lookup(struct inode *dir_inode, const char *name, struct inode **result_inode);
bool dir_add(struct inode *dir_inode, const char *name, block_sector_t inode_sector);
bool dir_remove(struct inode *dir_inode, const char *name);
bool dir_readdir(struct file *dir_file, char name[NAME_MAX + 1]);
bool dir_is_empty(struct inode *dir_inode);

#endif /* filesys/directory.h */
