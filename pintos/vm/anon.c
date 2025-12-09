/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static struct bitmap *swap_table;
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


void vm_anon_init (void) {
	swap_disk = disk_get (1, 1);
    if (swap_disk == NULL) {
        return;
    }

    swap_table = bitmap_create(disk_size(swap_disk)/8);
}

/* Initialize the file mapping */
bool anon_initializer (struct page *page, enum vm_type type, void *kva) {
	struct anon_page *anon_page;
	
	page->operations = &anon_ops;
    anon_page = &page->anon;
    anon_page->swap_idx = -1;
    return true;
}

static bool anon_swap_in (struct page *page, void *kva) {
	size_t disk_index;
	
	ASSERT (page != NULL);
	ASSERT (page->frame != NULL);

	disk_index = page->anon.swap_idx;
	if (disk_index == -1) {
		return false;
	}


	for (int i = 0; i < 8; i++) {
		disk_read(swap_disk, disk_index * 8 + i, page->frame->kva + DISK_SECTOR_SIZE * i);
	}

	bitmap_set (swap_table, disk_index, false);
	page->anon.swap_idx = -1;

	return true;
}

static bool anon_swap_out (struct page *page) {
	size_t disk_index;

	ASSERT (page != NULL);

	disk_index = bitmap_scan_and_flip(swap_table, 0, 1, false);
	if (disk_index == -1) {
		return false;
	}

	for (int i = 0; i < 8; i++) {
		disk_write(swap_disk, disk_index * 8 + i, page->frame->kva + DISK_SECTOR_SIZE * i);
	}

	page->anon.swap_idx = disk_index;
	pml4_clear_page(thread_current()->pml4, page->va);
	page->frame = NULL;

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;

	ASSERT (page != NULL);

	if (page->frame != NULL) {
		struct thread *t = thread_current ();

		pml4_clear_page (t->pml4, page->va);
		palloc_free_page (page->frame->kva);
		free (page->frame);
		page->frame = NULL;
	}
}
