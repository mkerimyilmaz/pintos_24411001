#include "filesys/free-map.h"
#include <bitmap.h>
#include <debug.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"

static struct file *map_file;
static struct bitmap *map_bitmap;

void
free_map_init(void)
{
    map_bitmap = bitmap_create(block_size(fs_device));
    if (map_bitmap == NULL)
        PANIC("creation failed");
    bitmap_mark(map_bitmap, FREE_MAP_SECTOR);
    bitmap_mark(map_bitmap, ROOT_DIR_SECTOR);
}

bool
free_map_allocate(size_t number, block_sector_t *sector_out)
{
    size_t start = bitmap_scan(map_bitmap, 0, number, false);
    if (start == BITMAP_ERROR)
        return false;
    bitmap_set_multiple(map_bitmap, start, number, true);
    if (map_file && !bitmap_write(map_bitmap, map_file)) {
        bitmap_set_multiple(map_bitmap, start, number, false);
        return false;
    }
    *sector_out = (block_sector_t) start;
    return true;
}

void
free_map_release(block_sector_t start, size_t number)
{
    ASSERT(bitmap_all(map_bitmap, start, number));
    bitmap_set_multiple(map_bitmap, start, number, false);
    bitmap_write(map_bitmap, map_file);
}

void
free_map_open(void)
{
    map_file = file_open(inode_open(FREE_MAP_SECTOR));
    if (!map_file)
        PANIC("cannot open free map file");
    if (!bitmap_read(map_bitmap, map_file))
        PANIC("cannot read free map file");
}

void
free_map_close(void)
{
    file_close(map_file);
    map_file = NULL;
}

void
free_map_create(void)
{
    if (!inode_create(FREE_MAP_SECTOR,
                      bitmap_file_size(map_bitmap),
                      false))
        PANIC("free map creation failed");

    struct file *f = file_open(inode_open(FREE_MAP_SECTOR));
    if (!f)
        PANIC("can't open free map");
    if (!bitmap_write(map_bitmap, f))
        PANIC("can not write free map");
    map_file = f;
}
