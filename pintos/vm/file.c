/* file.c: Implementation of memory backed file object (mmaped object). */

#include "filesys/file.h"

#include "include/threads/mmu.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "vm/file.h"
#include "vm/vm.h"

void* do_mmap(void* addr, size_t length, int writable, struct file* file, off_t offset);

static bool file_backed_swap_in(struct page* page, void* kva);
static bool file_backed_swap_out(struct page* page);
static void file_backed_destroy(struct page* page);

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
    /* Set up the handler */
    page->operations = &file_ops;

    struct file_page* file_page = &page->file;

    return true;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page* page, void* kva) {
    struct file_page* file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page* page) {
    struct file_page* file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page* page) {
    struct file_page* file_page UNUSED = &page->file;
}

/* Do the mmap */
void* do_mmap(void* addr, size_t length, int writable, struct file* file, off_t offset) {
    void* org_addr = addr;
    struct file* reopen_file = NULL;
    size_t total_length = file_length(file);
    struct file_page* file_aux = NULL;

    for (size_t i = 0; i < length; i += PGSIZE)
        if (spt_find_page(&thread_current()->spt, addr + i) != NULL)
            return NULL;

    reopen_file = file_reopen(file);
    if (reopen_file == NULL)
        return NULL;

    size_t read_bytes = (length > total_length) ? total_length : length;
    size_t zero_bytes = length - read_bytes;

    while (read_bytes > 0 || zero_bytes > 0) {
        // 한 페이지 당 읽을 bytes 양
        size_t page_read_bytes = (read_bytes < PGSIZE) ? read_bytes : PGSIZE;
        // 한 페이지 당 0으로 채울 bytes 양
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        // aux 할당 및 초기화
        file_aux = malloc(sizeof(struct file_page));
        if (file_aux == NULL)
            return NULL;

        file_aux->file = reopen_file;
        file_aux->page_read_bytes = page_read_bytes;
        file_aux->page_zero_bytes = page_zero_bytes;
        file_aux->ofs = offset;

        if (!vm_alloc_page_with_initializer(VM_FILE, addr, writable, lazy_load_segment, file_aux)) {
            free(file_aux);
            return NULL;
        }

        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;

        addr += PGSIZE;
        offset += page_read_bytes;
    }

    return org_addr;
}

/* Do the munmap */
void do_munmap(void* addr) {
    struct supplemental_page_table* spt = &thread_current()->spt;

    while (true) {
        struct page* page = spt_find_page(spt, addr);

        if (page == NULL)
            break;

        struct file_page* aux = (struct file_page*)page->uninit.aux;

        if (pml4_is_dirty(thread_current()->pml4, page->va)) {
            file_write_at(aux->file, addr, aux->page_read_bytes, aux->ofs);
            pml4_set_dirty(thread_current()->pml4, page->va, 0);
        }

        // 4. 하드웨어 매핑 끊기
        pml4_clear_page(thread_current()->pml4, page->va);

        // 5. SPT 및 리소스 제거
        spt_remove_page(spt, page);

        // 6. 다음 페이지로 이동
        addr += PGSIZE;
    }
}
