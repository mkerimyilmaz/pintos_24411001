#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "threads/thread.h"
#include "filesys/filesys.h"
#include "filesys/cache_manager.h"
#include "devices/timer.h"

#define FLAG_IN_USE             0x01
#define FLAG_DIRTY              0x02
#define FLAG_METADATA           0x08

#define CACHE_SIZE              64
#define WRITE_INTERVAL_MS       100

struct prefetch_request {
  block_sector_t sector;
  bool is_meta;
};

static struct cache_entry cache_pool[CACHE_SIZE];
static struct lock cache_lock;
static struct list cache_list;
static struct condition entry_available;

static struct lock prefetch_lock;
static struct prefetch_request prefetch_queue[CACHE_SIZE];
static int queue_head, queue_tail;
static size_t queue_size;
static struct condition prefetch_signal;
static bool stop_prefetch;
static struct semaphore prefetch_done;

static int total_accesses;
static int cache_hits;

static struct cache_entry *find_or_allocate_entry(block_sector_t sector); 
static struct cache_entry *find_dirty_entry(void); 
static void read_into_entry(block_sector_t sector, bool is_meta, struct cache_entry *entry); 
static void flush_dirty_entries(void); 
static void prefetch_thread(void *aux UNUSED); 
static void write_thread(void *aux UNUSED); 

void initialize_cache(void) { 
  int i;
  list_init(&cache_list);
  lock_init(&cache_lock);
  cond_init(&entry_available);
  lock_init(&prefetch_lock);
  cond_init(&prefetch_signal);
  sema_init(&prefetch_done, 0);
  stop_prefetch = false;

  for (i = 0; i < CACHE_SIZE; i++) {
    cache_pool[i].sector = UINT_MAX;
    cache_pool[i].evicting_sector = UINT_MAX;
    cond_init(&cache_pool[i].available);
    cond_init(&cache_pool[i].evicted);
    list_push_back(&cache_list, &cache_pool[i].elem);
  }

  thread_create("prefetch", PRI_DEFAULT, prefetch_thread, NULL);
  thread_create("writeback", PRI_DEFAULT, write_thread, NULL);
}

void shutdown_cache(void) { // previously: shutdown_cache
  lock_acquire(&cache_lock);
  stop_prefetch = true;
  cond_signal(&prefetch_signal, &cache_lock);
  lock_release(&cache_lock);
  sema_down(&prefetch_done);
  flush_dirty_entries();
  printf("cache accesses: %d, hits: %d\n", total_accesses, cache_hits);
}

struct cache_entry *acquire_entry(block_sector_t sector, bool is_meta) { 
  struct cache_entry *entry;
  bool acquired = false;

  lock_acquire(&cache_lock);
  total_accesses++;

  while (!acquired) {
    entry = find_or_allocate_entry(sector);

    if (entry == NULL) {
      cond_wait(&entry_available, &cache_lock);
    } else if (sector == entry->sector) {
      cache_hits++;
      entry->waiting++;
      while (entry->flags & FLAG_IN_USE)
        cond_wait(&entry->available, &cache_lock);
      entry->waiting--;
      entry->flags |= FLAG_IN_USE;
      lock_release(&cache_lock);
      acquired = true;
    } else if (sector == entry->evicting_sector) {
      ASSERT(entry->flags & FLAG_IN_USE);
      while (sector == entry->evicting_sector)
        cond_wait(&entry->evicted, &cache_lock);
    } else {
      read_into_entry(sector, is_meta, entry);
      acquired = true;
    }
  }

  return entry;
}

void release_entry(struct cache_entry *entry, bool dirty) { // previously: release_entry
  ASSERT(entry->flags & FLAG_IN_USE);
  lock_acquire(&cache_lock);

  entry->flags &= ~FLAG_IN_USE;
  if (dirty)
    entry->flags |= FLAG_DIRTY;

  if (entry->waiting > 0)
    cond_signal(&entry->available, &cache_lock);
  else {
    list_remove(&entry->elem);
    list_push_back(&cache_list, &entry->elem);
    cond_signal(&entry_available, &cache_lock);
  }

  lock_release(&cache_lock);
}

void queue_prefetch(block_sector_t sector, bool is_meta) { 
  struct prefetch_request req;

  lock_acquire(&prefetch_lock);
  if (queue_size < CACHE_SIZE) {
    req.sector = sector;
    req.is_meta = is_meta;
    prefetch_queue[queue_head++ % CACHE_SIZE] = req;
    queue_size++;
    cond_signal(&prefetch_signal, &prefetch_lock);
  }
  lock_release(&prefetch_lock);
}

static void read_into_entry(block_sector_t sector, bool is_meta, struct cache_entry *entry) { // previously: load_buffer
  ASSERT(!(entry->flags & FLAG_IN_USE));

  entry->flags |= FLAG_IN_USE;
  if (is_meta)
    entry->flags |= FLAG_METADATA;
  else
    entry->flags &= ~FLAG_METADATA;

  if (entry->flags & FLAG_DIRTY) {
    entry->evicting_sector = entry->sector;
    entry->sector = sector;
    lock_release(&cache_lock);
    block_write(get_fs_device(), entry->evicting_sector, entry->data);
    lock_acquire(&cache_lock);
    entry->evicting_sector = UINT_MAX;
    entry->flags &= ~FLAG_DIRTY;
    cond_signal(&entry->evicted, &cache_lock);
  } else {
    entry->sector = sector;
  }

  lock_release(&cache_lock);
  ASSERT(entry->evicting_sector == UINT_MAX);
  block_read(get_fs_device(), entry->sector, entry->data);
}

static void flush_dirty_entries(void) { // previously: flush_all
  struct cache_entry *entry;

  lock_acquire(&cache_lock);
  entry = find_dirty_entry();
  while (entry != NULL) {
    entry->waiting++;
    while (entry->flags & FLAG_IN_USE)
      cond_wait(&entry->available, &cache_lock);
    entry->waiting--;
    entry->flags |= FLAG_IN_USE;
    lock_release(&cache_lock);

    block_write(get_fs_device(), entry->sector, entry->data);
    entry->flags &= ~FLAG_DIRTY;
    release_entry(entry, false);
    lock_acquire(&cache_lock);
    entry = find_dirty_entry();
  }
  lock_release(&cache_lock);
}

static void prefetch_thread(void *aux UNUSED) { // previously: read_ahead
  struct prefetch_request req;
  struct cache_entry *entry;

  while (true) {
    lock_acquire(&prefetch_lock);
    while (queue_size == 0 && !stop_prefetch)
      cond_wait(&prefetch_signal, &prefetch_lock);
    if (stop_prefetch)
      break;

    req = prefetch_queue[queue_tail++ % CACHE_SIZE];
    queue_size--;
    lock_release(&prefetch_lock);

    lock_acquire(&cache_lock);
    entry = find_or_allocate_entry(req.sector);
    if (entry != NULL && req.sector != entry->sector && req.sector != entry->evicting_sector) {
      read_into_entry(req.sector, req.is_meta, entry);
      release_entry(entry, false);
    } else {
      lock_release(&cache_lock);
    }
  }

  sema_up(&prefetch_done);
  thread_exit();
}

static void write_thread(void *aux UNUSED) { 
  while (true) {
    flush_dirty_entries();
    timer_msleep(WRITE_INTERVAL_MS);
  }
}

static struct cache_entry *find_or_allocate_entry(block_sector_t sector) { 
  struct cache_entry *entry;
  struct cache_entry *meta_candidate = NULL;
  struct cache_entry *data_candidate = NULL;
  struct list_elem *e;

  for (e = list_begin(&cache_list); e != list_end(&cache_list); e = list_next(e)) {
    entry = list_entry(e, struct cache_entry, elem);
    if (sector == entry->sector || sector == entry->evicting_sector)
      return entry;
    else if (entry->waiting == 0 && !(entry->flags & FLAG_IN_USE)) {
      if (entry->flags & FLAG_METADATA) {
        if (meta_candidate == NULL)
          meta_candidate = entry;
      } else if (data_candidate == NULL) {
        data_candidate = entry;
      }
    }
  }
  return data_candidate != NULL ? data_candidate : meta_candidate;
}

static struct cache_entry *find_dirty_entry(void) { 
  struct cache_entry *entry;

  struct list_elem *e;

  for (e = list_begin(&cache_list); e != list_end(&cache_list); e = list_next(e)) {
    entry = list_entry(e, struct cache_entry, elem);
    if ((entry->flags & FLAG_DIRTY) && entry->evicting_sector == UINT_MAX)
      return entry;
  }
  return NULL;
}
