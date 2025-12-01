/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/inspect.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void) {
    vm_anon_init();
    vm_file_init();
#ifdef EFILESYS /* For project 4 */
    pagecache_init();
#endif
    register_inspect_intr();
    /* DO NOT MODIFY UPPER LINES. */
    /* TODO: Your code goes here. */

    /* frame table 과 frame_lock 초가화 */
    list_init(&frame_table);
    lock_init(&frame_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page* page) {
    int ty = VM_TYPE(page->operations->type);
    switch (ty) {
        case VM_UNINIT:
            return VM_TYPE(page->uninit.type);
        default:
            return ty;
    }
}

/* static 전역 변수 */
static struct list frame_table;
static struct lock frame_lock;

/* Helpers */
static struct frame* vm_get_victim(void);
static bool vm_do_claim_page(struct page* page);
static struct frame* vm_evict_frame(void);
static uint64_t page_hash(const struct hash_elem* e, void* aux);
static bool page_less(const struct hash_elem* a, const struct hash_elem* b, void* aux);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
/* 개별 페이지 생성 */
/* 가상 주소(VA)에 해당되는 실제 page가 아닌, struct page를 조립하는 함수 */
bool vm_alloc_page_with_initializer(enum vm_type type, void* upage, bool writable,
                                    vm_initializer* init, void* aux) {
    ASSERT(VM_TYPE(type) != VM_UNINIT)

    struct supplemental_page_table* spt = &thread_current()->spt;
    struct page* page = NULL;  // 에러 처리 시 free를 위해 초기화

    /* 1. 이미 같은 주소(upage)에 페이지가 존재하는지 확인 */
    if (spt_find_page(spt, upage) == NULL) {
        /* 2. struct page 구조체를 담을 메모리를 커널 힙에 할당 */
        page = (struct page*)malloc(sizeof(struct page));
        if (page == NULL) goto err;  // 메모리 부족 등으로 할당 실패 시

        /* 3. 타입에 맞춰 uninit 페이지로 초기화 */
        /* 이 시점에서 페이지는 'UNINIT' 상태가 되며, 나중에 접근 시 type으로 변신할 준비를 함 */
        switch (VM_TYPE(type)) {
            case VM_ANON:
                // anon_initializer: 나중에 실제 물리 프레임과 연결될 때 호출될 함수
                uninit_new(page, upage, init, type, aux, anon_initializer);
                break;
            case VM_FILE:
                // file_map_initializer: 파일 기반 페이지가 생성될 때 호출될 함수
                uninit_new(page, upage, init, type, aux, file_backed_initializer);
                break;
            default:
                goto err;  // 지원하지 않는 타입
        }

        /* 4. 쓰기 권한(writable) 설정 (직접 멤버에 기록) */
        page->writable = writable;

        /* 5. 보조 페이지 테이블(SPT)에 페이지 등록 */
        if (!spt_insert_page(spt, page)) {
            goto err;  // 해시 테이블 삽입 실패 (충돌 등)
        }

        return true;  // 성공적으로 페이지 등록 완료
    }

err:
    /* 실패 시, 만약 page 구조체가 할당되었다면 해제하여 메모리 누수 방지 */
    if (page) free(page);
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page* spt_find_page(struct supplemental_page_table* spt UNUSED, void* va UNUSED) {
    /* TODO: Fill this function. */
    struct page dummy_page;
    struct hash_elem* elem;

    if (spt == NULL || va == NULL) return NULL;

    dummy_page.va = pg_round_down(va);
    elem = hash_find(&spt->hash_table, &dummy_page.hash_elem);

    if (elem == NULL) return NULL;

    return hash_entry(elem, struct page, hash_elem);
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table* spt, struct page* page) {
    if (spt == NULL || page == NULL || page->va == NULL) return false;

    // page->va = pg_round_down (page->va);
    return hash_insert(&spt->hash_table, &page->hash_elem) == NULL;
}

bool spt_remove_page(struct supplemental_page_table* spt, struct page* page) {
    struct hash_elem* result;

    if (spt == NULL || page == NULL) return;

    result = hash_delete(&spt->hash_table, &page->hash_elem);

    if (result != NULL) {
        vm_dealloc_page(page);
        return true;
    }
    return false;
}

/* Get the struct frame, that will be evicted. */
static struct frame* vm_get_victim(void) {
    struct frame* victim = NULL;
    /* TODO: The policy for eviction is up to you. */

    return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame* vm_evict_frame(void) {
    struct frame* victim UNUSED = vm_get_victim();
    /* TODO: swap out the victim and return the evicted frame. */

    return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame* vm_get_frame(void) {
    struct frame* frame = NULL;

    void* kva = palloc_get_page(PAL_USER | PAL_ZERO);
    if (kva == NULL) {
        PANIC("todo");
    }

    frame = (struct frame*)malloc(sizeof(struct frame));
    if (frame == NULL) PANIC("Frame Malloc Failed");

    frame->kva = kva;
    frame->page = NULL;

    lock_acquire(&frame_lock);
    list_push_back(&frame_table, &frame->frame_elem);
    lock_release(&frame_lock);

    return frame;
}

/* Growing the stack. */
static void vm_stack_growth(void* addr UNUSED) {}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page* page UNUSED) {}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame* f UNUSED, void* addr UNUSED, bool user UNUSED,
                         bool write UNUSED, bool not_present UNUSED) {
    struct supplemental_page_table* spt UNUSED = &thread_current()->spt;
    struct page* page = NULL;
    /* TODO: Validate the fault */
    /* TODO: Your code goes here */

    return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page* page) {
    destroy(page);
    free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void* va UNUSED) {
    struct page* page = NULL;
    /* TODO: Fill this function */

    return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page(struct page* page) {
    // vm_get_frame으로
    struct frame* frame = vm_get_frame();
    if (frame == NULL) return false;

    /* Set links */
    frame->page = page;
    page->frame = frame;

    /* TODO: Insert page table entry to map page's VA to frame's PA. */
    /* 3. Physical Memory의 User Pool에 frame과
          Virtual Address Space의 User Space에 page의 1 : 1 매핑 */
    struct thread* cur = thread_current();
    if (!pml4_set_page(cur->pml4, page->va, frame->kva, page->writable)) return false;

    /* 3. 데이터 로딩 (Swap In) */
    // 현재는 Anonymous Page(Stack/Heap) 위주이므로 true 반환으로 충분하지만,
    // 추후 File Loading 등을 위해 swap_in 구조를 살리는 것이 정석입니다.
    // return swap_in(page, frame->kva);
    return true;
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table* spt UNUSED) {
    hash_init(&spt->hash_table, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table* dst UNUSED,
                                  struct supplemental_page_table* src UNUSED) {}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table* spt UNUSED) {
    /* TODO: Destroy all the supplemental_page_table hold by thread and
     * TODO: writeback all the modified contents to the storage. */
}

static uint64_t page_hash(const struct hash_elem* e, void* aux UNUSED) {
    const struct page* page;
    page = hash_entry(e, struct page, hash_elem);
    return hash_bytes(&page->va, sizeof page->va);
}

static bool page_less(const struct hash_elem* a, const struct hash_elem* b, void* aux UNUSED) {
    const struct page *page_a, *page_b;
    page_a = hash_entry(a, struct page, hash_elem);
    page_b = hash_entry(b, struct page, hash_elem);
    return page_a->va < page_b->va;
}
