/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void)
{
}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva)
{
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;

	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in(struct page *page, void *kva)
{
	struct file_page *file_page UNUSED = &page->file;
	return false;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out(struct page *page)
{
	struct file_page *file_page UNUSED = &page->file;
	return false;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page *page)
{
	/* [수정] PML4 정리 로직 추가 */
	if (page->frame != NULL)
	{
		struct thread *t = thread_current();

		/* 1. PML4 매핑 제거 */
		if (pml4_get_page(t->pml4, page->va) != NULL)
		{
			pml4_clear_page(t->pml4, page->va);
		}

		/* 2. 물리 메모리 및 프레임 해제 */
		palloc_free_page(page->frame->kva);
		free(page->frame);
	}

	/* 파일 관련 자원 정리 (file_close 등)가 필요하면 여기에 추가 */
	/* 예: if (page->file.file) file_close(page->file.file); */
	// 주의: aux를 공유하는 경우 닫을 때 주의해야 함 (보통 uninit에서 관리)
}

/* Do the mmap */
void *
do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset)
{
}

/* Do the munmap */
void do_munmap(void *addr)
{
}
