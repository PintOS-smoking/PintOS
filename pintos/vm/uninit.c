/* uninit.c: Implementation of uninitialized page.
 *
 * All of the pages are born as uninit page. When the first page fault occurs,
 * the handler chain calls uninit_initialize (page->operations.swap_in).
 * The uninit_initialize function transmutes the page into the specific page
 * object (anon, file, page_cache), by initializing the page object,and calls
 * initialization callback that passed from vm_alloc_page_with_initializer
 * function.
 * */

/* [수정 후] 반드시 vm.h가 1순위여야 합니다! */
#include "vm/vm.h"
#include "vm/uninit.h"
#include "vm/inspect.h"

static bool uninit_initialize(struct page *page, void *kva);
static void uninit_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations uninit_ops = {
    .swap_in = uninit_initialize,
    .swap_out = NULL,
    .destroy = uninit_destroy,
    .type = VM_UNINIT,
};

/* DO NOT MODIFY this function */
void uninit_new(struct page *page, void *va, vm_initializer *init,
                enum vm_type type, void *aux,
                bool (*initializer)(struct page *, enum vm_type, void *))
{
    ASSERT(page != NULL);

    *page = (struct page){.operations = &uninit_ops,
                          .va = va,
                          .frame = NULL, /* no frame for now */
                          .uninit = (struct uninit_page){
                              .init = init,
                              .type = type,
                              .aux = aux,
                              .page_initializer = initializer,
                          }};
}

/* Initalize the page on first fault */
static bool uninit_initialize(struct page *page, void *kva)
{
    /* [중요] 현재 페이지는 Union 중 'uninit' 상태입니다. */
    // page의 union의 struct uninit_page uninit 멤버 추출
    struct uninit_page *uninit = &page->uninit;

    // 지역 변수(vm_initializer)에 기존 union 영역(uninit 구조체)을 저장
    vm_initializer *init = uninit->init;

    // 파일 정보(lazy_load_info)를 aux에 저장
    void *aux = uninit->aux;

    /* TODO: You may need to fix this function. */
    // uninit->page_initializer로 해당 page는 ANON_PAGE로 변경 되고,
    //  마지막으로 lazy_load_segment를 호출하여 텅 빈 메모리에 파일 내용을 채워
    //  넣
    return uninit->page_initializer(page, uninit->type, kva) &&
           (init ? init(page, aux) : true);
}

/* Free the resources hold by uninit_page. Although most of pages are transmuted
 * to other page objects, it is possible to have uninit pages when the process
 * exit, which are never referenced during the execution.
 * PAGE will be freed by the caller. */
static void uninit_destroy(struct page *page)
{
    struct uninit_page *uninit UNUSED = &page->uninit;
    /* TODO: Fill this function.
     * TODO: If you don't have anything to do, just return. */
}
