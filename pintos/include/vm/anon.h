#ifndef VM_ANON_H
#define VM_ANON_H
#include "lib/kernel/bitmap.h"
#include "vm/vm.h"
struct page;
enum vm_type;

struct anon_page {
	size_t swap_idx; /* Swap slot index used when the page is evicted. */
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
