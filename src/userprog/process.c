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


static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

static void extract_cmd_name(char * cmd_string, char *command_name);
static void extract_cmd_args(char * cmd_string, char* argv[], int *argc);
void process_close_all(void);

struct write_request {
    int ticket;
    char *name_of_con;
    int msg; 
    struct list_elem elem;
};

struct read_request {
    int ticket;
    char *name_of_con;
    struct semaphore sema; // the semaphore sync primitive is used to implement the blocking
    struct list_elem elem;
};

static struct list write_requests;
static struct list read_requests;

struct process_identifier{
  int pid;
  struct list_elem elem;
};
//todo   initialize_process();
void process_init(void)
{
  connector_ini();

  list_init(&thread_current()->thread_prog.childThreads);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
pid_t
process_execute (const char *file_name)
{
  char *fn_copy;
  pid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  // make another copy of file name, which will be saved as a property in the process structure
  char *cmd_nom = malloc (strlen(fn_copy)+1);
  if (cmd_nom == NULL)
    return TID_ERROR;
    extract_cmd_name(fn_copy, cmd_nom);
  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (file_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR){
    palloc_free_page (fn_copy);
    free(cmd_nom);
    return -1;
  }
  // update thread with userprog properties
  struct thread *t = get_thread(tid);
  t->thread_prog.nextFileDescriptor = 2;
  t->thread_prog.programName = cmd_nom;
  list_init(&t->thread_prog.fileDescriptorTable);
  list_init(&t->thread_prog.childThreads);

  int status = connector_read("exec", tid);
  if (status != -1){
    // add the process as a child
    struct process_identifier *p = malloc(sizeof(struct process_identifier));
    p->pid = status;
    list_push_back(&thread_current()->thread_prog.childThreads, &p->elem);
  }
  return status;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{

  char *file_name = file_name_;

  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  palloc_free_page (file_name);

  if (!success){
    connector_write("exec", thread_tid(), -1);
    thread_exit (-1);
  }
  connector_write("exec", thread_tid(), thread_tid());  
  
  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  //todo kerim
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}


static bool process_is_parent_of(pid_t pid){
  struct list_elem *e; 
  for (e = list_begin (&thread_current()->thread_prog.childThreads); e != list_end (&thread_current()->thread_prog.childThreads);
       e = list_next (e))
    {
      if(list_entry(e, struct process_identifier, elem)->pid == pid){
        return true;
      };
    }
  return false;
}

static void remove_child(pid_t pid){
  struct list_elem *e = NULL; 
  for (e = list_begin (&thread_current()->thread_prog.childThreads); e != list_end (&thread_current()->thread_prog.childThreads);
       e = list_next (e))
    {
      if(list_entry(e, struct process_identifier, elem)->pid == pid){
        break;
      };
    }
  if (e != NULL)
    list_remove(e);
}
/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (pid_t child_tid)
{
  if(!process_is_parent_of(child_tid))
      return -1;
  remove_child(child_tid); // hack: remove the child from a process enfants list to make sure a process can't wait for a child twice
  return connector_read("wait", child_tid);
}

  
/* Free the current process's resources. */
void
process_exit (int status)
{
  struct thread *cur = thread_current();
  connector_write("wait", cur->tid, status);
  // return if it's a kernel thread
  if(thread_tid() == 1){
    return;
  }
  // close open descriptors;
  process_close_all();
  printf("%s: exit(%d)\n", cur->thread_prog.programName, status);

  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL)
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}


/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
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

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, char **argv, int argc);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

                          static bool load_segments(struct file *file, struct Elf32_Ehdr *ehdr);

bool
load(const char *file_name, void (**eip)(void), void **esp)
{
  struct thread *t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;

  // Parse command line arguments
  char file_name_copy[ARGUMENT_MAX];
  strlcpy(file_name_copy, file_name, ARGUMENT_MAX);
  char *argv[ARGUMENT_MAX];
  int argc;
  extract_cmd_args(file_name_copy, argv, &argc);

  // Create and activate page directory
  t->pagedir = pagedir_create();
  if (t->pagedir == NULL)
    goto done;
  process_activate();

  // Open executable
  file = filesys_open(argv[0]);
  if (file == NULL) {
    printf("load: %s: open failed\n", argv[0]);
    goto done;
  }

  // Read and validate ELF header
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) ||
      ehdr.e_type != 2 ||
      ehdr.e_machine != 3 ||
      ehdr.e_version != 1 ||
      ehdr.e_phentsize != sizeof(struct Elf32_Phdr) ||
      ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  // Load segments
  if (!load_segments(file, &ehdr))
    goto done;

  // Set up stack with arguments
  if (!setup_stack(esp, argv, argc))
    goto done;

  *eip = (void (*)(void))ehdr.e_entry;
  success = true;

done:
  if (success) {
    t->thread_prog.executable = file;
    file_deny_write(file);
  } else {
    file_close(file);
  }

  return success;
}

static bool
load_segments(struct file *file, struct Elf32_Ehdr *ehdr)
{
off_t file_ofs = ehdr->e_phoff;

for (int i = 0; i < ehdr->e_phnum; i++) {
struct Elf32_Phdr phdr;

if (file_ofs < 0 || file_ofs > file_length(file))
  return false;
file_seek(file, file_ofs);

if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
  return false;
file_ofs += sizeof phdr;

switch (phdr.p_type) {
  case PT_NULL:
  case PT_NOTE:
  case PT_PHDR:
  case PT_STACK:
    break;
  case PT_DYNAMIC:
  case PT_INTERP:
  case PT_SHLIB:
    return false;
  case PT_LOAD:
    if (!validate_segment(&phdr, file))
      return false;

    bool writable = (phdr.p_flags & PF_W) != 0;
    uint32_t file_page = phdr.p_offset & ~PGMASK;
    uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
    uint32_t page_offset = phdr.p_vaddr & PGMASK;

    uint32_t read_bytes = (phdr.p_filesz > 0)
      ? page_offset + phdr.p_filesz
      : 0;
    uint32_t zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes;

    if (!load_segment(file, file_page, (void *)mem_page,
                      read_bytes, zero_bytes, writable))
      return false;
    break;
}
}

return true;
}
                          

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack(void **esp, char **argv, int argc)
{
  uint8_t *kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage == NULL)
    return false;

  bool success = install_page(((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
  if (!success) {
    palloc_free_page(kpage);
    return false;
  }

  *esp = PHYS_BASE;
  uint32_t *arg_ptrs[argc];

  // Push argument strings onto stack (in reverse)
  for (int i = argc - 1; i >= 0; i--) {
    size_t len = strlen(argv[i]) + 1;
    *esp -= len;
    memcpy(*esp, argv[i], len);
    arg_ptrs[i] = (uint32_t *) *esp;
  }

  // Word align (optional in some implementations)
  *esp = (void *) ((uintptr_t)(*esp) & 0xfffffffc);

  // Push null sentinel
  *esp -= sizeof(uint32_t);
  *(uint32_t *)(*esp) = 0;

  // Push argument pointers
  for (int i = argc - 1; i >= 0; i--) {
    *esp -= sizeof(uint32_t);
    *(uint32_t **)(*esp) = arg_ptrs[i];
  }

  // Push argv (pointer to argument pointers)
  uint32_t *argv_addr = *esp;
  *esp -= sizeof(uint32_t);
  *(uint32_t **)(*esp) = argv_addr;

  // Push argc
  *esp -= sizeof(uint32_t);
  *(int *)(*esp) = argc;

  // Push fake return address
  *esp -= sizeof(uint32_t);
  *(uint32_t *)(*esp) = 0;

  return true;
}


/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}


static void
extract_cmd_name(char *cmd_string, char *cmd_name)
{
  char *save_ptr;
  strlcpy(cmd_name, cmd_string, PGSIZE);
  char *token = strtok_r(cmd_name, " ", &save_ptr);
  if (token != NULL)
    strlcpy(cmd_name, token, PGSIZE);
  else
    cmd_name[0] = '\0';
}

static void
extract_cmd_args(char *cmd_string, char *argv[], int *argc)
{
  char *save_ptr;
  *argc = 0;

  char *token = strtok_r(cmd_string, " ", &save_ptr);
  while (token != NULL && *argc < ARGUMENT_MAX)
  {
    argv[(*argc)++] = token;
    token = strtok_r(NULL, " ", &save_ptr);
  }

  if (*argc < ARGUMENT_MAX)
    argv[*argc] = NULL;
}

// File descriptor manager

static int allocate_fd (void)
{
  return thread_current()->thread_prog.nextFileDescriptor++;
}

struct fd_entry
{
  int fd;
  struct file *file;
  struct list_elem elem;
};

static struct fd_entry* get_fd_entry(int fd)
{
  struct list_elem *e;
  struct fd_entry *fe = NULL;
  struct list *fd_table = &thread_current()->thread_prog.fileDescriptorTable;

  for (e = list_begin (fd_table); e != list_end (fd_table);
       e = list_next (e))
    {
      struct fd_entry *tmp = list_entry (e, struct fd_entry, elem);
      if(tmp->fd == fd){
        fe = tmp;
        break;
      }
    }

  return fe;
}
int process_file_length (int fd)
{
  if (get_fd_entry(fd) != NULL){
    struct fd_entry *fd_entry = get_fd_entry(fd);
    return file_length(fd_entry->file);
  }
  return -1;
}


int process_open (const char *file_name)
{
  struct file * f = filesys_open (file_name);
  if (f == NULL)
    return -1;
  struct fd_entry *fd_entry = malloc (sizeof(struct fd_entry));
  if (fd_entry == NULL)
    return -1;
  fd_entry->fd = allocate_fd();
  fd_entry->file = f;
  list_push_back(&thread_current()->thread_prog.fileDescriptorTable, &fd_entry->elem);

  return fd_entry->fd;
}

void process_seek (int fd, unsigned position){
  if (get_fd_entry(fd) != NULL){
    struct fd_entry *fd_entry = get_fd_entry(fd);
    file_seek(fd_entry->file, position);
  }
}


int process_write(int fd, const void *buffer, unsigned size)
{
  if (fd == STDOUT_FILENO){
    putbuf((char *)buffer, (size_t)size);
    return (int)size;
  }else if (get_fd_entry(fd) != NULL){
    return (int)file_write(get_fd_entry(fd)->file, buffer, size);
  }
  return -1;
}

void process_close (int fd)
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
  if (get_fd_entry(fd) != NULL){
    struct fd_entry *fd_entry = get_fd_entry(fd);
    return file_read(fd_entry->file, buffer, length);
  }
  return -1;
}

int process_file_position (int fd)
{
  if (get_fd_entry(fd) != NULL){
    struct fd_entry *fd_entry = get_fd_entry(fd);
    return file_length(fd_entry->file);
  }
  return -1;
}


// close all open files (including the executable)
void process_close_all(void)
{
  struct list *fd_table = &thread_current()->thread_prog.fileDescriptorTable;
  struct list_elem *e = list_begin (fd_table);
  while (e != list_end (fd_table))
    {
      struct fd_entry *tmp = list_entry (e, struct fd_entry, elem);
      e = list_next (e);
      process_close(tmp->fd);
    }
  // close the executable
  file_close (thread_current()->thread_prog.executable );
}


void connector_ini(void){
    list_init(&read_requests);
    list_init(&write_requests);
}

int connector_read(char * name_of_con, int ticket){
  // check whether there is already something written that correspond to the read 
  struct list_elem *e;
  for (e = list_begin (&write_requests); e != list_end (&write_requests); e = list_next (e))
  {
    struct write_request *w = list_entry (e, struct write_request, elem);
    if(w->name_of_con == name_of_con && ticket == w->ticket){
      list_remove(e);
      int msg = w->msg;
      free(w);
      return msg;
    }
  }
  struct read_request *r = malloc(sizeof(struct read_request));
  sema_init(&r->sema, 0);
  r->ticket = ticket;
  r->name_of_con = name_of_con;
  list_push_back(&read_requests, &r->elem);
  sema_down(&r->sema);

  for (e = list_begin (&write_requests); e != list_end (&write_requests); e = list_next (e))
  {
    struct write_request *w = list_entry (e, struct write_request, elem);
    if(w->name_of_con == r->name_of_con && r->ticket == w->ticket){
      list_remove(e);
      list_remove(&r->elem);
      int msg = w->msg;
      free(w);
      free(r);
      return msg;
    }
  }
  NOT_REACHED ();
}

void connector_write(char *name_of_con, int ticket, int msg){
  struct write_request *w= malloc(sizeof(struct write_request));
  w->ticket = ticket;
  w->name_of_con = name_of_con;
  w->msg = msg;
  list_push_back(&write_requests, &w->elem);

  struct list_elem *e;
  for (e = list_begin (&read_requests); e != list_end (&read_requests); e = list_next (e))
  {
    struct read_request *r = list_entry (e, struct read_request, elem);
    if(r->name_of_con == name_of_con && r->ticket == w->ticket){
      sema_up(&r->sema);
    }
  }
}
