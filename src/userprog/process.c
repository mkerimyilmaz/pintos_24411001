#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"


#define MAX_ARGUMENT_COUNT 12

static thread_func start_process NO_RETURN;
static bool load (const char *commandLine, void (**entryPoint) (void), void **stackPointer);

static void extract_command_name(char *commandString, char *commandName);
static void extract_command_arguments(char *commandString, char* argumentVector[], int *argumentCount);

void process_close_all(void);

struct write_entry {
    int ticket;
    char *pipe_name;
    int message;
    struct list_elem elem;
};

struct read_entry {
    int ticket;
    char *pipe_name;
    struct semaphore sema;  
    struct list_elem elem;
};

static struct list writeList;
static struct list readList;


struct process_pid {
  int pid;
  struct list_elem elem;
};

void process_init(void)
{
  list_init(&thread_current()->thread_prog.childThreads);
}

pid_t process_execute (const char *file_name) 
{
  char *fileNameCopy;
  pid_t pid;

  fileNameCopy = palloc_get_page(0);
  if (fileNameCopy == NULL)
    return TID_ERROR;
  strlcpy(fileNameCopy, file_name, PGSIZE);

  char *commandName = malloc(strlen(fileNameCopy) + 1);
  if (commandName == NULL)
    return TID_ERROR;
  extract_command_name(fileNameCopy, commandName);

  pid = thread_create(file_name, PRI_DEFAULT, start_process, fileNameCopy);
  if (pid == TID_ERROR) {
    palloc_free_page(fileNameCopy);
    free(commandName);
    return -1;
  }
  
  struct thread *newThread = get_thread(pid);
  newThread->thread_prog.nextFileDescriptor = 2;
  newThread->thread_prog.programName = commandName;
  list_init(&newThread->thread_prog.fileDescriptorTable);
  list_init(&newThread->thread_prog.childThreads);


  return -1;
}

static void start_process (void *file_name_)
{
  char *file_name = file_name_;

  struct intr_frame if_;
  bool success;

  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load(file_name, &if_.eip, &if_.esp);

  palloc_free_page(file_name);

  if (!success) {
    thread_exit(-1);
  }

  /* Start the user process by simulating a return from an interrupt.
     Jump to intr_exit to start user execution. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED();
}

/* Checks whether the current process is parent of a given pid. */
static bool process_is_parent_of(pid_t pid) {
  struct list_elem *e; 
  for (e = list_begin(&thread_current()->thread_prog.childThreads);
       e != list_end(&thread_current()->thread_prog.childThreads);
       e = list_next(e)) {
    if (list_entry(e, struct process_pid, elem)->pid == pid)
      return true;
  }
  return false;
}

static void remove_child(pid_t pid) {
  struct list_elem *e = NULL; 
  for (e = list_begin(&thread_current()->thread_prog.childThreads);
       e != list_end(&thread_current()->thread_prog.childThreads);
       e = list_next(e)) {
    if (list_entry(e, struct process_pid, elem)->pid == pid)
      break;
  }
  if (e != NULL)
    list_remove(e);
}

int process_wait(tid_t child_tid) {
  struct thread *currentThread = thread_current();
  struct list_elem *e;
  struct process_pid *childEntry = NULL;
  
  /* Find the child process in the current thread's child list. */
  for (e = list_begin(&currentThread->thread_prog.childThreads);
       e != list_end(&currentThread->thread_prog.childThreads);
       e = list_next(e)) {
    struct process_pid *entry = list_entry(e, struct process_pid, elem);
    if (entry->pid == child_tid) {
      childEntry = entry;
      break;
    }
  }
  
  if (childEntry == NULL) {
    return -1;
  }
  

  list_remove(e);
  free(childEntry);
  
  return 0;  }

/* Frees the current process's resources. */
void process_exit (int status) {
  struct thread *cur = thread_current();
  uint32_t *pd = cur->pagedir;

  if (pd != NULL) {
    cur->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }
}

void process_activate (void) {
  struct thread *t = thread_current();
  pagedir_activate(t->pagedir);
  tss_update();
}

/* ================= ELF Definitions ================= */

typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

#define PE32Wx PRIx32
#define PE32Ax PRIx32
#define PE32Ox PRIx32
#define PE32Hx PRIx16

struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half    e_type;
  Elf32_Half    e_machine;
  Elf32_Word    e_version;
  Elf32_Addr    e_entry;
  Elf32_Off     e_phoff;
  Elf32_Off     e_shoff;
  Elf32_Word    e_flags;
  Elf32_Half    e_ehsize;
  Elf32_Half    e_phentsize;
  Elf32_Half    e_phnum;
  Elf32_Half    e_shentsize;
  Elf32_Half    e_shnum;
  Elf32_Half    e_shstrndx;
};

struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off  p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_STACK   0x6474e551

#define PF_X 1
#define PF_W 2
#define PF_R 4

bool setup_stack(void **stack_ptr, const char **argument_vector, int argument_count);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t offset, uint8_t *userPage,
                          uint32_t readBytes, uint32_t zeroBytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load (const char *file_name, void (**entryPoint) (void), void **stackPointer) 
{
  struct thread *t = thread_current();
  struct Elf32_Ehdr elfHeader;
  struct file *file = NULL;
  off_t fileOffset;
  bool success = false;
  int i;

  t->pagedir = pagedir_create();
  if (t->pagedir == NULL)
    goto done;
  process_activate();
  
  char separator[] = " ";
  char *arguments[MAX_ARGUMENT_COUNT];
  arguments[0] = strtok_r((char *)file_name, separator, &file_name);
  i = 1;
  char *token;
  while ((token = strtok_r(file_name, separator, &file_name)) != NULL) {
    printf("Argument: %s\n", token);
    arguments[i++] = token;
    if (i >= MAX_ARGUMENT_COUNT)
      break;
  }
  
  file = filesys_open(arguments[0]);
  if (file == NULL) {
      printf("load: %s: open failed\n", arguments[0]);
      goto done; 
  }

  if (file_read(file, &elfHeader, sizeof elfHeader) != sizeof elfHeader ||
      memcmp(elfHeader.e_ident, "\177ELF\1\1\1", 7) ||
      elfHeader.e_type != 2 ||
      elfHeader.e_machine != 3 ||
      elfHeader.e_version != 1 ||
      elfHeader.e_phentsize != sizeof (struct Elf32_Phdr) ||
      elfHeader.e_phnum > 1024) {
      printf("load: %s: error loading executable\n", arguments[0]);
      goto done; 
  }

  fileOffset = elfHeader.e_phoff;
  for (i = 0; i < elfHeader.e_phnum; i++) {
      struct Elf32_Phdr programHeader;

      if (fileOffset < 0 || fileOffset > file_length(file))
          goto done;
      file_seek(file, fileOffset);

      if (file_read(file, &programHeader, sizeof programHeader) != sizeof programHeader)
          goto done;
      fileOffset += sizeof programHeader;
      switch (programHeader.p_type) {
          case PT_NULL:
          case PT_NOTE:
          case PT_PHDR:
          case PT_STACK:
          default:
              break;
          case PT_DYNAMIC:
          case PT_INTERP:
          case PT_SHLIB:
              goto done;
          case PT_LOAD:
              if (validate_segment(&programHeader, file)) {
                  bool writable = (programHeader.p_flags & PF_W) != 0;
                  uint32_t filePage = programHeader.p_offset & ~PGMASK;
                  uint32_t memPage = programHeader.p_vaddr & ~PGMASK;
                  uint32_t pageOffset = programHeader.p_vaddr & PGMASK;
                  uint32_t readBytes, zeroBytes;
                  if (programHeader.p_filesz > 0) {
                      readBytes = pageOffset + programHeader.p_filesz;
                      zeroBytes = (ROUND_UP(pageOffset + programHeader.p_memsz, PGSIZE)
                                   - readBytes);
                  } else {
                      readBytes = 0;
                      zeroBytes = ROUND_UP(pageOffset + programHeader.p_memsz, PGSIZE);
                  }
                  if (!load_segment(file, filePage, (void *) memPage,
                                    readBytes, zeroBytes, writable))
                      goto done;
              } else {
                  goto done;
              }
              break;
      }
  }

  if (!setup_stack(stackPointer, (const char **)arguments, i))
      goto done;

  *entryPoint = (void (*) (void)) elfHeader.e_entry;
  success = true;

 done:
  if (success) {
      thread_current()->thread_prog.executable = file;
      file_deny_write(file);
  } else {
      file_close(file);
  }
  return success;
}

static bool install_page(void *userPage, void *kernelPage, bool writable);

/* Checks whether the ELF segment described by programHeader is valid. */
static bool validate_segment (const struct Elf32_Phdr *programHeader, struct file *file) 
{
  if ((programHeader->p_offset & PGMASK) != (programHeader->p_vaddr & PGMASK))
    return false;
  if (programHeader->p_offset > (Elf32_Off) file_length(file))
    return false;
  if (programHeader->p_memsz < programHeader->p_filesz)
    return false;
  if (programHeader->p_memsz == 0)
    return false;
  if (!is_user_vaddr((void *) programHeader->p_vaddr))
    return false;
  if (!is_user_vaddr((void *)(programHeader->p_vaddr + programHeader->p_memsz)))
    return false;
  if (programHeader->p_vaddr + programHeader->p_memsz < programHeader->p_vaddr)
    return false;
  if (programHeader->p_vaddr < PGSIZE)
    return false;

  return true;
}

/* Loads a segment starting at offset in the file into user memory at userPage. */
static bool load_segment (struct file *file, off_t offset, uint8_t *userPage,
                          uint32_t readBytes, uint32_t zeroBytes, bool writable) 
{
  ASSERT ((readBytes + zeroBytes) % PGSIZE == 0);
  ASSERT (pg_ofs(userPage) == 0);
  ASSERT (offset % PGSIZE == 0);

  file_seek(file, offset);
  while (readBytes > 0 || zeroBytes > 0) {
      size_t pageReadBytes = readBytes < PGSIZE ? readBytes : PGSIZE;
      size_t pageZeroBytes = PGSIZE - pageReadBytes;

      uint8_t *kernelPage = palloc_get_page(PAL_USER);
      if (kernelPage == NULL)
          return false;

      if (file_read(file, kernelPage, pageReadBytes) != (int) pageReadBytes) {
          palloc_free_page(kernelPage);
          return false;
      }
      memset(kernelPage + pageReadBytes, 0, pageZeroBytes);

      if (!install_page(userPage, kernelPage, writable)) {
          palloc_free_page(kernelPage);
          return false;
      }

      readBytes -= pageReadBytes;
      zeroBytes -= pageZeroBytes;
      userPage += PGSIZE;
  }
  return true;
}

/* Creates a minimal stack by mapping a zeroed page at the top of user virtual memory. */
bool setup_stack(void **stackPointer, const char **argumentVector, int argumentCount) 
{
   uint8_t *kernelPage;
   bool status = false;
   kernelPage = palloc_get_page(PAL_USER | PAL_ZERO);
   if (kernelPage != NULL) {
       status = install_page(((uint8_t *)PHYS_BASE) - PGSIZE, kernelPage, true);
       if (status)
           *stackPointer = PHYS_BASE;
       else
           palloc_free_page(kernelPage);
   }
   
   int index = argumentCount - 1;
   uint32_t lastArgumentAddress; 
   uintptr_t originalStack = (uintptr_t)*stackPointer;
   uint32_t firstArgumentAddress; 
   int totalBytes = 0;          

   while (index >= 0) {
       *stackPointer = (void *)((char *)*stackPointer - strlen(argumentVector[index]) - 1);
       totalBytes += strlen(argumentVector[index]) + 1;
       memcpy(*stackPointer, argumentVector[index], strlen(argumentVector[index]) + 1);
       if (index == argumentCount - 1)
           lastArgumentAddress = (uintptr_t)*stackPointer;
       if (index == 0)
           firstArgumentAddress = (uintptr_t)*stackPointer;
       index--;
   }
   hex_dump((uintptr_t)*stackPointer, *stackPointer, totalBytes, 1);
   
   uintptr_t alignment = (uintptr_t)*stackPointer;
   if (alignment % 4 != 0)
       *stackPointer = (void *)((char *)*stackPointer - (alignment % 4));
   totalBytes += alignment % 4;
   
   index = argumentCount - 1;
   *stackPointer = (void *)((char *)*stackPointer - sizeof(char *));
   totalBytes += sizeof(char *);
   memset(*stackPointer, 0, sizeof(char *));
   
   while (index >= 0) {
       *stackPointer = (void *)((char *)*stackPointer - sizeof(char *));
       totalBytes += sizeof(char *);
       *((char **)*stackPointer) = (char *)lastArgumentAddress;
       if (index != 0)
           lastArgumentAddress = (uintptr_t)((char *)lastArgumentAddress - strlen(argumentVector[index - 1]) - 1);
       firstArgumentAddress = (uintptr_t)*stackPointer;
       index--;
   }
   *stackPointer = (void *)((char *)*stackPointer - sizeof(char **));
   totalBytes += sizeof(char **);
   *((char *** )*stackPointer) = (char **)firstArgumentAddress;
   
   *stackPointer = (void *)((char *)*stackPointer - sizeof(int));
   totalBytes += sizeof(int);
   *((int *)*stackPointer) = argumentCount;
   
   *stackPointer = (void *)((char *)*stackPointer - sizeof(void *));
   totalBytes += sizeof(void *);
   *((void **)*stackPointer) = NULL;
   
   hex_dump((uintptr_t)*stackPointer, *stackPointer, totalBytes, 1);
   return status;
}

/* Maps user virtual address userPage to kernel virtual address kernelPage.
   Returns true if successful. */
static bool install_page (void *userPage, void *kernelPage, bool writable)
{
  struct thread *t = thread_current();
  return (pagedir_get_page(t->pagedir, userPage) == NULL &&
          pagedir_set_page(t->pagedir, userPage, kernelPage, writable));
}

/* --- Utility Functions --- */

/* Extracts the command name from commandString and copies it to commandName. */
static void extract_command_name(char *commandString, char *commandName)
{
  char *savePtr;
  strlcpy(commandName, commandString, PGSIZE);
  /* The strtok_r tokenizes commandName in place. */
  commandName = strtok_r(commandName, " ", &savePtr);
}

/* Extracts command arguments from commandString into argumentVector array and sets argumentCount. */
static void extract_command_arguments(char *commandString, char* argumentVector[], int *argumentCount)
{
  char *savePtr;
  argumentVector[0] = strtok_r(commandString, " ", &savePtr);
  char *token;
  *argumentCount = 1;
  while ((token = strtok_r(NULL, " ", &savePtr)) != NULL) {
    argumentVector[(*argumentCount)++] = token;
  }
}

/* --- File Descriptor Management --- */
static int allocate_fd(void)
{
  return thread_current()->thread_prog.nextFileDescriptor++;
}

struct fd_entry {
  int fd;
  struct file *file;
  struct list_elem elem;
};

static struct fd_entry* get_fd_entry(int fd)
{
  struct list_elem *e;
  struct fd_entry *entry = NULL;
  struct list *fd_table = &thread_current()->thread_prog.fileDescriptorTable;

  for (e = list_begin(fd_table); e != list_end(fd_table); e = list_next(e)) {
      struct fd_entry *temp = list_entry(e, struct fd_entry, elem);
      if (temp->fd == fd) {
        entry = temp;
        break;
      }
  }
  return entry;
}

int process_open (const char *file_name)
{
  struct file *openedFile = filesys_open(file_name);
  if (openedFile == NULL)
    return -1;
  struct fd_entry *fdEntry = malloc(sizeof(struct fd_entry));
  if (fdEntry == NULL)
    return -1;
  fdEntry->fd = allocate_fd();
  fdEntry->file = openedFile;
  list_push_back(&thread_current()->thread_prog.fileDescriptorTable, &fdEntry->elem);
  return fdEntry->fd;
}

int process_write(int fd, const void *buffer, unsigned size)
{
  if (fd == STDOUT_FILENO) {
    putbuf((char *)buffer, (size_t)size);
    return (int)size;
  } else if (get_fd_entry(fd) != NULL) {
    return (int)file_write(get_fd_entry(fd)->file, buffer, size);
  }
  return -1;
}

void process_close(int fd)
{
  if (get_fd_entry(fd) != NULL){
    struct fd_entry *fd_entry = get_fd_entry(fd);
    file_close(fd_entry->file);
    list_remove(&fd_entry->elem);
    free(fd_entry);
  }
}
int process_read (int fd, void *buffer, unsigned length)
{
  if (get_fd_entry(fd) != NULL) {
    struct fd_entry *entry = get_fd_entry(fd);
    return file_read(entry->file, buffer, length);
  }
  return -1;
}

int process_file_length (int fd)
{
  if (get_fd_entry(fd) != NULL) {
    struct fd_entry *entry = get_fd_entry(fd);
    return file_length(entry->file);
  }
  return -1;
}

int process_file_position (int fd)
{
  if (get_fd_entry(fd) != NULL) {
    struct fd_entry *entry = get_fd_entry(fd);
    return file_tell(entry->file);
  }
  return -1;
}

void process_seek (int fd, unsigned position)
{
  if (get_fd_entry(fd) != NULL) {
    struct fd_entry *entry = get_fd_entry(fd);
    file_seek(entry->file, position);
  }
}

/* Closes all open files (including the executable file). */
void process_close_all(void)
{
  struct list *fd_table = &thread_current()->thread_prog.fileDescriptorTable;
  struct list_elem *e = list_begin(fd_table);
  while (e != list_end(fd_table)) {
    struct fd_entry *entry = list_entry(e, struct fd_entry, elem);
    e = list_next(e);
    process_close(entry->fd);
  }
  file_close(thread_current()->thread_prog.executable);
}

void initialize_process(void)
{

  list_init(&thread_current()->thread_prog.childThreads);
}

