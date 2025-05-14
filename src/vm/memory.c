#include <stdio.h>
#include "vm/memory.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "vm/frame_table.h"
#include "vm/memory.h"
#include "vm/page_metadata.h"


/* The stack size cannot grow beyond 256K.*/
#define MAX_STACK_SIZE (PHYS_BASE - PGSIZE * 64)
  
void grow_stack(uint32_t *pd, const void *vaddr) {
  void *page = pg_round_down(vaddr);
  if (pagedir_get_info(pd, page) == NULL
      && check_stack_access(vaddr)) {
      struct page_metadata *pi = page_metadata_create();
      if (pi) {
          page_metadata_set_upage(pi, page);
          page_metadata_set_pagedir(pi, pd);
          page_metadata_set_type(pi, PAGE_TYPE_ZERO);
          page_metadata_set_writable(pi, WRITABLE_TO_SWAP);
          pagedir_attach_info(pd, page, pi);
      }
  }
}

bool check_stack_access(const void *vaddr) {
  void *esp = thread_current()->user_esp;
  ptrdiff_t delta = esp - vaddr;
  return vaddr >= (void*)MAX_STACK_SIZE
      && (delta == 8 || delta == 32 || vaddr >= esp);
}


static bool register_file_page(void *upage, struct file *f, off_t ofs) {
  struct page_metadata *pi = page_metadata_create();
  if (!pi) return false;
  page_metadata_set_pagedir(pi, thread_current()->pagedir);
  page_metadata_set_upage(pi, upage);
  page_metadata_set_type(pi, PAGE_TYPE_FILE);
  page_metadata_set_fileinfo(pi, f, ofs);
  page_metadata_set_writable(pi, WRITABLE_TO_FILE);
  pagedir_attach_info(thread_current()->pagedir, upage, pi);
  return true;
}

static void unload_pages(uint32_t *pd, void *start, size_t count) {
  for (size_t i = 0; i < count; i++, start = (char*)start + PGSIZE)
      frametable_unload_frame(pd, start);
}

static int
allocate_map_descriptor (void *upage, struct file *file, size_t num_pages);
static bool is_map_request_valid(int fd, void *addr) {
  return addr != NULL
      && pg_ofs(addr) == 0
      && fd > STDOUT_FILENO
      && fd_get_file(fd) != NULL
      && file_length(fd_get_file(fd)) > 0;
}

static bool is_range_free(void *start, size_t count) {
  struct thread *cur = thread_current();
  for (size_t i = 0; i < count; i++, start = (char*)start + PGSIZE) {
      if (pagedir_get_info(cur->pagedir, start) != NULL
          || check_stack_access(start))
          return false;
  }
  return true;
}

static bool load_mapping_pages(void *start, struct file *f,
                             size_t count, off_t total_len) {
  off_t remaining = total_len, offset = 0;
  struct thread *cur = thread_current();

  for (size_t i = 0; i < count; i++, start = (char*)start + PGSIZE) {
      offset += (remaining > PGSIZE ? PGSIZE : remaining);
      remaining = remaining > PGSIZE ? remaining - PGSIZE : 0;
      if (!register_file_page(start, f, offset)) {
          return false;
      }
  }
  return true;
}

int mmap(int fd, void *start_addr) {
  if (!is_map_request_valid(fd, start_addr))
      return -1;

  struct file *f = fd_get_file(fd);
  off_t len = file_length(f);
  size_t pages = (size_t)pg_round_up((void *)len) / PGSIZE;

  if (!is_range_free(start_addr, pages))
      return -1;

  f = file_reopen(f);
  if (!f)
      return -1;

  int map_id = allocate_map_descriptor(start_addr, f, pages);
  if (map_id < 0) {
      file_close(f);
      return -1;
  }

  if (!load_mapping_pages(start_addr, f, pages, len)) {
      unload_pages(thread_current()->pagedir, start_addr, pages);
      file_close(f);
      return -1;
  }

  return map_id;
}


void munmap (int md)
{
  struct thread *cur = thread_current ();
  struct mmap *mmap;
  void *upage;
  size_t i;
  
  if (md >= 0 && md < MAX_MMAP_FILES)
    {
      mmap = &cur->mfiles[md];
      if (mmap->file != NULL)
        {
          for (upage = mmap->upage, i = 0; i < mmap->num_pages; i++, upage += PGSIZE)
            frametable_unload_frame (cur->pagedir, upage);
          file_close (mmap->file);
          mmap->file = NULL;
        }
    }
}

static int allocate_map_descriptor(void *vaddr, struct file *f, size_t num) {
  struct thread *cur = thread_current();
  for (int i = 0; i < MAX_MMAP_FILES; i++) {
      if (cur->mfiles[i].file == NULL) {
          cur->mfiles[i].upage     = vaddr;
          cur->mfiles[i].file      = f;
          cur->mfiles[i].num_pages = num;
          return i;
      }
  }
  return -1;
}