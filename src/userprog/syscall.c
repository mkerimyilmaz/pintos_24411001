#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "filesys/filesys.h"

typedef int pid_t;
static int (*syscall_handlers[20]) (struct intr_frame *);

static int
read_user_byte(const uint8_t *uaddr)
{
  if (!is_user_vaddr(uaddr))
    return -1;
  int result;
  asm("movl $1f, %0; movzbl %1, %0; 1:"
      : "=&a" (result) : "m" (*uaddr));
  return result;
}

static bool
write_user_byte(uint8_t *udst, uint8_t byte)
{
  if (!is_user_vaddr(udst))
    return false;
  int error_code;
  asm("movl $1f, %0; movb %b2, %1; 1:"
      : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}

static bool is_valid_pointer(void *esp, uint8_t argc) {
  for (uint8_t i = 0; i < argc; ++i) {
    if (read_user_byte(((uint8_t *)esp) + i) == -1)
      return false;
  }
  return true;
}

static bool is_valid_string(void *str) {
  int ch;
  while ((ch = read_user_byte((uint8_t *)str++)) != '\0' && ch != -1);
  return ch == '\0';
}

static void syscall_exit(int status) {
  thread_exit(status);
}

static void syscall_halt(void) {
  shutdown();
}

static pid_t syscall_exec(const char *file_name) {
  return process_execute(file_name);
}

static int syscall_wait(pid_t pid) {
  return process_wait(pid);
}

static bool syscall_create(const char *file_name, unsigned initial_size) {
  return filesys_create(file_name, initial_size);
}

static bool syscall_remove(const char *file_name) {
  return filesys_remove(file_name);
}

static int syscall_filesize(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 4)) return -1;
  int fd = *(int *)(f->esp + 4);
  f->eax = process_file_length(fd);
  return 0;
}

static int syscall_seek(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 8)) return -1;
  int fd = *(int *)(f->esp + 4);
  unsigned pos = *(unsigned *)(f->esp + 8);
  process_seek(fd, pos);
  return 0;
}

static int syscall_tell(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 4)) return -1;
  int fd = *(int *)(f->esp + 4);
  f->eax = process_file_position(fd);
  return 0;
}

static int syscall_create_wrapper(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 4) ||
      !is_valid_string(*(char **)(f->esp + 4)) ||
      !is_valid_pointer(f->esp + 8, 4)) return -1;
  char *str = *(char **)(f->esp + 4);
  unsigned size = *(int *)(f->esp + 8);
  f->eax = syscall_create(str, size);
  return 0;
}

static int syscall_remove_wrapper(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 4) || !is_valid_string(*(char **)(f->esp + 4))) return -1;
  char *str = *(char **)(f->esp + 4);
  f->eax = syscall_remove(str);
  return 0;
}

static int syscall_open(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 4) || !is_valid_string(*(char **)(f->esp + 4))) return -1;
  char *str = *(char **)(f->esp + 4);
  f->eax = process_open(str);
  return 0;
}

static int syscall_close(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 4)) return -1;
  int fd = *(int *)(f->esp + 4);
  process_close(fd);
  return 0;
}

static int syscall_exit_wrapper(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 4)) return -1;
  int status = *((int *)f->esp + 1);
  syscall_exit(status);
  return 0;
}

static int syscall_halt_wrapper(struct intr_frame *f UNUSED) {
  syscall_halt();
  return 0;
}

static int syscall_wait_wrapper(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 4)) return -1;
  pid_t pid = *((int *)f->esp + 1);
  f->eax = syscall_wait(pid);
  return 0;
}

static int syscall_exec_wrapper(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 4) || !is_valid_string(*(char **)(f->esp + 4))) return -1;
  char *str = *(char **)(f->esp + 4);
  if (strlen(str) >= PGSIZE || strlen(str) == 0 || str[0] == ' ') return -1;
  f->eax = syscall_exec(str);
  return 0;
}

static int syscall_write(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 12)) return -1;
  int fd = *(int *)(f->esp + 4);
  void *buffer = *(char **)(f->esp + 8);
  unsigned size = *(unsigned *)(f->esp + 12);
  if (!is_valid_pointer(buffer, 1) || !is_valid_pointer(buffer + size, 1)) return -1;
  f->eax = process_write(fd, buffer, size);
  return 0;
}

static int syscall_read(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp + 4, 12)) return -1;
  int fd = *(int *)(f->esp + 4);
  void *buffer = *(char **)(f->esp + 8);
  unsigned size = *(unsigned *)(f->esp + 12);
  if (!is_valid_pointer(buffer, 1) || !is_valid_pointer(buffer + size, 1)) return -1;
  f->eax = process_read(fd, buffer, size);
  return 0;
}

static void terminate_program(void) {
  thread_exit(-1);
}

static void syscall_handler(struct intr_frame *f) {
  if (!is_valid_pointer(f->esp, 4)) {
    terminate_program();
    return;
  }

  int syscall_num = *(int *)f->esp;
  if (syscall_num < 0 || syscall_num >= 20) {
    terminate_program();
    return;
  }

  int result = -1;
  switch (syscall_num) {
    case SYS_EXIT:     result = syscall_exit_wrapper(f); break;
    case SYS_WRITE:    result = syscall_write(f); break;
    case SYS_EXEC:     result = syscall_exec_wrapper(f); break;
    case SYS_HALT:     result = syscall_halt_wrapper(f); break;
    case SYS_WAIT:     result = syscall_wait_wrapper(f); break;
    case SYS_CREATE:   result = syscall_create_wrapper(f); break;
    case SYS_REMOVE:   result = syscall_remove_wrapper(f); break;
    case SYS_OPEN:     result = syscall_open(f); break;
    case SYS_CLOSE:    result = syscall_close(f); break;
    case SYS_READ:     result = syscall_read(f); break;
    case SYS_FILESIZE: result = syscall_filesize(f); break;
    case SYS_SEEK:     result = syscall_seek(f); break;
    case SYS_TELL:     result = syscall_tell(f); break;
    default:           terminate_program(); return;
  }

  if (result == -1)
    terminate_program();
}

void syscall_init(void) {
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}
