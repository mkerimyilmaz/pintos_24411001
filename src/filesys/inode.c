#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <limits.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache_manager.h"
#include "threads/malloc.h"

#define NDIRECT_SECTORS   124
#define NINDIRECT_SECTORS 128
#define NDIRECT_BYTES     (NDIRECT_SECTORS * BLOCK_SECTOR_SIZE)
#define NDINDIRECT_BYTES  (NINDIRECT_SECTORS * BLOCK_SECTOR_SIZE);

/* Identifies an inode. */
#define INODE_MAGIC 0x494e

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  block_sector_t start;               /* First data sector. */
  off_t length;                       /* File size in bytes. */
  unsigned short magic;               /* Magic number. */
  unsigned short is_directory_flag;              

  block_sector_t sectors[NDIRECT_SECTORS + 1];
};

static off_t update_length (struct inode *inode, off_t offset);

static inline size_t
direct_sector_idx (off_t pos)
{
  return pos / BLOCK_SECTOR_SIZE;
}

static inline size_t
indirect_sector_idx (off_t pos)
{
  return (pos - NDIRECT_BYTES) / NDINDIRECT_BYTES;
}

static inline size_t
dindirect_sector_idx (off_t pos)
{
  return (pos - NDIRECT_BYTES) / BLOCK_SECTOR_SIZE % NINDIRECT_SECTORS;
}

/* In-memory inode. */
struct inode 
{
  struct list_elem elem;              /* Element in inode list. */
  block_sector_t sector;              /* Sector number of disk location. */
  int open_cnt;                       /* Number of openers. */
  bool removed;                       /* True if deleted, false otherwise. */
  int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
  struct lock lock;                   
  struct inode_disk *data;            /* Inode content. */
};


   static block_sector_t
   ensure_child_sector(block_sector_t parent, size_t index, bool parent_is_inode)
   {
       struct cache_entry *buf = acquire_entry(parent, true);
       struct inode_disk *disk =
         parent_is_inode
         ? (struct inode_disk *) buf->data
         : (struct inode_disk *) buf->data; 
   
       block_sector_t child = disk->sectors[index];
       if (child == 0)
       {
           release_entry(buf, false);
           if (!free_map_allocate(1, &child))
               return 0;             
           buf = acquire_entry(parent, true);
           disk = (struct inode_disk *) buf->data;
           disk->sectors[index] = child;
           release_entry(buf, true);
   
           buf = acquire_entry(child, true);
           memset(buf->data, 0, BLOCK_SECTOR_SIZE);
           release_entry(buf, true);
       }
       else
       {
           release_entry(buf, false);
       }
       return child;
   }
   
   static bool
   byte_to_sector(struct inode *inode, bool is_directory_flag, off_t pos,
                  block_sector_t *out_sector)
   {
       ASSERT(inode != NULL);
       ASSERT(pos < MAX_FILE_SIZE);
   
       if (!is_directory_flag)
           lock_acquire(&inode->lock);
   
       size_t idx0 = direct_sector_idx(pos);
       if (idx0 > NDIRECT_SECTORS) idx0 = NDIRECT_SECTORS;
   
       block_sector_t sector = ensure_child_sector(inode->sector, idx0, true);
       if (sector == 0)
           goto fail;
       if (idx0 < NDIRECT_SECTORS)
       {
           *out_sector = sector;
           goto success;
       }
   
       size_t idx1 = indirect_sector_idx(pos);
       sector = ensure_child_sector(sector, idx1, false);
       if (sector == 0)
           goto fail;
   
       size_t idx2 = dindirect_sector_idx(pos);
       sector = ensure_child_sector(sector, idx2, false);
       if (sector == 0)
           goto fail;
   
       *out_sector = sector;
   
   success:
       if (!is_directory_flag)
           lock_release(&inode->lock);
       return true;
   
   fail:
       if (!is_directory_flag)
           lock_release(&inode->lock);
       return false;
   }
   
/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
static struct lock inodes_lock;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&inodes_lock);
}

bool
inode_create (block_sector_t sector, off_t length, bool is_directory_flag)
{
  struct inode_disk *disk_inode = NULL;
  struct cache_entry *cache_entry;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_directory_flag = is_directory_flag;
      cache_entry = acquire_entry (sector, true);
      memcpy (cache_entry->data, disk_inode, sizeof *disk_inode);
      release_entry (cache_entry, true);
      success = true; 
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode = NULL;

  lock_acquire (&inodes_lock);
  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode->open_cnt++;
          goto done;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    goto done;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->lock);

 done:
  lock_release (&inodes_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  lock_acquire (&inodes_lock);
  if (inode != NULL)
    inode->open_cnt++;
  lock_release (&inodes_lock);
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

//todo kerim
static void
free_indirect_block(block_sector_t sector, size_t count) {
    if (sector == 0)
        return;
    struct cache_entry *buf = acquire_entry(sector, true);
    block_sector_t *entries = (block_sector_t *)buf->data;
    for (size_t k = 0; k < count; k++) {
        if (entries[k] != 0)
            free_map_release(entries[k], 1);
    }
    release_entry(buf, false);
    free_map_release(sector, 1);
}

static void
release_all_blocks(struct inode *inode) {
    struct cache_entry *meta = acquire_entry(inode->sector, true);
    struct inode_disk *disk = (struct inode_disk *)meta->data;

    for (size_t i = 0; i < NDIRECT_SECTORS; i++) {
        if (disk->sectors[i] != 0)
            free_map_release(disk->sectors[i], 1);
    }

    block_sector_t single = disk->sectors[NDIRECT_SECTORS];
    release_entry(meta, false);
    free_indirect_block(single, NINDIRECT_SECTORS);

    if (single != 0) {
        struct cache_entry *indirect_buf = acquire_entry(single, true);
        block_sector_t *indirect_entries = (block_sector_t *)indirect_buf->data;
        for (size_t i = 0; i < NINDIRECT_SECTORS; i++) {
            free_indirect_block(indirect_entries[i], NINDIRECT_SECTORS);
        }
        release_entry(indirect_buf, false);
    }
}

void
inode_close(struct inode *inode) 
{
    if (inode == NULL)
        return;

    lock_acquire(&inodes_lock);
    if (--inode->open_cnt > 0) {
        lock_release(&inodes_lock);
        return;
    }

    list_remove(&inode->elem);
    lock_release(&inodes_lock);

    if (inode->removed) {
        release_all_blocks(inode);
        free_map_release(inode->sector, 1);
    }
    free(inode);
}


void
inode_lock (struct inode *inode )
{
  lock_acquire (&inode->lock);
}

void
inode_unlock (struct inode *inode)
{
  lock_release (&inode->lock);
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  struct cache_entry *cached_buffer;
  uint8_t *cache_entry = buffer_;
  off_t bytes_read = 0;
  off_t length;
  bool is_directory_flag;
  off_t new_offset;
  block_sector_t sector;

  if (size <= 0)
    return 0;
  length = inode_length (inode);
  is_directory_flag = inode_is_dir (inode);
  if (offset >= length)
    return 0;
  while (size > 0) 
    {
      if (!byte_to_sector (inode, is_directory_flag, offset, &sector))
        break;
      
      /* Disk sector to read, starting byte offset within sector. */
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      cached_buffer = acquire_entry (sector, false);
      memcpy (buffer_ + bytes_read, cached_buffer->data + sector_ofs,
              chunk_size);          
      release_entry (cached_buffer, false);
          
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  /* If possible, read ahead the next sector so it's cached for a sequential
     read. */
  new_offset = offset + BLOCK_SECTOR_SIZE - 1;
  if (size == 0 && new_offset > offset && new_offset < length
      && byte_to_sector (inode, is_directory_flag, new_offset, &sector))
    queue_prefetch (sector, false);
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs. */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  struct cache_entry *cached_buffer;
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  off_t length;
  bool is_directory_flag;
  off_t new_offset;
  block_sector_t sector;

  if (inode->deny_write_cnt || size <= 0)
    return 0;
  is_directory_flag = inode_is_dir (inode);
  while (size > 0) 
    {
      if (!byte_to_sector (inode, is_directory_flag, offset, &sector))
        break;

      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = MAX_FILE_SIZE - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      
      cached_buffer = acquire_entry (sector, false);
      memcpy (cached_buffer->data + sector_ofs, buffer + bytes_written,
              chunk_size);
      release_entry (cached_buffer, true);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  /* If the file was extended, update the length. */
  length = update_length (inode, offset);
  /* If possible, read ahead the next sector so it's cached for a sequential
     write. */
  new_offset = offset + BLOCK_SECTOR_SIZE - 1;
  if (size == 0 && new_offset > offset && new_offset < length
      && byte_to_sector (inode, is_directory_flag, new_offset, &sector))
    queue_prefetch (sector, false);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (struct inode *inode)
{
  struct cache_entry *buffer;
  off_t length;
  
  buffer = acquire_entry (inode->sector, true);
  inode->data = (struct inode_disk *) buffer->data;
  length = inode->data->length;
  release_entry (buffer, false);
  return length;
}

/* Returns true if the inode is a directory, false if it's a file. */
bool
inode_is_dir (struct inode *inode)
{
  struct cache_entry *buffer;
  bool is_directory_flag;
  
  buffer = acquire_entry (inode->sector, true);
  inode->data = (struct inode_disk *) buffer->data;
  is_directory_flag = inode->data->is_directory_flag;
  release_entry (buffer, false);
  return is_directory_flag;
}

bool
inode_is_removed (struct inode *inode)
{
  return inode->removed;
}

static off_t
update_length (struct inode *inode, off_t offset)
{
  struct cache_entry *buffer;
  off_t length;
  
  buffer = acquire_entry (inode->sector, true);
  inode->data = (struct inode_disk *) buffer->data;
  length = inode->data->length;
  if (offset > length)
    {
      length = offset;
      inode->data->length = length;
      release_entry (buffer, true);
    }
  else
    release_entry (buffer, false);
  return length;
}
