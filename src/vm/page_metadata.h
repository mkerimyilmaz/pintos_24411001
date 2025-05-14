#ifndef VM_PAGEINFO_H
#define VM_PAGEINFO_H

#include <stdint.h>
#include "filesys/off_t.h"
#include "devices/block.h"


/* Page content is zero. */
#define PAGE_TYPE_ZERO    0x01
/* Page content is loaded from a page size chunk of kernel memory. */
#define PAGE_TYPE_KERNEL  0x02
/* Page is backed by a file. */
#define PAGE_TYPE_FILE    0x04

/* If a page is writable, it will be written back to a file or to swap. */
#define WRITABLE_TO_FILE  0x01
#define WRITABLE_TO_SWAP  0x02

struct file;

struct page_metadata *page_metadata_create (void);
void page_metadata_destroy (struct page_metadata *page_metadata);
void page_metadata_set_upage (struct page_metadata *page_metadata, const void *upage);
void page_metadata_set_type (struct page_metadata *page_metadata, int type);
void page_metadata_set_writable (struct page_metadata *page_metadata, int writable);
void page_metadata_set_pagedir (struct page_metadata *page_metadata, uint32_t *pd);
void page_metadata_set_fileinfo (struct page_metadata *page_metadata, struct file *file, off_t offset_cnt);
void page_metadata_set_kpage (struct page_metadata *page_metadata, const void *kpage);

#endif /* vm/page_metadata.h */
