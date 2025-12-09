/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"

#include <string.h>

#include "filesys/file.h"
#include "lib/kernel/list.h"  // 추가 권장
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "vm/file.h"
#include "vm/inspect.h"

static struct list frame_table;
static struct list_elem* clock_elem;

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
    list_init(&frame_table);
    clock_elem = NULL;
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

/* Helpers */
static struct frame* vm_get_victim(void);
static struct frame* vm_get_frame(void);
static bool vm_do_claim_page(struct page* page);
static struct frame* vm_evict_frame(void);
static uint64_t page_hash(const struct hash_elem* e, void* aux);
static bool page_less(const struct hash_elem* a, const struct hash_elem* b, void* aux);
static bool should_grow_stack(struct intr_frame* f, void* addr, bool user);
static bool vm_stack_growth(void* addr);
static void spt_destroy_page(struct hash_elem* elem, void* aux);
static bool copy_uninit_page(struct supplemental_page_table* dst, struct page* src_page);
static bool copy_anon_page(struct supplemental_page_table* dst, struct page* src_page);
static bool copy_file_page(struct supplemental_page_table* dst_spt, struct page* src_page);

#define STACK_LIMIT (1 << 20)
#define STACK_HEURISTIC 8

bool vm_alloc_page_with_initializer(enum vm_type type, void* upage, bool writable,
                                    vm_initializer* init, void* aux) {
    ASSERT(VM_TYPE(type) != VM_UNINIT)

    struct supplemental_page_table* spt = &thread_current()->spt;

    if (spt_find_page(spt, upage) == NULL) {
        struct page* page = malloc(sizeof *page);
        bool (*initializer)(struct page*, enum vm_type, void*) = NULL;

        if (page == NULL) goto err;

        switch (VM_TYPE(type)) {
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

        if (!spt_insert_page(spt, page)) {
            free(page);
            goto err;
        }
        return true;
    }
err:
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page* spt_find_page(struct supplemental_page_table* spt, void* va) {
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

    if (spt == NULL || page == NULL) return false;

    result = hash_delete(&spt->hash_table, &page->hash_elem);

    if (result != NULL) {
        vm_dealloc_page(page);
        return true;
    }
    return false;
}

/* Get the struct frame, that will be evicted. */
/* [Clock Algorithm] 희생자를 고르는 함수 */
static struct frame* vm_get_victim(void) {
    struct frame* candidate = NULL;
    struct thread* curr = thread_current();

    // frame_table이 비어있으면 쫓아낼 것도 없음 (Error)
    if (list_empty(&frame_table)) PANIC("Frame table is empty!");

    // 시계 바늘이 초기화 안 됐거나 끝에 도달했으면 처음으로 돌림
    if (clock_elem == NULL || clock_elem == list_end(&frame_table)) {
        clock_elem = list_begin(&frame_table);
    }

    // 무한 루프를 돌며 희생자를 찾음 (반드시 하나는 찾게 되어 있음)
    while (true) {
        candidate = list_entry(clock_elem, struct frame, frame_table_elem);

        // 다음 검사 대상을 미리 정해둠 (Circular List 처럼 동작)
        clock_elem = list_next(clock_elem);
        if (clock_elem == list_end(&frame_table)) {
            clock_elem = list_begin(&frame_table);
        }

        struct page* p = candidate->page;

        // [핵심 로직] 기회 주기 (Second Chance)
        // pml4_is_accessed: 최근에 CPU가 이 페이지를 읽거나 쓴 적이 있는가?
        if (pml4_is_accessed(curr->pml4, p->va)) {
            // 1. 최근에 썼다면(Accessed=1), 봐준다. 대신 Accessed 비트를 0으로 끈다.
            pml4_set_accessed(curr->pml4, p->va, 0);
        } else {
            // 2. 최근에 안 썼다면(Accessed=0), 당첨! 네가 희생자다.
            return candidate;
        }
    }
}
/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
struct frame* vm_evict_frame(void) {
    struct frame* victim = vm_get_victim();  // [1] 희생자 선정 (Clock Algo)
    struct page* page = victim->page;

    // [2] 짐 싸서 내보내기 (Swap Out)
    if (swap_out(page) == false) {
        PANIC("Swap out failed!");  // 디스크도 꽉 찼다면 답이 없음
    }

    // [3] 연결 끊기 (Unmap)
    // 이 페이지는 이제 메모리에 없으므로, 접근하면 Page Fault가 나게 만듦
    page->frame = NULL;   // 페이지에서 프레임 정보 삭제
    victim->page = NULL;  // 프레임에서 페이지 정보 삭제

    // 중요: 하드웨어 페이지 테이블(PML4)에서도 매핑 제거
    pml4_clear_page(thread_current()->pml4, page->va);

    return victim;  // [4] 빈 프레임 반환
}
/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame* vm_get_frame(void) {
    struct frame* frame = NULL;
    void* kva;

    kva = palloc_get_page(PAL_USER);
    if (kva == NULL) {
        // PANIC("todo");
        frame = vm_evict_frame();

        frame->page = NULL;  // 주인 이름표만 뗌
        return frame;        // 이 헌 방(하지만 비어있는)을 리턴
    }

    frame = malloc(sizeof(struct frame));
    if (frame == NULL) {
        palloc_free_page(kva);
        return NULL;
    }

    frame->kva = kva;
    frame->page = NULL;

    list_push_back(&frame_table, &frame->frame_table_elem);

    ASSERT(frame != NULL);
    ASSERT(frame->page == NULL);
    return frame;
}

static bool vm_stack_growth(void* addr) {
    struct supplemental_page_table* spt;
    void* stack_bottom = pg_round_down(addr);

    spt = &thread_current()->spt;

    if (spt_find_page(spt, stack_bottom) != NULL) return true;

    return vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true);
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page* page UNUSED) { return false; }

bool vm_try_handle_fault(struct intr_frame* f, void* addr, bool user, bool write,
                         bool not_present) {
    struct supplemental_page_table* spt;
    struct page* page;
    void* page_addr;

    spt = &thread_current()->spt;

    if (addr == NULL || is_kernel_vaddr(addr) || !not_present) return false;

    page_addr = pg_round_down(addr);
    page = spt_find_page(spt, page_addr);

    if (page == NULL) {
        if (!should_grow_stack(f, addr, user) || !vm_stack_growth(page_addr)) return false;

        page = spt_find_page(spt, page_addr);

        if (page == NULL) return false;
    }

    if (write && !page->writable) return false;

    return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page* page) {
    destroy(page);
    free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void* va) {
    struct page* page = NULL;

    page = spt_find_page(&thread_current()->spt, va);

    if (page == NULL) return false;

    return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page(struct page* page) {
    struct frame* frame;

    if (page == NULL) return false;

    frame = vm_get_frame();
    frame->page = page;
    page->frame = frame;

    if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)) {
        palloc_free_page(frame->kva);
        free(frame);
        page->frame = NULL;
        return false;
    }

    return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table* spt UNUSED) {
    hash_init(&spt->hash_table, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table* dst,
                                  struct supplemental_page_table* src) {
    struct hash_iterator i;

    hash_first(&i, &src->hash_table);

    while (hash_next(&i)) {
        struct page* src_page;
        enum vm_type type;

        src_page = hash_entry(hash_cur(&i), struct page, hash_elem);
        type = page_get_type(src_page);

        switch (type) {
            case VM_UNINIT:
                if (!copy_uninit_page(dst, src_page)) return false;
                break;

            case VM_ANON:
                if (!copy_anon_page(dst, src_page)) return false;
                break;

            case VM_FILE:
                if (!copy_file_page(dst, src_page)) return false;
                break;

            default:
                break;
        }
    }

    return true;
}

static bool copy_uninit_page(struct supplemental_page_table* dst, struct page* src_page) {
    struct uninit_page* uninit = &src_page->uninit;
    void* aux = uninit->aux;
    lazy_load_info* dst_info = NULL;

    if (aux != NULL) {
        lazy_load_info* src_info = aux;
        dst_info = malloc(sizeof *dst_info);

        if (dst_info == NULL) return false;

        memcpy(dst_info, src_info, sizeof *dst_info);

        if (src_info->file != NULL) {
            dst_info->file = file_reopen(src_info->file);
            if (dst_info->file == NULL) goto fail;
        }
        aux = dst_info;
    }

    if (!vm_alloc_page_with_initializer(uninit->type, src_page->va, src_page->writable,
                                        uninit->init, aux))
        goto fail;

    return true;

fail:
    if (dst_info != NULL) {
        if (dst_info->file != NULL) file_close(dst_info->file);
        free(dst_info);
    }
    return false;
}

static bool copy_anon_page(struct supplemental_page_table* dst, struct page* src_page) {
    struct page* dst_page;

    if (!vm_alloc_page(VM_ANON, src_page->va, src_page->writable)) return false;

    if (src_page->frame == NULL) return true;

    if (!vm_claim_page(src_page->va)) return false;

    dst_page = spt_find_page(dst, src_page->va);
    memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
    return true;
}

static bool copy_file_page(struct supplemental_page_table* dst_spt, struct page* src_page) {
    void* va = src_page->va;
    bool writable = src_page->writable;
    struct file_page* src_fp = &src_page->file;

    struct file_page* aux = malloc(sizeof *aux);
    if (aux == NULL) return false;

    *aux = *src_fp;

    if (!vm_alloc_page_with_initializer(VM_FILE, va, writable, lazy_load_file, aux)) {
        free(aux);
        return false;
    }

    struct page* child_page = spt_find_page(dst_spt, va);
    if (child_page == NULL) return false;

    if (src_page->frame != NULL) {
        if (!vm_claim_page(va)) return false;

        memcpy(child_page->frame->kva, src_page->frame->kva, PGSIZE);
    }

    return true;
}

void supplemental_page_table_kill(struct supplemental_page_table* spt UNUSED) {
    if (spt == NULL) return;

    hash_destroy(&spt->hash_table, spt_destroy_page);
}

static void spt_destroy_page(struct hash_elem* elem, void* aux UNUSED) {
    struct page* page = hash_entry(elem, struct page, hash_elem);

    vm_dealloc_page(page);
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

static bool should_grow_stack(struct intr_frame* f, void* addr, bool user) {
    uint8_t* rsp = NULL;
    uint8_t* fault_addr = addr;

    rsp = user ? (uint8_t*)f->rsp : (uint8_t*)thread_current()->tf.rsp;

    if (fault_addr >= (uint8_t*)USER_STACK) return false;

    if (fault_addr < (uint8_t*)USER_STACK - STACK_LIMIT) return false;

    if (fault_addr < rsp - STACK_HEURISTIC) return false;

    return true;
}
