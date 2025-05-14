#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache_manager.h"
#include "filesys/path.h"
#include "threads/thread.h"
#include "devices/input.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);


struct block * 
get_fs_device() {
    return fs_device;
}
/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  struct inode *dinode;
  
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  initialize_cache ();
  inode_init();
  free_map_init();
  if (format) {
      do_format();
  }
  free_map_open();
  /* Root dizine özel başlangıç girişleri oluştur */
  struct inode *root_inode = inode_open(ROOT_DIR_SECTOR);
  inode_lock(root_inode);
  dir_add(root_inode, ".", ROOT_DIR_SECTOR);
  dir_add(root_inode, "..", ROOT_DIR_SECTOR);
  inode_unlock(root_inode);
  thread_current()->cwd = root_inode;  
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  shutdown_cache ();
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create(const char *path, off_t initial_size) {
    if (path == NULL || *path == '\0') {
        return false;  /* Boş yol geçersiz */
    }
    if (path_has_trailing_slash(path)) {
        return false;  /* Sonda '/' varsa, bir dosya ismi eksik demektir */
    }

    /* Parent dizini bul ve yeni dosya ismini al */
    char file_name[NAME_MAX + 1];
    struct inode *parent_inode = path_lookup_parent(path, file_name);
    if (parent_inode == NULL) {
        return false;  /* Yol çözümlenemedi veya parent yok */
    }

    bool success = false;
    block_sector_t new_sector = 0;
    inode_lock(parent_inode);
 
    if (free_map_allocate(1, &new_sector)) {
        if (inode_create(new_sector, initial_size, false) 
            && dir_add(parent_inode, file_name, new_sector)) {
            success = true; 
        } else {
            free_map_release(new_sector, 1);
        }
    }
    inode_unlock(parent_inode);
    inode_close(parent_inode);
  return success;
}

/* Opens a file or directory with the given PATH.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file or directory named PATH exists,
   or if an internal memory allocation fails. */
struct file *
   filesys_open(const char *path) {
       if (path == NULL || *path == '\0') {
           return NULL;  
       }
       struct inode *inode = path_lookup(path);
       if (inode == NULL) {
           return NULL;  
       }

       if (path_has_trailing_slash(path) && !inode_is_dir(inode)) {
           inode_close(inode);
           return NULL;
       }
       return file_open(inode);
}

/* Deletes a file or directory named PATH.
   Returns true if successful, false on failure.
   Fails if no file or directory named PATH exists,
   if the directory being deleted is not empty,
   or if an internal memory allocation fails. */
bool
filesys_remove(const char *path) {
    if (path == NULL || *path == '\0') {
        return false;
    }
    if (path_has_trailing_slash(path)) {
        return false;
    }

    char target_name[NAME_MAX + 1];
    struct inode *parent_inode = path_lookup_parent(path, target_name);
    if (parent_inode == NULL) {
        return false;  
    }
    if (!strcmp(target_name, ".") || !strcmp(target_name, "..")) {
        inode_close(parent_inode);
        return false; 
    }

    bool success = false;
    struct inode *target_inode = NULL;
    inode_lock(parent_inode);
  
    if (dir_lookup(parent_inode, target_name, &target_inode)) {
        bool is_directory_flag = inode_is_dir(target_inode);
        if (is_directory_flag) {
            inode_lock(target_inode);
            if (!dir_is_empty(target_inode)) {
                inode_unlock(target_inode);
                goto remove_done;
            }
        }
        if (dir_remove(parent_inode, target_name)) {
            inode_remove(target_inode);
            success = true;
        }
        if (is_directory_flag) {
            inode_unlock(target_inode);
        }
    }
remove_done:
    inode_unlock(parent_inode);
    if (target_inode) {
        inode_close(target_inode);
    }
    inode_close(parent_inode);
    return success;
}

static int
allocate_fd(struct file *file) {
    struct thread *cur = thread_current();
    for (int fd = 2; fd < MAX_OPEN_FILES; ++fd) {  
        if (cur->ofiles[fd] == NULL) {
            cur->ofiles[fd] = file;
            return fd;
        }
    }
    return -1;  
}


int
fd_open(const char *path, bool deny_write) {
    struct file *file = filesys_open(path);
    if (file == NULL) {
        return -1;
    }
    if (deny_write) {
        file_deny_write(file);
    }
    int fd = allocate_fd(file);
    if (fd == -1) {
        file_close(file);
    }
    return fd;
}


off_t
fd_size(int fd) {
    struct file *file = fd_get_file(fd);
    if (file != NULL && !file_is_dir(file)) {
        return file_length(file);
    }
    return -1;
}


off_t
fd_read(int fd, void *buffer_, off_t size) {
    uint8_t *buffer = buffer_;
    off_t bytes_read = 0;
    if (fd == STDIN_FILENO) {
        while (bytes_read < size) {
            char c = input_getc();
            buffer[bytes_read++] = c;
            if (c == '\n') break;  
        }
        return bytes_read;
    }
    struct file *file = fd_get_file(fd);
    if (file != NULL && !file_is_dir(file)) {
        bytes_read = file_read(file, buffer, size);
    } else {
        bytes_read = -1;
    }
    return bytes_read;
}

off_t
fd_write(int fd, const void *buffer, off_t size) {
    if (fd == STDOUT_FILENO) {
        putbuf(buffer, size);
        return size;
    }
    struct file *file = fd_get_file(fd);
    if (file != NULL && !file_is_dir(file)) {
        return file_write(file, buffer, size);
    }
    return -1;
}

void
fd_seek(int fd, off_t new_pos) {
    struct file *file = fd_get_file(fd);
    if (file != NULL && !file_is_dir(file)) {
        file_seek(file, new_pos);
    }
}


off_t
fd_tell(int fd) {
    struct file *file = fd_get_file(fd);
    if (file != NULL && !file_is_dir(file)) {
        return file_tell(file);
    }
    return -1;
}

void
fd_close(int fd) {
    struct thread *cur = thread_current();
    struct file *file = fd_get_file(fd);
    if (file != NULL) {
        file_allow_write(file);
        file_close(file);
        cur->ofiles[fd] = NULL;
    }
}


bool
filesys_chdir(const char *path) {
    if (path == NULL || *path == '\0') {
        return false;
    }
    struct inode *inode = path_lookup(path);
    if (inode == NULL) {
        return false;
    }
    if (!inode_is_dir(inode)) {
        inode_close(inode);
        return false;
    }
    struct thread *cur = thread_current();
    if (cur->cwd != NULL) {
        inode_close(cur->cwd);
    }
    cur->cwd = inode;
    return true;
}

bool
filesys_mkdir(const char *path, off_t initial_size) {
    if (path == NULL || *path == '\0') {
        return false;
    }
    char dir_name[NAME_MAX + 1];
    struct inode *parent_inode = path_lookup_parent(path, dir_name);
    if (parent_inode == NULL) {
        return false;
    }

    bool success = false;
    block_sector_t sector = 0;
    struct inode *inode = NULL;
    inode_lock(parent_inode);
    if (free_map_allocate(1, &sector)) {
        if (inode_create(sector, initial_size, true)) {
            inode = inode_open(sector);
            if (inode != NULL) {
                if (dir_add(inode, ".", sector)
                    && dir_add(inode, "..", inode_get_inumber(parent_inode))
                    && dir_add(parent_inode, dir_name, sector)) {
                    success = true;
                } else {
                    dir_remove(parent_inode, dir_name);
                    inode_remove(inode);
                }
            }
        } 
        if (!success) {
            free_map_release(sector, 1);
        }
    }
    inode_unlock(parent_inode);
    if (inode != NULL) {
        inode_close(inode);
    }
    inode_close(parent_inode);
    return success;
}

bool
fd_readdir (int fd, char *name)
{
  struct file *file;
  bool success = false;

  file = fd_get_file (fd);
  if (file != NULL && file_is_dir (file))
    success = dir_readdir (file, name);
  return success;
}

bool
fd_is_dir (int fd)
{
  struct file *file;
  bool is_directory_flag = false;

  file = fd_get_file (fd);
  if (file != NULL)
    is_directory_flag = file_is_dir (file);
  return is_directory_flag;
}

block_sector_t
fd_inumber (int fd)
{
  struct file *file;
  block_sector_t inumber = 0;

  file = fd_get_file (fd);
  if (file != NULL)
    inumber = inode_get_inumber (file_get_inode (file));
  return inumber;
}

struct file *
fd_get_file (int fd)
{
  struct thread *cur = thread_current ();
  
  if (fd < 2 || fd >= MAX_OPEN_FILES)
    return NULL;
  return cur->ofiles[fd];
}


/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
