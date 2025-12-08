/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "threads/malloc.h"
#include "threads/mmu.h"


static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

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

bool file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	page->operations = &file_ops;
	lazy_load_info *aux;

	aux = page->uninit.aux;

	struct file_page *file_page = &page->file;
	file_page->aux = aux;

	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}


static void file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	lazy_load_info *aux = file_page->aux;

	if (pml4_is_dirty(thread_current()->pml4, page->va)) {
		file_write_at(aux->file, page->frame->kva, aux->page_read_bytes, aux->ofs);
		pml4_set_dirty(thread_current()->pml4, page->va, 0);
	}

	if (aux->file) {
		file_close(aux->file);
	}

	free(aux);
}

void *do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {
	void *cur = addr;
	unsigned count = 0;
	struct page *head_page = NULL;

	while (cur - addr < length) {

		if (is_kernel_vaddr(cur) || spt_find_page(&thread_current()->spt, cur) != NULL) {
			return NULL;
		}

		cur += PGSIZE;
		count++;
	}

	cur = addr;
	while (length > 0) {
		struct lazy_load_info* aux = malloc(sizeof *aux);
		size_t page_read_bytes = length < PGSIZE ? length : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        if (aux == NULL) 
            return NULL;

        aux->file = file_reopen(file);  

        if (aux->file == NULL) {
            free(aux);
            return NULL;
        }

        aux->ofs = offset;
		aux->page_read_bytes = page_read_bytes;
		aux->page_zero_bytes = page_zero_bytes;

		if (!vm_alloc_page_with_initializer(VM_FILE, cur, writable, lazy_load_segment, aux)) {
			file_close(aux->file);
			free(aux);
			return NULL;
		}

		if (cur == addr) {
			head_page = spt_find_page(&thread_current()->spt, addr);
			head_page->page_length = count;
		}

		cur += PGSIZE;
		offset += page_read_bytes;
		length -= page_read_bytes;
	}

	return addr;
}

void do_munmap (void *addr) {
	
	struct page *head_page;
	void *current;
	unsigned count;
	struct supplemental_page_table *spt;

	spt = &thread_current()->spt;
	head_page = spt_find_page(spt, addr);
	if (head_page == NULL) {
		return;
	} 

	count = head_page->page_length;
	current = addr;

	for (int i = 0; i < count; i++) {
		struct page *page = spt_find_page(spt, current);
		if (page != NULL) {
			spt_remove_page(spt, page);
		}
		current += PGSIZE;
	}
}
