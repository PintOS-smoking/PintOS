#include "userprog/validate.h"

#include "threads/mmu.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static int64_t get_user(const uint8_t* uaddr);
static int64_t put_user(uint8_t* udst, uint8_t byte);

bool valid_address(const void* uaddr, bool write) {
    struct thread* t = thread_current();
    void* page_addr;

    // 유효한 주소가 아니거나 || 커널 영역의 주소라면, false
    if (uaddr == NULL || !is_user_vaddr(uaddr))
        return false;

    // 메모리가 매핑되어 있는지 확인
    if (get_user(uaddr) == -1)
        return false;

    // 쓰기 권한 필요 없으면 통과
    if (!write)
        return true;

    // pml4 테이블로 해당 page의 쓰기 권한 확인
    page_addr = pg_round_down(uaddr);
    return pml4_is_writable(t->pml4, page_addr);
}

static int64_t get_user(const uint8_t* uaddr) {
    int64_t result;
    __asm __volatile(
        "movabsq $done_get, %0\n"
        "movzbq %1, %0\n"
        "done_get:\n"
        : "=&a"(result)
        : "m"(*uaddr));
    return result;
}

static int64_t put_user(uint8_t* udst, uint8_t byte) {
    int64_t error_code;
    __asm __volatile(
        "movabsq $done_put, %0\n"
        "movb %b2, %1\n"
        "done_put:\n"
        : "=&a"(error_code), "=m"(*udst)
        : "q"(byte));
    return error_code;
}