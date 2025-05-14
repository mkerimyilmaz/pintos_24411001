#ifndef USERPROG_PAGEDIR_H
#define USERPROG_PAGEDIR_H

#include <stdbool.h>
#include <stdint.h>

struct page_metadata;

uint32_t *pagedir_create (void);
void pagedir_destroy (uint32_t *pd);
bool pagedir_set_page (uint32_t *pd, const void *upage, void *kpage, bool rw);
void *pagedir_get_page (uint32_t *pd, const void *uaddr);
void pagedir_clear_page (uint32_t *pd, const void *upage);
bool pagedir_is_dirty (uint32_t *pd, const void *vpage);
void pagedir_set_dirty (uint32_t *pd, const void *vpage, bool dirty);
bool pagedir_is_accessed (uint32_t *pd, const void *vpage);
void pagedir_set_accessed (uint32_t *pd, const void *vpage, bool accessed);
void pagedir_activate (uint32_t *pd);
void pagedir_unmap_page (uint32_t *pd, const void *upage);
bool pagedir_attach_info (uint32_t *pd, const void *upage, struct page_metadata *info);
struct page_metadata *pagedir_get_info(uint32_t *page_dir, const void *user_page);
#endif /* userprog/pagedir.h */
