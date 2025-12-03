/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "devices/disk.h"
#include "vm/vm.h"

#include "threads/mmu.h"
/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in(struct page *page, void *kva);
static bool anon_swap_out(struct page *page);
static void anon_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
    .swap_in = anon_swap_in,
    .swap_out = anon_swap_out,
    .destroy = anon_destroy,
    .type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void vm_anon_init(void) { swap_disk = disk_get(1, 1); }

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva)
{
    struct anon_page *anon_page;

    // 1. 👉 잘못된 인자가 들어오면 즉시 중단
    ASSERT(page != NULL);
    ASSERT(VM_TYPE(type) == VM_ANON);

    /* 2. [핵심] 신분증 교체 (Operations Switch) */
    /* 가장 중요한 줄입니다. 페이지의 동작 방식을 정의하는 함수 테이블을
       'uninit_ops'에서 'anon_ops'로 갈아끼웁니다.
       이제 커널은 이 페이지를 볼 때 "아, 이건 익명 페이지구나"라고 인식합니다. */
    page->operations = &anon_ops;

    /* 3. 익명 페이지 전용 데이터 초기화 */
    /* Union(공용체) 메모리 영역을 이제 'anon_page' 구조체로 사용합니다. */
    anon_page = &page->anon;

    /* 4. 스왑 인덱스 초기화 */
    /* -1 (또는 INVALID_SWAP_IDX)로 설정하여 "이 페이지는 스왑 디스크에 없고 메모리에 있다"고
       명시합니다. 이 과정이 없으면, 이전 uninit 상태일 때의 쓰레기 값이 남아 나중에 버그를
       유발합니다. */
    // anon_page->swap_idx = BITMAP_ERROR; : 이거 왜 인식 안됨?
    anon_page->swap_idx = -1;

    return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page *page, void *kva)
{
    // struct anon_page *anon_page = &page->anon;
    ASSERT(page != NULL);
    ASSERT(page->frame != NULL);
    return true; // 아직 Swap 구현 안 함
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out(struct page *page)
{
    // struct anon_page *anon_page = &page->anon;
    ASSERT(page != NULL);
    return true; // 아직 Swap 구현 안 함
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page *page)
{
    ASSERT(page != NULL);
}