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

    if (swap_bitmap == NULL || !bitmap_test(swap_bitmap, swap_slot_idx)) return false;

    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        disk_sector_t sec_no = (swap_slot_idx * SECTORS_PER_PAGE) + i;
        void* buffer = kva + (DISK_SECTOR_SIZE * i);
        disk_read(swap_disk, sec_no,
                  buffer);  // 물리 메모리의 해당 frame에 디스크에 있는 sector 내용들 작성
    }

    // 다 옮겨 적었으면, 그 해당 slot의 sector들 모두 0으로 채우기(청소)
    bitmap_set(swap_bitmap, swap_slot_idx, false);

    // 해당 page의 기존 데이터 있었던 곳인 swap_disk의 slot_idx=5 기록을 삭제
    anon_page->swap_idx = BITMAP_ERROR;

    return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out(struct page* page) {
    struct anon_page* anon_page = &page->anon;

    // [1] 빈 슬롯 찾기 (예약)
    size_t swap_slot_idx = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);

    // [2] 예외 처리: 디스크 꽉 참
    if (swap_slot_idx == BITMAP_ERROR) {
        return false;
    }

    // [3] 데이터 복사 (메모리 -> 디스크)
    // 4KB(1 Page)를 512B(1 Sector) 단위로 8번 나누어 저장
    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        // 3-1. 디스크 섹터 번호 계산
        disk_sector_t sec_no = (swap_slot_idx * SECTORS_PER_PAGE) + i;

        // 3-2. 메모리 주소 계산 (읽을 위치)
        void* buffer = page->frame->kva + (DISK_SECTOR_SIZE * i);

        // 3-3. 디스크 쓰기
        disk_write(swap_disk, sec_no, buffer);
    }

    // [4] 위치 기록 (영수증)
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
