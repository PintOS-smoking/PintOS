/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "devices/disk.h"

#define SECTORS_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static struct bitmap *swap_bitmap;
static struct lock swap_lock;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void vm_anon_init (void) {
	swap_disk = disk_get (1, 1);

	ASSERT(swap_disk != NULL);

    size_t swap_size = disk_size (swap_disk) / SECTORS_PER_PAGE;
    swap_bitmap = bitmap_create (swap_size);

	ASSERT(swap_bitmap != NULL);

	lock_init(&swap_lock);
}

/* Initialize the file mapping */
bool anon_initializer (struct page *page, enum vm_type type, void *kva) {
	struct anon_page *anon_page = &page->anon;

	page->operations = &anon_ops;
	anon_page->swap_idx = BITMAP_ERROR;
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	size_t swap_idx;
	disk_sector_t start_sector;

	ASSERT (page != NULL);
	ASSERT (page->frame != NULL);

	if (kva == NULL)
		return false;

	swap_idx = anon_page->swap_idx;

	if (swap_idx == BITMAP_ERROR)
		return false;

	lock_acquire (&swap_lock);

	if (!bitmap_test (swap_bitmap, swap_idx)) {
		lock_release (&swap_lock);
		return false;
	}

	start_sector = swap_idx * SECTORS_PER_PAGE;

	for (size_t i = 0; i < SECTORS_PER_PAGE; i++) {
		disk_read (swap_disk, start_sector + i,	kva + DISK_SECTOR_SIZE * i);
	}

	bitmap_reset (swap_bitmap, swap_idx);
	anon_page->swap_idx = BITMAP_ERROR;

	lock_release (&swap_lock);
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	struct frame *frame = page->frame;
	size_t swap_idx;
	disk_sector_t start_sector;
	struct thread *t = page->owner;

	ASSERT (page != NULL);
	ASSERT (frame != NULL);

	if (t == NULL)
		t = thread_current ();

	lock_acquire (&swap_lock);
	swap_idx = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);

	if (swap_idx == BITMAP_ERROR) {
		lock_release (&swap_lock);
		PANIC ("full");
	}

	start_sector = swap_idx * SECTORS_PER_PAGE;

	for (size_t i = 0; i < SECTORS_PER_PAGE; i++) {
		disk_write (swap_disk, start_sector + i, frame->kva + DISK_SECTOR_SIZE * i);
	}

	anon_page->swap_idx = swap_idx;
	lock_release (&swap_lock);

	pml4_clear_page (t->pml4, page->va);
	frame->page = NULL;
	page->frame = NULL;
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy (struct page *page) {
	ASSERT (page != NULL);

	if (page->frame != NULL) {
		struct thread *t = page->owner;

		if (t == NULL)
			t = thread_current ();

		pml4_clear_page (t->pml4, page->va);
		vm_frame_free (page->frame);
		page->frame = NULL;
	}
}
