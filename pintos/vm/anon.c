/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "devices/disk.h"
#include "vm/vm.h"
#define SECTORS_PER_PAGE 8

#include "lib/kernel/bitmap.h"
#include "threads/malloc.h"
#include "threads/mmu.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk* swap_disk;
static bool anon_swap_in(struct page* page, void* kva);
static bool anon_swap_out(struct page* page);
static void anon_destroy(struct page* page);

static struct disk* swap_disk;
static struct bitmap* swap_bitmap;

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
    .swap_in = anon_swap_in,
    .swap_out = anon_swap_out,
    .destroy = anon_destroy,
    .type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void vm_anon_init(void) {
    swap_disk = disk_get(1, 1);
    size_t swap_size = disk_size(swap_disk) / SECTORS_PER_PAGE;
    swap_bitmap = bitmap_create(swap_size);
}

/* Initialize the file mapping */
bool anon_initializer(struct page* page, enum vm_type type, void* kva) {
    struct anon_page* anon_page = &page->anon;
    page->operations = &anon_ops;
    anon_page->swap_idx = BITMAP_ERROR;
    return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page* page, void* kva) {
    ASSERT(page != NULL);
    ASSERT(page->frame != NULL);

    struct anon_page* anon_page = &page->anon;

    size_t swap_slot_idx = anon_page->swap_idx;

    if (swap_bitmap == NULL || !bitmap_test(swap_bitmap, swap_slot_idx))
        return false;

    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        disk_sector_t sec_no = (swap_slot_idx * SECTORS_PER_PAGE) + i;
        void* buffer = kva + (DISK_SECTOR_SIZE * i);
        disk_read(swap_disk, sec_no, buffer);
    }

    bitmap_set(swap_bitmap, swap_slot_idx, false);

    anon_page->swap_idx = BITMAP_ERROR;

    return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out(struct page* page) {
    struct anon_page* anon_page = &page->anon;

    size_t swap_slot_idx = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);

    if (swap_slot_idx == BITMAP_ERROR)
        return false;

    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        disk_sector_t sec_no = (swap_slot_idx * SECTORS_PER_PAGE) + i;

        void* buffer = page->frame->kva + (DISK_SECTOR_SIZE * i);

        disk_write(swap_disk, sec_no, buffer);
    }

    anon_page->swap_idx = swap_slot_idx;

    return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page* page) {
    struct anon_page* anon_page = &page->anon;

    ASSERT(page != NULL);

    if (page->frame != NULL) {
        struct thread* t = thread_current();

        pml4_clear_page(t->pml4, page->va);
        palloc_free_page(page->frame->kva);
        list_remove(&page->frame->frame_table_elem);
        free(page->frame);
        page->frame = NULL;
    }
}
