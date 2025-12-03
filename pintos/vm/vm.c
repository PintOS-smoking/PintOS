/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/inspect.h"

typedef bool vm_initializer(struct page *, void *aux);

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void)
{
    vm_anon_init();
    vm_file_init();
#ifdef EFILESYS /* For project 4 */
    pagecache_init();
#endif
    register_inspect_intr();
    /* DO NOT MODIFY UPPER LINES. */
    /* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page *page)
{
    int ty = VM_TYPE(page->operations->type);
    switch (ty)
    {
    case VM_UNINIT:
        return VM_TYPE(page->uninit.type);
    default:
        return ty;
    }
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);
static uint64_t page_hash(const struct hash_elem *e, void *aux);
static bool page_less(const struct hash_elem *a, const struct hash_elem *b,
                      void *aux);

bool vm_alloc_page_with_initializer(enum vm_type type, void *upage,
                                    bool writable, vm_initializer *init,
                                    void *aux)
{
    ASSERT(VM_TYPE(type) != VM_UNINIT)

    struct supplemental_page_table *spt = &thread_current()->spt;

    if (spt_find_page(spt, upage) == NULL)
    {
        struct page *page = malloc(sizeof *page);
        bool (*initializer)(struct page *, enum vm_type, void *) = NULL;

        if (page == NULL)
            goto err;

        switch (VM_TYPE(type))
        {
        case VM_ANON:
            initializer = anon_initializer;
            break;
        case VM_FILE:
            initializer = file_backed_initializer;
            break;
        default:
            free(page);
            goto err;
        }

        uninit_new(page, upage, init, type, aux, initializer);
        page->writable = writable;

        if (!spt_insert_page(spt, page))
        {
            free(page);
            goto err;
        }
        return true;
    }
err:
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *spt_find_page(struct supplemental_page_table *spt, void *va)
{
    /* TODO: Fill this function. */
    struct page dummy_page;
    struct hash_elem *elem;

    if (spt == NULL || va == NULL)
        return NULL;

    dummy_page.va = pg_round_down(va);
    elem = hash_find(&spt->hash_table, &dummy_page.hash_elem);

    if (elem == NULL)
        return NULL;

    return hash_entry(elem, struct page, hash_elem);
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page)
{
    if (spt == NULL || page == NULL || page->va == NULL)
        return false;

    // page->va = pg_round_down (page->va);
    return hash_insert(&spt->hash_table, &page->hash_elem) == NULL;
}

bool spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
    struct hash_elem *result;

    if (spt == NULL || page == NULL)
        return;

    result = hash_delete(&spt->hash_table, &page->hash_elem);

    if (result != NULL)
    {
        vm_dealloc_page(page);
        return true;
    }
    return false;
}

/* Get the struct frame, that will be evicted. */
static struct frame *vm_get_victim(void)
{
    struct frame *victim = NULL;
    /* TODO: The policy for eviction is up to you. */

    return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void)
{
    struct frame *victim UNUSED = vm_get_victim();
    /* TODO: swap out the victim and return the evicted frame. */

    return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *vm_get_frame(void)
{
    struct frame *frame = NULL;
    void *kva;

    kva = palloc_get_page(PAL_USER);
    if (kva == NULL)
    {
        PANIC("todo");
    }

    frame = malloc(sizeof(struct frame));
    if (frame == NULL)
    {
        palloc_free_page(kva);
        return NULL;
    }

    frame->kva = kva;
    frame->page = NULL;

    ASSERT(frame != NULL);
    ASSERT(frame->page == NULL);
    return frame;
}

/* Growing the stack. */
static void vm_stack_growth(void *addr UNUSED) {}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page UNUSED) {}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED,
                         bool user UNUSED, bool write UNUSED,
                         bool not_present UNUSED)
{
    struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
    struct page *page = NULL;

    /* TODO: Validate the fault */
    /* TODO: Your code goes here */

    // 1. 유저 모드에서 읽다가 page fault가 나는게 정상인데,
    // 만약, 커널모드이거나 이상한 주소라면 걍 종료
    if (is_kernel_vaddr(addr) || addr == NULL)
        return false;
    // 2. spt에서 해당 주소에 대한 page 검색
    // 유효한 page가 있다면, lazy_load_segment
    // 없다면, segment default
    page = spt_find_page(spt, addr);

    // 페이지가 없으면 처리 불가 (Stack Growth 실패 포함) */
    if (page == NULL)
    {
        /* [Step A] 현재 스택 포인터(rsp) 확인 */
        /* user가 true면 유저 모드에서 발생했으므로 f->rsp가 맞음.
           user가 false면 커널 모드(시스템 콜 중)에서 발생했으므로
           스레드에 저장된 tf.rsp를 가져와야 함. */
        uintptr_t rsp = user ? f->rsp : thread_current()->tf.rsp;

        /* [Step B] 스택 확장 조건 검사 (Heuristic) */
        /* 1. addr이 rsp보다 8바이트 아래(PUSH 명령어 감안) 이상인가?
           2. addr이 유저 스택 상한선(USER_STACK) 아래인가?
           3. addr이 스택 최대 크기(1MB) 범위 내인가? */
        if (addr >= (void *)(rsp - 8) && addr <= (void *)USER_STACK && addr >= (void *)((uint8_t *)USER_STACK - STACK_LIMIT))
        {
            /* [Step C] 페이지 할당 (Marking) */
            /* 해당 주소를 포함하는 페이지 시작점 계산 */
            void *page_addr = pg_round_down(addr);

            /* 스택 페이지 생성 (VM_ANON | VM_MARKER_0) */
            if (!vm_alloc_page_with_initializer(VM_ANON | VM_MARKER_0,
                                                page_addr, true, NULL, NULL))
                return false;

            /* [Step D] 물리 메모리 연결 (Claim) */
            /* 방금 만든 페이지를 바로 사용해야 하므로 즉시 Claim */
            if (!vm_claim_page(page_addr))
            {
                return false;
            }

            /* 스택 확장 성공! */
            return true;
        }

        /* 스택 확장 조건도 아니면 진짜 오류(Segfault) */
        return false;
    }

    /* [추가] 권한 위반(Protection Violation) 체크 */
    /* not_present가 false라면: 페이지는 존재(PML4에 있음)하는데 Fault 발생 ->
       권한 위반 write가 true(쓰기 시도)인데 page->writable이 false(읽기
       전용)라면 -> 권한 위반
    */
    if (write && !page->writable)
        return false;
    // Lazy Loading 처리 (Claim) -> vm_do_claim_page(page);
    // page는 있기에, uninited page에서 anon 또는 file_backed_page로 만들기?
    if (not_present)
        return vm_do_claim_page(page);

    /* 스택 증가 (Stack Growth) - 이건 나중에 구현 */
    // 페이지가 없더라도 스택 포인터(rsp) 근처라면 스택을 늘려줘야 함.
    // if (stack_growth_condition...) { ... }

    return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page)
{
    destroy(page);
    free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va)
{
    struct page *page = NULL;

    page = spt_find_page(&thread_current()->spt, va);
    if (page == NULL)
    {
        return false;
    }

    return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page(struct page *page)
{
    struct frame *frame;

    if (page == NULL)
        return false;

    frame = vm_get_frame();
    if (frame == NULL)
        return false;

    frame->page = page;
    page->frame = frame;

    if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva,
                       page->writable))
        return false;

    // return swap_in(page, frame->kva); // 여기서 uninit_initialize -> lazy_load_segment 호출됨

    if (swap_in(page, frame->kva))
    {
        return true; /* 성공! 여기서 함수 종료 */
    }
    /* [수정] swap_in(로딩) 실패 시 깔끔하게 정리 (Clean up on failure) */
    /* 1. 프레임 연결 끊기 */
    page->frame = NULL;

    /* 2. 매핑 제거 */
    pml4_clear_page(thread_current()->pml4, page->va);

    /* 3. 메모리 해제 (여기서 한 번만 수행) */
    palloc_free_page(frame->kva);
    free(frame);

    return false;
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED)
{
    hash_init(&spt->hash_table, page_hash, page_less, NULL);
}

/* Copy supplemental page ta ble from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst,
                                  struct supplemental_page_table *src)
{
    struct hash_iterator i;

    /* 1. src(부모)의 해시 테이블을 처음부터 끝까지 순회 */
    hash_first(&i, &src->hash_table);
    while (hash_next(&i))
    {
        struct page *src_page = hash_entry(hash_cur(&i), struct page, hash_elem);
        enum vm_type type = src_page->operations->type;
        void *upage = src_page->va;
        bool writable = src_page->writable;

        /* A. [UNINIT 페이지인 경우] */
        /* 아직 물리 메모리에 없으므로, 설정 정보(aux)만 복사해서 예약만 걸어둡니다. */
        if (type == VM_UNINIT)
        {
            /* 1. setup_stack이나 load_segment에서 설정한 init, aux 가져오기 */
            vm_initializer *init = src_page->uninit.init;
            void *aux = src_page->uninit.aux;
            if (aux != NULL)
            {
                struct lazy_load_info *src_info = (struct lazy_load_info *)aux;
                struct lazy_load_info *dst_info = malloc(sizeof(struct lazy_load_info));

                /* 구조체 값 복사 */
                memcpy(dst_info, src_info, sizeof(struct lazy_load_info));

                /* [핵심] 파일 객체를 새로 엽니다 (독립적인 오프셋/상태 유지) */
                if (src_info->file != NULL)
                {
                    dst_info->file = file_reopen(src_info->file);
                }
                aux = dst_info;
            }
            /* 2. 자식 프로세스에도 똑같은 UNINIT 페이지 생성 */
            /* 주의: aux가 load_segment의 file info라면,
               자식도 파일을 읽어야 하므로 deep copy가 필요할 수 있습니다.
               하지만 간단한 구현에서는 같은 포인터를 쓰거나 file_reopen 등을 고려해야 합니다.
               (Pintos 프로젝트 수준에서는 aux를 그대로 넘겨도 보통 통과되지만,
               엄밀히는 aux 내부의 file 구조체도 복제가 필요할 수 있음) */
            if (!vm_alloc_page_with_initializer(VM_ANON, upage, writable, init, aux))
            {
                return false;
            }
        }
        /* B. [이미 로드된 페이지인 경우 (ANON, FILE)] */
        else
        {
            /* 1. 자식에게도 일단 VM_ANON 등으로 페이지 할당 (초기화 함수는 NULL) */
            /* FILE 타입이라도 일단 ANON으로 복사하는 경우가 많음 (Copy-on-Write가 아니라면)
                  하지만 Project 3에서는 타입에 맞춰 vm_alloc을 호출합니다. */

            /* 여기서는 단순화를 위해 VM_ANON으로 통일하여 설명합니다.
               (파일 매핑인 경우 별도 처리가 필요할 수 있으나, 보통 fork 시
               Private Mapping은 Anon으로 변환되기도 함) */

            // 편의상 타입을 그대로 가져오되, initializer는 NULL (이미 내용이 있으므로)
            if (!vm_alloc_page_with_initializer(type, upage, writable, NULL, NULL))
            {
                return false;
            }

            /* 2. 자식 페이지 할당 요청 (물리 프레임 생성) */
            if (!vm_claim_page(upage))
            {
                return false;
            }

            /* 3. 내용 복사 (Deep Copy) */
            struct page *dst_page = spt_find_page(dst, upage);
            if (dst_page)
            {
                memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
            }
        }
    }
    return true;
}
static void spt_destructor(struct hash_elem *e, void *aux UNUSED)
{
    /* 1. hash_elem으로부터 struct page 포인터 획득 */
    struct page *page = hash_entry(e, struct page, hash_elem);

    /* 2. 해당 페이지의 destroy 함수 호출 (vtable 방식) */
    /* - VM_ANON: anon_destroy 호출 (프레임 해제, 스왑 해제 등) */
    /* - VM_FILE: file_backed_destroy 호출 */
    /* - VM_UNINIT: uninit_destroy 호출 */
    destroy(page);

    /* 3. page 구조체 자체의 메모리 해제 */
    free(page);
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED)
{
    /* TODO: Destroy all the supplemental_page_table hold by thread and
     * TODO: writeback all the modified contents to the storage. */
    if (spt->hash_table.buckets != NULL)
    {
        hash_destroy(&spt->hash_table, spt_destructor);
    }
}

static uint64_t page_hash(const struct hash_elem *e, void *aux UNUSED)
{
    const struct page *page;
    page = hash_entry(e, struct page, hash_elem);
    return hash_bytes(&page->va, sizeof page->va);
}

static bool page_less(const struct hash_elem *a, const struct hash_elem *b,
                      void *aux UNUSED)
{
    const struct page *page_a, *page_b;
    page_a = hash_entry(a, struct page, hash_elem);
    page_b = hash_entry(b, struct page, hash_elem);
    return page_a->va < page_b->va;
}
