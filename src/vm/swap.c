#include <stdbool.h>
#include <debug.h>
#include <bitmap.h>
#include "threads/vaddr.h"
#include "vm/swap.h"

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

static bool        swap_map_allocate(block_sector_t *sectorp);
static void        swap_map_release(block_sector_t sector);
static block_sector_t get_swap_slot(void);
static void        write_swap_page(block_sector_t slot, const void *kpage);
static void        read_swap_page(block_sector_t slot, void *kpage);

struct block *swap_device;
static struct bitmap *swap_map;

void
swap_init(void)
{
    ASSERT(PGSIZE % BLOCK_SECTOR_SIZE == 0);
    swap_device = block_get_role(BLOCK_SWAP);
    swap_map = bitmap_create(block_size(swap_device) / SECTORS_PER_PAGE);
    if (swap_map == NULL)
        PANIC("bitmap creation failed--swap device is too large");
}

block_sector_t
swap_write(void *kpage)
{
    block_sector_t slot = get_swap_slot();
    write_swap_page(slot, kpage);
    return slot;
}

void
swap_read(block_sector_t slot, void *kpage)
{
    read_swap_page(slot, kpage);
    swap_release(slot);
}

void
swap_release(block_sector_t slot)
{
    swap_map_release(slot);
}

static block_sector_t
get_swap_slot(void)
{
    block_sector_t slot;
    if (!swap_map_allocate(&slot))
        PANIC("no swap space");
    return slot;
}

static void
write_swap_page(block_sector_t slot, const void *kpage)
{
    const uint8_t *ptr = kpage;
    for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
        block_write(swap_device, slot + i, ptr + i * BLOCK_SECTOR_SIZE);
}

static void
read_swap_page(block_sector_t slot, void *kpage)
{
    uint8_t *ptr = kpage;
    for (size_t i = 0; i < SECTORS_PER_PAGE; i++)
        block_read(swap_device, slot + i, ptr + i * BLOCK_SECTOR_SIZE);
}

static bool
swap_map_allocate(block_sector_t *sectorp)
{
    size_t idx = bitmap_scan_and_flip(swap_map, 0, 1, false);
    if (idx == BITMAP_ERROR)
        return false;
    *sectorp = idx * SECTORS_PER_PAGE;
    return true;
}

static void
swap_map_release(block_sector_t sector)
{
    size_t idx = sector / SECTORS_PER_PAGE;
    ASSERT(bitmap_all(swap_map, idx, 1));
    bitmap_set_multiple(swap_map, idx, 1, false);
}
