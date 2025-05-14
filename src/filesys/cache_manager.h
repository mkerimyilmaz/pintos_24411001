#ifndef FILESYS_BUFFER_H
#define FILESYS_BUFFER_H

#include <stdint.h>
#include <list.h>
#include "threads/synch.h"
#include "devices/block.h"

struct cache_entry
{
  uint8_t flags;
  block_sector_t sector;
  block_sector_t evicting_sector;
  unsigned waiting;
  struct condition available;
  struct condition evicted;
  uint8_t data[BLOCK_SECTOR_SIZE];
  struct list_elem elem;
};

void initialize_cache (void);
void shutdown_cache (void);
struct cache_entry *acquire_entry (block_sector_t sector, bool is_meta);
void release_entry (struct cache_entry *buffer, bool dirty);
void queue_prefetch (block_sector_t sector, bool is_meta);

#endif /* filesys/buffer.h */
