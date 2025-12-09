/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/file.h"

#include <round.h>
#include <string.h>

#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"
#include "vm/vm.h"

static bool file_backed_swap_in(struct page* page, void* kva);
static bool file_backed_swap_out(struct page* page);
static void file_backed_destroy(struct page* page);
bool lazy_load_file(struct page* page, void* aux);
static struct mmap_file* find_mmap(struct thread* t, void* addr);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void) {}
/* Initialize the file backed page */
bool file_backed_initializer(struct page* page, enum vm_type type, void* kva) {
    page->operations = &file_ops;
    return true;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page* page, void* kva) {
    struct file_page* file_page = &page->file;
    off_t ofs;
    size_t page_read_bytes;

    ASSERT(page != NULL);
    ASSERT(kva != NULL);

    ofs = file_page->ofs;
    page_read_bytes = file_page->read_bytes;

    lock_acquire(&file_lock);
    off_t bytes_read = file_read_at(file_page->file, kva, page_read_bytes, ofs);
    lock_release(&file_lock);

    if (bytes_read != (off_t)page_read_bytes) return false;

    memset(kva + page_read_bytes, 0, file_page->zero_bytes);
    return true;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page* page) {
    struct frame* frame = page->frame;
    struct thread* t = thread_current();
    struct file_page* file_page = &page->file;

    if (frame == NULL) return true;

    if (file_page->read_bytes > 0 && pml4_is_dirty(t->pml4, page->va)) {
        lock_acquire(&file_lock);
        file_write_at(file_page->file, frame->kva, file_page->read_bytes, file_page->ofs);
        lock_release(&file_lock);
    }

    pml4_clear_page(t->pml4, page->va);
    // palloc_free_page (frame->kva);
    // free (frame);
    page->frame = NULL;
    return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page* page) { file_backed_swap_out(page); }

void* do_mmap(void* addr, size_t length, int writable, struct file* file, off_t offset) {
    struct thread* t = thread_current();
    size_t page_cnt = DIV_ROUND_UP(length, PGSIZE);
    void* upage = addr;
    struct mmap_file* map;
    off_t file_len = file_length(file);

    for (size_t i = 0; i < page_cnt; i++, upage += PGSIZE) {
        if (spt_find_page(&t->spt, upage) != NULL) goto fail_file;
    }

    map = calloc(1, sizeof *map);

    if (map == NULL) goto fail_file;

    map->start = addr;
    map->page_cnt = page_cnt;
    map->file = file;
    map->offset = offset;
    list_push_back(&t->mmap_list, &map->elem);

    upage = addr;
    for (size_t i = 0; i < page_cnt; i++, upage += PGSIZE) {
        off_t cur_ofs = offset + i * PGSIZE;
        size_t read_bytes = 0;

        if (cur_ofs < file_len) {
            size_t file_left = file_len - cur_ofs;
            read_bytes = file_left >= PGSIZE ? PGSIZE : file_left;
        }

        size_t zero_bytes = PGSIZE - read_bytes;
        struct file_page* aux = malloc(sizeof *aux);

        if (aux == NULL) goto fail_map;

        aux->file = file;
        aux->ofs = offset + i * PGSIZE;
        aux->read_bytes = read_bytes;
        aux->zero_bytes = zero_bytes;
        aux->mmap = map;

        if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable, lazy_load_file, aux)) {
            free(aux);
            goto fail_map;
        }
    }
    return addr;

fail_map:
    do_munmap(addr);
    return NULL;

fail_file:
    lock_acquire(&file_lock);
    file_close(file);
    lock_release(&file_lock);
    return NULL;
}

/* Do the munmap */
void do_munmap(void* addr) {
    struct thread* t = thread_current();
    struct mmap_file* target;

    if (addr == NULL) return;

    target = find_mmap(t, addr);
    if (target == NULL) return;

    void* upage = target->start;
    for (size_t i = 0; i < target->page_cnt; i++, upage += PGSIZE) {
        struct page* page = spt_find_page(&t->spt, upage);

        if (page == NULL) continue;

        spt_remove_page(&t->spt, page);
    }

    lock_acquire(&file_lock);
    file_close(target->file);
    lock_release(&file_lock);
    list_remove(&target->elem);
    free(target);
}

bool lazy_load_file(struct page* page, void* aux) {
    struct file_page* dst = &page->file;
    struct file_page* src = aux;

    ASSERT(src != NULL);
    ASSERT(page->frame != NULL);

    *dst = *src;
    free(src);
    return file_backed_swap_in(page, page->frame->kva);
}

static struct mmap_file* find_mmap(struct thread* t, void* addr) {
    struct list_elem* e = list_begin(&t->mmap_list);

    while (e != list_end(&t->mmap_list)) {
        struct mmap_file* map = list_entry(e, struct mmap_file, elem);
        if (map->start == addr) return map;
        e = list_next(e);
    }
    return NULL;
}
