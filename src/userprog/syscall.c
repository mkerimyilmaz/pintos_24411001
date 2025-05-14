#include "userprog/syscall.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/directory.h"
#include "devices/shutdown.h"
#include "devices/block.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/pte.h"
#include "vm/frame_table.h"
#include "vm/memory.h"

#include <stdio.h>
#include <syscall-nr.h>

#define ARG(ptr, pos, type, var) \
  if (!get_int_arg((ptr), (pos), (int *)&(var))) thread_exit()

#define ARG_PTR(ptr, pos, var) \
  if (!get_int_arg((ptr), (pos), (int *)&(var)) || !is_user_vaddr((var))) thread_exit()

#define ARG_STR(ptr, pos, var) \
  if (!get_str_arg((ptr), (pos), &(var))) thread_exit()

static void syscall_handler(struct intr_frame *);

static int get_user(const uint8_t *uaddr) {
  return is_user_vaddr(uaddr) ? *uaddr : -1;
}

static bool read_int(const uint8_t *uaddr, int *pi) {
  int bytes[4];
  for (int i = 0; i < 4; ++i) {
    bytes[i] = get_user(uaddr + i);
    if (bytes[i] == -1) return false;
  }
  *pi = (uint8_t)bytes[0] | (uint8_t)bytes[1] << 8 |
        (uint8_t)bytes[2] << 16 | (uint8_t)bytes[3] << 24;
  return true;
}

static bool get_int_arg(const uint8_t *uaddr, int pos, int *pi) {
  return read_int(uaddr + sizeof(int) * pos, pi);
}

static bool get_str_arg(const uint8_t *uaddr, int pos, char **pstr) {
  if (!get_int_arg(uaddr, pos, (int *)pstr)) return false;
  uint8_t *ustr = (uint8_t *)*pstr;
  int ch;
  while ((ch = get_user(ustr++)) != -1) {
    if (ch == 0) return true;
  }
  return false;
}

static bool lock_buffer(const void *buffer, off_t size, bool write) {
  struct thread *cur = thread_current();
  size_t num_pages = (pg_round_down(buffer + size) - pg_round_down(buffer)) / PGSIZE + 1;
  void *upage = pg_round_down(buffer);
  size_t i;

  grow_stack(cur->pagedir, buffer);
  for (i = 0; i < num_pages; ++i, upage += PGSIZE) {
    grow_stack(cur->pagedir, upage);
    if (!frametable_lock_frame(cur->pagedir, upage, write)) break;
  }

  if (i < num_pages) {
    for (upage = pg_round_down(buffer), i = 0; i < num_pages; ++i, upage += PGSIZE)
      frametable_unlock_frame(cur->pagedir, upage);
    return false;
  }

  return true;
}

static void unlock_buffer(const void *buffer, off_t size) {
  size_t num_pages = (pg_round_down(buffer + size) - pg_round_down(buffer)) / PGSIZE + 1;
  void *upage = pg_round_down(buffer);
  for (size_t i = 0; i < num_pages; ++i, upage += PGSIZE)
    frametable_unlock_frame(thread_current()->pagedir, upage);
}

void syscall_init(void) {
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}


// System Call Implementations

int sys_halt(const uint8_t *arg_base) {
  shutdown_power_off();
  NOT_REACHED();
  return 0;
}

int sys_exit(const uint8_t *arg_base) {
  int status;
  ARG(arg_base, 0, int, status);
  thread_current()->exit_status = status;
  thread_exit();
  NOT_REACHED();
  return 0;
}

int sys_exec(const uint8_t *arg_base) {
  char *cmd_line;
  ARG_STR(arg_base, 0, cmd_line);
  return process_execute(cmd_line);
}

int sys_wait(const uint8_t *arg_base) {
  tid_t tid;
  ARG(arg_base, 0, tid_t, tid);
  return process_wait(tid);
}

int sys_create(const uint8_t *arg_base) {
  char *path;
  off_t size;
  ARG_STR(arg_base, 0, path);
  ARG(arg_base, 1, off_t, size);
  return filesys_create(path, size);
}

int sys_remove(const uint8_t *arg_base) {
  char *path;
  ARG_STR(arg_base, 0, path);
  return filesys_remove(path);
}

int sys_open(const uint8_t *arg_base) {
  char *path;
  ARG_STR(arg_base, 0, path);
  return fd_open(path, false);
}

int sys_filesize(const uint8_t *arg_base) {
  int fd;
  ARG(arg_base, 0, int, fd);
  return fd_size(fd);
}

int sys_read(const uint8_t *arg_base) {
  int fd;
  void *buffer;
  off_t size;
  ARG(arg_base, 0, int, fd);
  ARG_PTR(arg_base, 1, buffer);
  ARG(arg_base, 2, off_t, size);

  if (!lock_buffer(buffer, size, true)) thread_exit();
  off_t result = fd_read(fd, buffer, size);
  unlock_buffer(buffer, size);
  return result;
}

int sys_write(const uint8_t *arg_base) {
  int fd;
  const void *buffer;
  off_t size;
  ARG(arg_base, 0, int, fd);
  ARG_PTR(arg_base, 1, buffer);
  ARG(arg_base, 2, off_t, size);

  if (!lock_buffer(buffer, size, false)) thread_exit();
  off_t result = fd_write(fd, buffer, size);
  unlock_buffer(buffer, size);
  return result;
}

int sys_seek(const uint8_t *arg_base) {
  int fd;
  off_t pos;
  ARG(arg_base, 0, int, fd);
  ARG(arg_base, 1, off_t, pos);
  fd_seek(fd, pos);
  return 0;
}

int sys_tell(const uint8_t *arg_base) {
  int fd;
  ARG(arg_base, 0, int, fd);
  return fd_tell(fd);
}

int sys_close(const uint8_t *arg_base) {
  int fd;
  ARG(arg_base, 0, int, fd);
  fd_close(fd);
  return 0;
}

int sys_mmap(const uint8_t *arg_base) {
  int fd;
  void *addr;
  ARG(arg_base, 0, int, fd);
  ARG_PTR(arg_base, 1, addr);
  return mmap(fd, addr);
}

int sys_munmap(const uint8_t *arg_base) {
  int mapid;
  ARG(arg_base, 0, int, mapid);
  munmap(mapid);
  return 0;
}

int sys_chdir(const uint8_t *arg_base) {
  char *path;
  ARG_STR(arg_base, 0, path);
  return filesys_chdir(path);
}

int sys_mkdir(const uint8_t *arg_base) {
  char *path;
  ARG_STR(arg_base, 0, path);
  return filesys_mkdir(path, 0);
}

int sys_readdir(const uint8_t *arg_base) {
  int fd;
  void *name;
  ARG(arg_base, 0, int, fd);
  ARG_PTR(arg_base, 1, name);
  return fd_readdir(fd, name);
}

int sys_isdir(const uint8_t *arg_base) {
  int fd;
  ARG(arg_base, 0, int, fd);
  return fd_is_dir(fd);
}

int sys_inumber(const uint8_t *arg_base) {
  int fd;
  ARG(arg_base, 0, int, fd);
  return fd_inumber(fd);
}
static void syscall_handler(struct intr_frame *f) {
  int syscall_num;
  thread_current()->user_esp = f->esp;

  if (!get_int_arg(f->esp, 0, &syscall_num)) thread_exit();

  static int (*syscalls[])(const uint8_t *) = {
    [SYS_HALT]    = sys_halt,
    [SYS_EXIT]    = sys_exit,
    [SYS_EXEC]    = sys_exec,
    [SYS_WAIT]    = sys_wait,
    [SYS_CREATE]  = sys_create,
    [SYS_REMOVE]  = sys_remove,
    [SYS_OPEN]    = sys_open,
    [SYS_FILESIZE]= sys_filesize,
    [SYS_READ]    = sys_read,
    [SYS_WRITE]   = sys_write,
    [SYS_SEEK]    = sys_seek,
    [SYS_TELL]    = sys_tell,
    [SYS_CLOSE]   = sys_close,
    [SYS_MMAP]    = sys_mmap,
    [SYS_MUNMAP]  = sys_munmap,
    [SYS_CHDIR]   = sys_chdir,
    [SYS_MKDIR]   = sys_mkdir,
    [SYS_READDIR] = sys_readdir,
    [SYS_ISDIR]   = sys_isdir,
    [SYS_INUMBER] = sys_inumber
  };

  if (syscall_num >= 0 && syscall_num < sizeof(syscalls)/sizeof(*syscalls) && syscalls[syscall_num])
    f->eax = syscalls[syscall_num]((uint8_t *)f->esp + sizeof(int));
  else
    f->eax = -1;
}
