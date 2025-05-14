#include <stdio.h>
#include <stdbool.h>
#include <list.h>
#include <hash.h>
#include <debug.h>
#include <string.h>
#include "userprog/pagedir.h"
#include "vm/page_metadata.h"
#include "vm/frame_table.h"
#include "vm/swap.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "filesys/file.h"
#include "filesys/inode.h"
#include "filesys/filesys.h"
/* Additional file information, only relevant if the page is backed by a 
   file. */
struct file_info
{
  struct file *file;
  /* The file offset of the end of the mapped region.  It's used to
     calculate the start offset and the size of the region.
  */
  off_t end_offset;
};

static inline off_t offset (off_t end_offset)
{
  return end_offset > 0  ? (end_offset - 1) & ~PGMASK : 0;
}

static inline off_t size (off_t end_offset)
{
  return end_offset - offset (end_offset);
}

/* Additional information associated with user pages. */
struct page_metadata
{
  uint8_t type;
  uint8_t writable;
  /* The page directory that is mapping the page. */
  uint32_t *pd;
  /* The user virtual page address corresponding to the page. */
  const void *upage;
  /* If true the page is swapped and its contents can be read back
     from swap_sector. */
  bool swapped;
  /* Information about the frame backing the page. */
  struct frame *frame;
  /* Depending on the type this can be information about the backing file, the 
     swap block if the page is swapped out, or the kernel virtual page address
     of content to to initialize the page with. */
  union
  {
    struct file_info file_info;
    block_sector_t swap_sector;
    const void *kpage;
  } data;
  /* List element for the associated frame's page_metadata_list. */
  struct list_elem elem;
};

/* Information associated with each frame. */
struct frame
{
  /* The kernal virtual page address corresponding to the physical frame. */
  void *kpage;
  /* List of information about each page that is mapped to this frame. */
  struct list page_metadata_list;
  /* If greater than zero the frame is locked and will not be evicted. */
  unsigned short lock;
  /* If true, data is being read to or written from this frame. */
  bool io;
  struct condition io_done;
  /* Hash element for read_only_frames. */
  struct hash_elem hash_elem;
  struct list_elem list_elem;
};

/* Lock used for manipullating internal data structures. */
static struct lock frame_lock;
/* Cache of read-only file frames indexed by inode and offset. */
static struct hash read_only_frames;
/* List of frames that are potentially available for for eviction.
   This is treated as a circular list with clock hand pointing 
   to the beginning of the list.  Frames are always added to the
   end of the list. */
static struct list frame_list;
/* The hand of the clock for the clock page replacement algorithm
   which is used for choosing a frame to evict. The clock hand 
   points to the next frame to examine. */
static struct list_elem *clock_hand;

static void frame_init (struct frame *frame);
static struct frame *allocate_frame (void);
static bool load_frame (uint32_t *pd, const void *upage, bool write,
                        bool keep_locked);
static void map_page (struct page_metadata *page_metadata, struct frame *frame,
                      const void *upage);
static void wait_for_io_done (struct frame **frame);
static struct frame *lookup_read_only_frame (struct page_metadata *page_metadata);
static void *evict_frame (void);
static void *get_frame_to_evict (void);
static unsigned frame_hash (const struct hash_elem *e, void *aux UNUSED);
static bool frame_less (const struct hash_elem *a, const struct hash_elem *b,
                        void *aux UNUSED);

/* Yeni yardımcı fonksiyonlar */
static void write_dirty_page_to_file(struct page_metadata *page_metadata, struct frame *frame);
static void handle_frame_removal_from_cache(struct page_metadata *page_metadata, struct frame *frame);
static void unmap_and_clear_page(struct page_metadata *page_metadata);
static void handle_io_operations(struct page_metadata *page_metadata, struct frame *frame);
static void prepare_frame_for_eviction(struct frame *frame);
static void mark_pages_as_swapped(struct frame *frame, block_sector_t swap_sector);
static bool check_frame_eligibility(struct frame *frame);
static void clean_up_page_resources(struct page_metadata *page_metadata);

struct page_metadata *
page_metadata_create (void)
{
  struct page_metadata *page_metadata;
  
  page_metadata = calloc (1, sizeof *page_metadata);
  return page_metadata;
}

void
page_metadata_destroy (struct page_metadata *page_metadata)
{
  free (page_metadata);
}

void
page_metadata_set_upage (struct page_metadata *page_metadata, const void *upage)
{
  page_metadata->upage = upage;
}

void
page_metadata_set_type (struct page_metadata *page_metadata, int type)
{
  page_metadata->type = type;
}

void
page_metadata_set_writable (struct page_metadata *page_metadata, int writable)
{
  page_metadata->writable = writable;
}

void
page_metadata_set_pagedir (struct page_metadata *page_metadata, uint32_t *pd)
{
  page_metadata->pd = pd;
}

void
page_metadata_set_fileinfo (struct page_metadata *page_metadata, struct file *file,
                       off_t end_offset)
{
  page_metadata->data.file_info.file = file;
  page_metadata->data.file_info.end_offset = end_offset;
}

void
page_metadata_set_kpage (struct page_metadata *page_metadata, const void *kpage)
{
  page_metadata->data.kpage = kpage;
}

void
initialize_frame_table (void)
{
  lock_init (&frame_lock);
  list_init (&frame_list);
  clock_hand = list_end (&frame_list);
  hash_init (&read_only_frames, frame_hash, frame_less, NULL);
}

/* Reads data into a frame from the appropriate place and maps the
   user virtual page UPAGE to it.  If WRITE is true, the page will be
   mapped as read/write. */
bool
frametable_load_frame (uint32_t *pd, const void *upage, bool write)
{
  return load_frame (pd, upage, write, false);
}

/* Unmaps the frame mapped by UPAGE, writes out any modified data to the 
   appropriate place, and frees all resources associated with the 
   the frame and the page info. */
void
frametable_unload_frame (uint32_t *pd, const void *upage)
{
  struct page_metadata *page_metadata;
  struct frame *frame;

  ASSERT (is_user_vaddr (upage));
  page_metadata = pagedir_get_info (pd, upage);
  if (page_metadata == NULL)
    return;
    
  lock_acquire (&frame_lock);
  /* It's possible the frame could be in the process of being evicted.
     If so, wait for eviction to finish before continuing. When 
     wait_for_io_done returns, frame_lock will be held. */
  wait_for_io_done (&page_metadata->frame);
  
  if (page_metadata->frame != NULL)
  {
    frame = page_metadata->frame;
    page_metadata->frame = NULL;
    
    handle_frame_removal_from_cache(page_metadata, frame);
    unmap_and_clear_page(page_metadata);
    
    /* At this point the frame has been removed from the shared data
       structures and it's safe to release the lock and, if necessary,
       free the resources associated with the frame. */
    lock_release (&frame_lock);
    
    if (list_empty (&frame->page_metadata_list))
    {
      write_dirty_page_to_file(page_metadata, frame);
      palloc_free_page (frame->kpage);
      ASSERT (frame->lock == 0);
      free (frame);
    }
  }
  else
    lock_release (&frame_lock);
    
  /* Free resources associated with page info. */
  clean_up_page_resources(page_metadata);
  pagedir_attach_info (page_metadata->pd, upage, NULL);
  free (page_metadata);
}

/* Writes a dirty page back to its backing file if needed */
static void 
write_dirty_page_to_file(struct page_metadata *page_metadata, struct frame *frame)
{
  struct file_info *file_info;
  off_t bytes_written;
  
  if (page_metadata->writable & WRITABLE_TO_FILE
      && pagedir_is_dirty (page_metadata->pd, page_metadata->upage))
  {
    ASSERT (page_metadata->writable != 0);
    file_info = &page_metadata->data.file_info;
    bytes_written = file_write_at (file_info->file,
                                 frame->kpage,
                                 size (file_info->end_offset),
                                 offset (file_info->end_offset));
    ASSERT (bytes_written == size (file_info->end_offset));
  }
}

/* Handles removing a frame from the cache and lists */
static void
handle_frame_removal_from_cache(struct page_metadata *page_metadata, struct frame *frame)
{
  struct page_metadata *p;
  struct list_elem *e;
  
  if (list_size (&frame->page_metadata_list) > 1)
  {
    for (e = list_begin (&frame->page_metadata_list);
         e != list_end (&frame->page_metadata_list); e = list_next (e))
    {
      p = list_entry (e, struct page_metadata, elem);
      if (page_metadata == p)
      {
        list_remove (e);
        break;
      }
    }
  }
  else
  {
    ASSERT (list_entry (list_begin (&frame->page_metadata_list),
                      struct page_metadata, elem) == page_metadata);
    /* Don't remove the page info from the frame's list until
       the frame is removed from the cache because in order to
       lookup the frame for removal it needs to have a page info
       in its list. */
    if (page_metadata->type & PAGE_TYPE_FILE && page_metadata->writable == 0)
      hash_delete (&read_only_frames, &frame->hash_elem);
    if (clock_hand == &frame->list_elem)
    {
      clock_hand = list_next (clock_hand);
      if (clock_hand == list_end (&frame_list))
        clock_hand = list_begin (&frame_list);
    }
    list_remove (&page_metadata->elem);
    list_remove (&frame->list_elem);
  }
}

/* Unmaps and clears the page */
static void
unmap_and_clear_page(struct page_metadata *page_metadata)
{
  pagedir_clear_page (page_metadata->pd, page_metadata->upage);
}

/* Cleans up resources associated with a page metadata */
static void
clean_up_page_resources(struct page_metadata *page_metadata)
{
  void *kpage;
  
  if (page_metadata->swapped)
  {
    swap_release (page_metadata->data.swap_sector);
    page_metadata->swapped = false;
  }
  else if (page_metadata->type & PAGE_TYPE_KERNEL)
  {
    /* If it's a kernel page, it must not have been loaded in
       because if it was it would be a zero page. */
    kpage = (void *) page_metadata->data.kpage;
    ASSERT (kpage != NULL);
    palloc_free_page (kpage);
    page_metadata->data.kpage = NULL;
  }
}

/* Identical to frametale_load_frame with the exception that,
   upon return, the frame is locked to prevent it from being evicted. */
bool
frametable_lock_frame(uint32_t *pd, const void *upage, bool write)
{
  return load_frame (pd, upage, write, true);
}

/* Unlocks a frame that was locked with frametable_lock_frame.  NOTE:
   the frame is not unloaded, only unlocked. */
void
frametable_unlock_frame(uint32_t *pd, const void *upage)
{
  struct page_metadata *page_metadata;
  
  ASSERT (is_user_vaddr (upage));
  page_metadata = pagedir_get_info (pd, upage);
  if (page_metadata == NULL)
    return;
  ASSERT (page_metadata->frame != NULL);
  lock_acquire (&frame_lock);
  page_metadata->frame->lock--;
  lock_release (&frame_lock);
}

static bool
load_frame (uint32_t *pd, const void *upage, bool write, bool keep_locked)
{
  struct page_metadata *page_metadata;
  struct frame *frame = NULL;
  bool success = false;

  /* Only holds the frame lock when modifying shared data structures.  Releases
     the lock when doing a I/O operations so other processes can load frames
     that don't require I/O without having to wait. */
  ASSERT (is_user_vaddr (upage));
  page_metadata = pagedir_get_info (pd, upage);
  if (page_metadata == NULL || (write && page_metadata->writable == 0))
    return false;
    
  lock_acquire (&frame_lock);
  /* It's possible the frame could be in the process of being evicted.
     If so, wait for eviction to finish before continuing. When 
     wait_for_io_done returns, frame_lock will be held and frame will be
     NULL. */
  wait_for_io_done (&page_metadata->frame);
  ASSERT (page_metadata->frame == NULL || keep_locked);
  
  if (page_metadata->frame != NULL)
  {
    if (keep_locked)
      page_metadata->frame->lock++;
    lock_release (&frame_lock);
    return true;
  }
  
  /* Attempt to satisfy a read only page by looking it up in the 
     cache. */
  if (page_metadata->type & PAGE_TYPE_FILE
      && page_metadata->writable == 0)
  {
    frame = lookup_read_only_frame (page_metadata);
    if (frame != NULL)
    {
      /* Make sure to map the page before releasing the lock.  If not,
         it's possible that frame could be freed if the final process 
         that maps the frame exits. */
      map_page (page_metadata, frame, upage);
      /* If another process is loading the frame in, wait for it to
         finish.  Lock the frame so it won't get evicted right
         after it's loaded in and before the page is mapped. */
      frame->lock++;
      wait_for_io_done (&frame);
      frame->lock--;
      success = true;
    }
  }
  
  /* Fill a new frame. */
  if (frame == NULL)
  {
    frame = allocate_frame ();
    if (frame != NULL)
    {
      /* Map page to frame and read the data in. */
      map_page (page_metadata, frame, upage);
      handle_io_operations(page_metadata, frame);
      success = true;
    }
  }
  
  if (success && keep_locked)
    frame->lock++;
  lock_release (&frame_lock);
  return success;
}

/* Handles IO operations for loading frame data */
static void
handle_io_operations(struct page_metadata *page_metadata, struct frame *frame)
{
  struct file_info *file_info;
  void *kpage;
  off_t bytes_read;
  
  if (page_metadata->swapped || page_metadata->type & PAGE_TYPE_FILE)
  {
    frame->io = true;
    frame->lock++;
    if (page_metadata->swapped)
    {
      lock_release (&frame_lock);
      swap_read (page_metadata->data.swap_sector, frame->kpage);
      page_metadata->swapped = false;
    }
    else
    {
      if (page_metadata->writable == 0)
      {
        /* Add the read only frame to the cache before the data is read
           from the file to ensure that the next process that tries to
           read it in will wait for the read to complete instead of 
           reading the same data into a new frame. */
        hash_insert (&read_only_frames, &frame->hash_elem);
      }
      file_info = &page_metadata->data.file_info;
      lock_release (&frame_lock);
      bytes_read = file_read_at (file_info->file,
                               frame->kpage,
                               size (file_info->end_offset),
                               offset (file_info->end_offset));
      ASSERT (bytes_read == size (file_info->end_offset));
    }
    lock_acquire (&frame_lock);
    frame->lock--;
    frame->io = false;
    cond_broadcast (&frame->io_done, &frame_lock);
  }
  else if (page_metadata->type & PAGE_TYPE_KERNEL)
  {
    kpage = (void *) page_metadata->data.kpage;
    ASSERT (kpage != NULL);
    memcpy (frame->kpage, kpage, PGSIZE);
    palloc_free_page (kpage);
    page_metadata->data.kpage = NULL;
    /* Change to a zero page now that the data has been copied in. */
    page_metadata->type = PAGE_TYPE_ZERO;
  }
  /* else zero page */
}

static void
frame_init (struct frame *frame)
{
  list_init (&frame->page_metadata_list);
  cond_init (&frame->io_done);
}

static struct frame *
allocate_frame (void)
{
  struct frame *frame;
  void *kpage;
  
  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL)
  {
    frame = calloc (1, sizeof *frame);
    if (frame != NULL)
    {
      frame_init (frame);
      frame->kpage = kpage;
      /* Add the frame to the end of the list so it becomes eligible 
         for eviction. */
      if (!list_empty (&frame_list))
        list_insert (clock_hand, &frame->list_elem);
      else
      {
        list_push_front (&frame_list, &frame->list_elem);
        clock_hand = list_begin (&frame_list);
      }
    }
    else
      palloc_free_page (kpage);
  }
  else
    frame = evict_frame ();
  return frame;
}

static void
map_page (struct page_metadata *page_metadata, struct frame *frame, const void *upage)
{
  page_metadata->frame = frame;
  list_push_back (&frame->page_metadata_list, &page_metadata->elem);
  pagedir_set_page (page_metadata->pd, upage, frame->kpage,
                    page_metadata->writable != 0);
  pagedir_set_dirty (page_metadata->pd, upage, false);
  pagedir_set_accessed (page_metadata->pd, upage, true);
}

static void
wait_for_io_done (struct frame **frame)
{
  while (*frame != NULL && (*frame)->io)
    cond_wait (&(*frame)->io_done, &frame_lock);
}

/* Evicts and returns a free frame. */
static void *
evict_frame (void)
{
  struct frame *frame;

  frame = get_frame_to_evict();
  prepare_frame_for_eviction(frame);
  memset (frame->kpage, 0, PGSIZE);
  return frame;
}

/* Prepares a frame for eviction by handling dirty data and updating metadata */
static void
prepare_frame_for_eviction(struct frame *frame)
{
  struct page_metadata *page_metadata;
  struct file_info *file_info;
  off_t bytes_written;
  block_sector_t swap_sector = 0;
  struct list_elem *e;
  bool dirty = false;

  /* Check if any pages are dirty */
  for (e = list_begin (&frame->page_metadata_list);
       e != list_end (&frame->page_metadata_list); e = list_next (e))
  {
    page_metadata = list_entry (e, struct page_metadata, elem);
    dirty = dirty || pagedir_is_dirty (page_metadata->pd, page_metadata->upage);
    /* Make sure to cause page faults before the data is written out or
       else it's possible for a process to be writing to memory as the
       data is being written or swapped. This could result in lost data. */
    pagedir_clear_page (page_metadata->pd, page_metadata->upage);
  }

  /* Get first page metadata for type checking */
  page_metadata = list_entry (list_front (&frame->page_metadata_list),
                          struct page_metadata, elem);
                          
  /* If a frame is writable to swap, it doesn't matter whether or not the
     frame is dirty it must be written to swap.  There is no other place aside
     from swap to read the data back into a frame. */
  if (dirty || page_metadata->writable & WRITABLE_TO_SWAP)
  {
    ASSERT (page_metadata->writable != 0);
    frame->io = true;
    frame->lock++;
    if (page_metadata->writable & WRITABLE_TO_FILE)
    {
      file_info = &page_metadata->data.file_info;
      lock_release (&frame_lock);
      bytes_written = file_write_at (file_info->file,
                                   frame->kpage,
                                   size (file_info->end_offset),
                                   offset (file_info->end_offset));
      ASSERT (bytes_written == size (file_info->end_offset));          
    }
    else
    {
      lock_release (&frame_lock);
      swap_sector = swap_write (frame->kpage);
    }
    lock_acquire (&frame_lock);
    frame->lock--;
    frame->io = false;
    cond_broadcast (&frame->io_done, &frame_lock);
  }
  else if (page_metadata->type & PAGE_TYPE_FILE && page_metadata->writable == 0)
  {
    ASSERT (hash_find (&read_only_frames, &frame->hash_elem) != NULL);
    hash_delete (&read_only_frames, &frame->hash_elem);
  }
  
  mark_pages_as_swapped(frame, swap_sector);
}

/* Marks all pages in a frame as swapped and updates their metadata */
static void
mark_pages_as_swapped(struct frame *frame, block_sector_t swap_sector)
{
  struct page_metadata *page_metadata;
  struct list_elem *e;
  
  for (e = list_begin (&frame->page_metadata_list);
       e != list_end (&frame->page_metadata_list); )
  {
    page_metadata = list_entry (list_front (&frame->page_metadata_list),
                            struct page_metadata, elem);
    page_metadata->frame = NULL;
    if (page_metadata->writable & WRITABLE_TO_SWAP)
    {
      page_metadata->swapped = true;
      page_metadata->data.swap_sector = swap_sector;
    }
    e = list_remove (e);
  }
}

/* Implementation of the clock page replacement algorithm. A list of frames
   is maintained for eviction.  The "clock hand" points to the next frame to 
   examine.  A frame is eligible for eviction if the access bit is set and it's
   not locked.  If the page is not eligible the access bit is cleared and the
   next frame is examined.  In both cases, the clock hand is moved forward. */ 
static void *
get_frame_to_evict (void)
{
  struct frame *frame;
  struct frame *start;
  struct frame *found = NULL;

  ASSERT (!list_empty (&frame_list));
  start = list_entry (clock_hand, struct frame, list_elem);
  frame = start;
  do
  {
    if (check_frame_eligibility(frame))
      found = frame;
      
    clock_hand = list_next (clock_hand);
    if (clock_hand == list_end (&frame_list))
      clock_hand = list_begin (&frame_list);
    frame = list_entry (clock_hand, struct frame, list_elem);
  } while (!found && frame != start);
  
  if (found == NULL)
  {
    /* Iterated through the entire list and ended up back at the start. */
    ASSERT (frame == start);
    if (frame->lock > 0)
      PANIC ("no frame available for eviction");
    found = frame;
    clock_hand = list_next (clock_hand);
    if (clock_hand == list_end (&frame_list))
      clock_hand = list_begin (&frame_list);
  }

  return found;
}

/* Checks if a frame is eligible for eviction and clears access bits */
static bool
check_frame_eligibility(struct frame *frame)
{
  struct page_metadata *page_metadata;
  struct list_elem *e;
  bool accessed = false;
  
  if (frame->lock == 0)
  {
    accessed = false;
    ASSERT (!list_empty (&frame->page_metadata_list));
    for (e = list_begin (&frame->page_metadata_list);
         e != list_end (&frame->page_metadata_list); e = list_next (e))
    {
      page_metadata = list_entry (e, struct page_metadata, elem);
      accessed = accessed || pagedir_is_accessed (page_metadata->pd,
                                                page_metadata->upage);
      pagedir_set_accessed (page_metadata->pd, page_metadata->upage, false);
    }
    return !accessed;
  }
  return false;
}

static struct frame *
lookup_read_only_frame (struct page_metadata *page_metadata)
{
  struct frame frame;
  struct hash_elem *e;

  list_init (&frame.page_metadata_list);
  list_push_back (&frame.page_metadata_list, &page_metadata->elem);
  e = hash_find (&read_only_frames, &frame.hash_elem);
  return e != NULL ? hash_entry (e, struct frame, hash_elem) : NULL;
}

static unsigned
frame_hash (const struct hash_elem *e, void *aux UNUSED)
{
  struct frame *frame = hash_entry (e, struct frame, hash_elem);
  struct page_metadata *page_metadata;
  block_sector_t sector;

  ASSERT (!list_empty (&frame->page_metadata_list));
  page_metadata = list_entry (list_front (&frame->page_metadata_list),
                          struct page_metadata, elem);
  ASSERT (page_metadata->type & PAGE_TYPE_FILE && page_metadata->writable == 0);
  sector = inode_get_inumber (file_get_inode (page_metadata->data.file_info.file));
  return hash_bytes (&sector, sizeof sector)
    ^ hash_bytes (&page_metadata->data.file_info.end_offset,
                  sizeof page_metadata->data.file_info.end_offset);
}

static bool
frame_less (const struct hash_elem *a_, const struct hash_elem *b_,
            void *aux UNUSED)
{
  struct frame *frame_a = hash_entry (a_, struct frame, hash_elem);
  struct frame *frame_b = hash_entry (b_, struct frame, hash_elem);
  struct page_metadata *page_metadata_a, *page_metadata_b;
  block_sector_t sector_a, sector_b;
  struct inode *inode_a, *inode_b;

  ASSERT (!list_empty (&frame_a->page_metadata_list));
  ASSERT (!list_empty (&frame_b->page_metadata_list));
  page_metadata_a = list_entry (list_front (&frame_a->page_metadata_list),
                            struct page_metadata, elem);
  page_metadata_b = list_entry (list_front (&frame_b->page_metadata_list),
                            struct page_metadata, elem);
  inode_a = file_get_inode (page_metadata_a->data.file_info.file);
  inode_b = file_get_inode (page_metadata_b->data.file_info.file);
  sector_a = inode_get_inumber (inode_a);
  sector_b = inode_get_inumber (inode_b);
  if (sector_a < sector_b)
    return true;
  else if (sector_a > sector_b)
    return false;
  else
    if (page_metadata_a->data.file_info.end_offset
        < page_metadata_b->data.file_info.end_offset)
      return true;
    else
      return false;
}