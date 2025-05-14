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
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/page_metadata.h"
#include "vm/memory.h"

/* Maximum size of program arguments. */
#define MAX_ARGS_SIZE 512

struct start_args
{
  char *program_name;
  char *program_args;
  tid_t ptid;
  struct semaphore start_wait;
  struct thread *child;
};

static thread_func start_process NO_RETURN;
static bool load (char *program_name, char *program_args, void (**eip) (void),
                  void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name_) 
{
  struct thread *cur = thread_current ();
  char *file_name;
  struct start_args args;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  file_name = palloc_get_page (0);
  if (file_name == NULL)
    return TID_ERROR;
  strlcpy (file_name, file_name_, PGSIZE);
  args.program_name = strtok_r (file_name, " ", &args.program_args);
  args.ptid = cur->tid;

  /* Create a new thread to execute FILE_NAME. */
  sema_init (&args.start_wait, 0);
  tid = thread_create (args.program_name, PRI_DEFAULT, start_process, &args);
  if (tid != TID_ERROR)
    {
      /* Wait for the thread to start running so it can be added to the child
         list. */
      sema_down (&args.start_wait);
      if (args.child != NULL)  
        list_push_back (&cur->child_list, &args.child->child_elem);
      else
        tid = TID_ERROR;
    }
  palloc_free_page (file_name); 
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *args_)
{
  struct thread *cur = thread_current ();
  struct start_args *args = args_;
  struct intr_frame if_;
  bool success = false;

  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  if (load (args->program_name, args->program_args, &if_.eip, &if_.esp))
    {
      args->child = cur;
      cur->ptid = args->ptid;
      success = true;
    }
  else
    args->child = NULL;
  sema_up (&args->start_wait);

  if (!success)
    thread_exit ();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
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
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct thread *child;
  struct list_elem *e;
  int exit_status;

  for (child = NULL, e = list_begin (&cur->child_list);
       e != list_end (&cur->child_list); e = list_next (e))
    {
      child = list_entry (e, struct thread, child_elem);
      if (child->tid == child_tid)
        {
          list_remove (e);
          break;
        }
    }
  if (child != NULL)
    {
 
      lock_acquire (&child->exit_lock);
      while (child->status != THREAD_EXITING)
        cond_wait (&child->exiting, &child->exit_lock);
      lock_release (&child->exit_lock);
      exit_status = child->exit_status;
      palloc_free_page (child);
    }
  else
    exit_status = -1; 

  return exit_status;
}

void
process_exit (void)
{
    struct thread *self = thread_current ();
    uint32_t *pd_dir;
    int fd_idx;
    int mm_idx;
    struct list_elem *le;
    enum thread_status next_status;

    /* Unmap all memory-mapped files */
    if (self->mfiles != NULL)
    {
        for (mm_idx = 0; mm_idx < MAX_MMAP_FILES; mm_idx++)
            munmap (mm_idx);
        free (self->mfiles);
    }

    /* Destroy and switch away from this process's page directory */
    pd_dir = self->pagedir;
    if (pd_dir != NULL)
    {
        self->pagedir = NULL;
        pagedir_activate (NULL);
        pagedir_destroy (pd_dir);
        printf ("%s: exit(%d)\n", self->name, self->exit_status);
    }

    /* Close all open file descriptors */
    if (self->ofiles != NULL)
    {
        for (fd_idx = 2; fd_idx < MAX_OPEN_FILES; fd_idx++)
            fd_close (fd_idx);
        free (self->ofiles);
    }

    /* Signal any waiting parent and clean up child structures */
    lock_acquire (&self->exit_lock);
    for (le = list_begin (&self->child_list);
         le != list_end (&self->child_list);
         le = list_remove (le))
    {
        struct thread *child_thr = list_entry (le, struct thread, child_elem);

        lock_acquire (&child_thr->exit_lock);
        /* Orphan the child so it won't signal us later */
        child_thr->ptid = TID_NONE;
        if (child_thr->status == THREAD_EXITING)
            palloc_free_page (child_thr);
        lock_release (&child_thr->exit_lock);
    }

    if (self->ptid != TID_NONE)
    {
        next_status = THREAD_EXITING;
        cond_signal (&self->exiting, &self->exit_lock);
    }
    else
    {
        next_status = THREAD_DYING;
    }

    intr_disable ();
    lock_release (&self->exit_lock);
    self->status = next_status;
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *cur = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (cur->pagedir);

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
#define PE32Ax PRIx32   /* Print Enlf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
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
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (const char *program_name, char *args, void **esp);
static bool validate_segment (const struct Elf32_Phdr *, int fd);
static bool load_segment (int fd, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);


bool
load (char *program_name, char *program_args, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  off_t file_ofs;
  bool success = false;
  int i;
  int fd;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  t->ofiles = calloc (MAX_OPEN_FILES, sizeof *t->ofiles);
  if (t->ofiles == NULL)
    goto done;
  t->mfiles = calloc (MAX_MMAP_FILES, sizeof *t->mfiles);
  if (t->mfiles == NULL)
    goto done;

  fd = fd_open (program_name, true);
  if (fd == -1)
    {
      printf ("load: %s: open failed\n", program_name);
      goto done;       
    }

  /* Read and verify executable header. */
  if (fd_read (fd, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", program_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > fd_size (fd))
        goto done;
      fd_seek (fd, file_ofs);

      if (fd_read (fd, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, fd)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (fd, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }
  
  /* Set up stack. */
  if (!setup_stack (program_name, program_args, esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, int fd) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) fd_size (fd)) 
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

   Return true if successful, false if a memory allocation error occurs.
*/
static bool
load_segment (int fd, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  struct thread *cur = thread_current ();
  struct page_metadata *page_metadata;
  struct file *file;
    
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file = fd_get_file (fd);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      page_metadata = page_metadata_create ();
      if (page_metadata == NULL)
        return false;
      
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      page_metadata_set_pagedir (page_metadata, cur->pagedir);
      page_metadata_set_upage (page_metadata, upage);
      if (page_read_bytes > 0)
        {
          ofs += page_read_bytes;
          page_metadata_set_type (page_metadata, PAGE_TYPE_FILE);
          page_metadata_set_fileinfo (page_metadata, file, ofs);
        }
      else
        page_metadata_set_type (page_metadata, PAGE_TYPE_ZERO);
      if (writable)
        page_metadata_set_writable (page_metadata, WRITABLE_TO_SWAP);
        pagedir_attach_info (cur->pagedir, upage, page_metadata);
      
      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}
static bool
setup_stack (const char *prog_name, char *cmdline, void **esp)
{
    struct thread *th = thread_current ();
    struct page_metadata *meta;
    const char *token;
    uint8_t *kernel_page = NULL, *arg_addr, *arg_list_top, *argv_base;
    void *user_page;
    size_t token_len;
    int count;
    bool ok = false;

    /* Allocate a zeroed page and create its metadata */
    kernel_page = palloc_get_page (PAL_ZERO);
    meta = page_metadata_create ();
    if (kernel_page != NULL && meta != NULL)
    {
        user_page = (void *) (PHYS_BASE - PGSIZE);
        ok = install_page (user_page, kernel_page, true);
        if (ok)
        {
            /* Layout arguments and pointers on the new stack page */
            arg_addr      = PHYS_BASE;
            arg_list_top  = PHYS_BASE - MAX_ARGS_SIZE;
            argv_base     = arg_list_top;
            count         = 0;
            token         = prog_name;

            while (token != NULL)
            {
                token_len = strlen (token) + 1;
                if (word_round_down (arg_addr - token_len)
                    <= (void *) (arg_list_top + 2 * sizeof (char *)))
                    break;

                arg_addr -= token_len;
                memcpy (arg_addr, token, token_len);
                *((uint8_t **) arg_list_top) = arg_addr;
                arg_list_top += sizeof (char *);
                count++;
                token = cmdline != NULL
                        ? strtok_r (NULL, " ", &cmdline)
                        : NULL;
            }

            /* Align and push argv array, argc, and null sentinel */
            arg_addr = word_round_down (arg_addr) - sizeof (char *);
            memmove (argv_base + (arg_addr - arg_list_top),
                     argv_base, count * sizeof (char *));
            argv_base += arg_addr - arg_list_top;
            *((uint8_t **)(argv_base - sizeof (char **))) = argv_base;
            argv_base -= sizeof (char **) + sizeof (count);
            *((int *) argv_base) = count;
            argv_base -= sizeof (void *);
            *argv_base = 0;
            *esp = argv_base;

            /* Attach metadata for lazy stack initialization */
            page_metadata_set_upage    (meta, user_page);
            page_metadata_set_pagedir  (meta, th->pagedir);
            page_metadata_set_type     (meta, PAGE_TYPE_KERNEL);
            page_metadata_set_kpage    (meta, kernel_page);
            page_metadata_set_writable (meta, WRITABLE_TO_SWAP);
            pagedir_attach_info        (th->pagedir, user_page, meta);
            pagedir_clear_page         (th->pagedir, user_page);
        }
        else
        {
            page_metadata_destroy (meta);
            palloc_free_page (kernel_page);
        }
    }
    return ok;
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
