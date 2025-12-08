/* file.c: Implementation of memory backed file object (mmaped object). */

#include <round.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "vm/file.h"
#include "userprog/syscall.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
static bool lazy_load_file (struct page *page, void *aux);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}
/* Initialize the file backed page */
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	page->operations = &file_ops;
	return true;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	off_t ofs;
	size_t page_read_bytes;

	ASSERT (page != NULL);
	ASSERT (kva != NULL);

	ofs = file_page->ofs;
	page_read_bytes = file_page->read_bytes;

	lock_acquire (&file_lock);
	off_t bytes_read = file_read_at (file_page->file, kva, page_read_bytes, ofs);
	lock_release (&file_lock);

	if (bytes_read != (off_t) page_read_bytes)
		return false;

	memset (kva + page_read_bytes, 0, file_page->zero_bytes);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
	struct thread *t = thread_current ();

	if (page->frame == NULL)
		return true;

	if (pml4_is_dirty (t->pml4, page->va)) {
		lock_acquire (&file_lock);
		file_write_at (file_page->file, page->frame->kva, file_page->read_bytes, file_page->ofs);
		lock_release (&file_lock);
		pml4_set_dirty (t->pml4, page->va, false);
	}

	pml4_clear_page (t->pml4, page->va);
	palloc_free_page (page->frame->kva);
	free (page->frame);
	page->frame = NULL;
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy (struct page *page) {
	file_backed_swap_out (page);
}

/* Do the mmap */
void *do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {
	struct thread *t = thread_current ();
	size_t page_cnt = DIV_ROUND_UP (length, PGSIZE);
	void *upage = addr;
	struct mmap_file *map;
	off_t file_len = file_length (file);

	for (size_t i = 0; i < page_cnt; i++, upage += PGSIZE) {
		if (spt_find_page (&t->spt, upage) != NULL)
			goto fail_file;
	}

	map = calloc (1, sizeof *map);
	if (map == NULL)
		goto fail_file;
	map->start = addr;
	map->page_cnt = page_cnt;
	map->file = file;
	map->offset = offset;
	list_push_back (&t->mmap_list, &map->elem);

	upage = addr;
	for (size_t i = 0; i < page_cnt; i++, upage += PGSIZE) {
		off_t cur_ofs = offset + i * PGSIZE;
		size_t read_bytes = 0;
		if (cur_ofs < file_len) {
			size_t file_left = file_len - cur_ofs;
			read_bytes = file_left >= PGSIZE ? PGSIZE : file_left;
		}
		size_t zero_bytes = PGSIZE - read_bytes;

		struct file_page *aux = malloc (sizeof *aux);
		if (aux == NULL)
			goto fail_map;

		aux->file = file;
		aux->ofs = offset + i * PGSIZE;
		aux->read_bytes = read_bytes;
		aux->zero_bytes = zero_bytes;
		aux->mmap = map;

		if (!vm_alloc_page_with_initializer (VM_FILE, upage, writable, lazy_load_file, aux)) {
			free (aux);
			goto fail_map;
		}
	}
	return addr;

fail_map:
	do_munmap (addr);
	return NULL;

fail_file:
#ifdef USERPROG
	lock_acquire (&file_lock);
#endif
	file_close (file);
#ifdef USERPROG
	lock_release (&file_lock);
#endif
	return NULL;
}


/* Do the munmap */
void do_munmap (void *addr) {
	struct thread *t = thread_current ();
	struct mmap_file *target = NULL;
	struct list_elem *e;

	if (addr == NULL)
		return;

	for (e = list_begin (&t->mmap_list); e != list_end (&t->mmap_list); e = list_next (e)) {
		struct mmap_file *map = list_entry (e, struct mmap_file, elem);
		if (map->start == addr) {
			target = map;
			break;
		}
	}

	if (target == NULL)
		return;

	void *upage = target->start;
	for (size_t i = 0; i < target->page_cnt; i++, upage += PGSIZE) {
		struct page *page = spt_find_page (&t->spt, upage);

		if (page == NULL)
			continue;

		if (page->operations->type == VM_UNINIT) {
			struct file_page *file_info = page->uninit.aux;
			if (file_info != NULL)
				free (file_info);
		}
		spt_remove_page (&t->spt, page);
	}

#ifdef USERPROG
	lock_acquire (&file_lock);
#endif
	file_close (target->file);
#ifdef USERPROG
	lock_release (&file_lock);
#endif
	list_remove (&target->elem);
	free (target);
}

static bool lazy_load_file (struct page *page, void *aux) {
	struct file_page *dst = &page->file;
	struct file_page *src = aux;

	ASSERT (src != NULL);
	ASSERT (page->frame != NULL);

	*dst = *src;
	free (src);
	return file_backed_swap_in (page, page->frame->kva);
}
