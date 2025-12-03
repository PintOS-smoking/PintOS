/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/mmu.h"
#include "threads/thread.h"
#include "userprog/process.h"


/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);
static uint64_t page_hash (const struct hash_elem *e, void *aux);
static bool page_less (const struct hash_elem *a,
		const struct hash_elem *b, void *aux);

bool vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	if (spt_find_page (spt, upage) == NULL) {
		struct page *page = malloc (sizeof *page);
		bool (*initializer) (struct page *, enum vm_type, void *) = NULL;

		if (page == NULL) 
			goto err;

		switch (VM_TYPE (type)) {
			case VM_ANON:
				initializer = anon_initializer;
				break;
			case VM_FILE:
				initializer = file_backed_initializer;
				break;
			default:
				free (page);
				goto err;
		}

		uninit_new (page, upage, init, type, aux, initializer);
		page->writable = writable;

		if (!spt_insert_page (spt, page)) {
			free (page);
			goto err;
		}
		return true;
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt, void *va) {
	/* TODO: Fill this function. */
	struct page dummy_page;
	struct hash_elem *elem;

	if (spt == NULL || va == NULL)
		return NULL;

	dummy_page.va = pg_round_down (va);
	elem = hash_find (&spt->hash_table, &dummy_page.hash_elem);

	if (elem == NULL)
		return NULL;
	
	return hash_entry(elem, struct page, hash_elem);	
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	if (spt == NULL || page == NULL || page->va == NULL)
		return false;

	// page->va = pg_round_down (page->va);
	return hash_insert (&spt->hash_table, &page->hash_elem) == NULL;
}

bool spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	struct hash_elem *result;

	if (spt == NULL || page == NULL)
		return false;

	result = hash_delete(&spt->hash_table, &page->hash_elem);

	if (result != NULL) {
		vm_dealloc_page (page);
		return true;
	}
	return false;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame * vm_get_frame (void) {
    struct frame *frame = NULL;
    void *kva;

    kva = palloc_get_page(PAL_USER);
    if (kva == NULL) {
        PANIC("todo");
    }

    frame = malloc(sizeof(struct frame));
    if (frame == NULL) {
        palloc_free_page(kva);
        return NULL;
    }

    frame->kva = kva;
    frame->page = NULL;

    ASSERT (frame != NULL);
    ASSERT (frame->page == NULL);
    return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr, bool user UNUSED, bool write, bool not_present) {
	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;

	if (addr == NULL) 
		return false;
	
	if (is_kernel_vaddr(addr))
		return false;


	if (not_present) {
		page = spt_find_page(spt, addr);
		if (page == NULL) {
			return false;
		}
		if (write == 1 && page->writable == 0) 
			return false;

		return vm_do_claim_page (page);
	}

	return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page (void *va) {
	struct page *page = NULL;
	
	page = spt_find_page(&thread_current()->spt, va);
	if (page == NULL) {
		return false;
	}

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page (struct page *page) {
    struct frame *frame;
	
	if (page == NULL) 
		return false;
	
	frame = vm_get_frame ();
    frame->page = page;
    page->frame = frame;

    if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)) 
		return false;
	
    return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init (struct supplemental_page_table *spt) {
	hash_init(&spt->hash_table, page_hash, page_less, NULL);
}

bool supplemental_page_table_copy (struct supplemental_page_table *dst, struct supplemental_page_table *src) {
	struct hash_iterator i;

	hash_first (&i, &src->hash_table);
	while (hash_next (&i)) {
		struct page *page = hash_entry (hash_cur (&i), struct page, hash_elem);
		enum vm_type type = page->operations->type;

		switch (type) {
			case VM_UNINIT:
				void *aux = page->uninit.aux;

				if (aux != NULL) {
					lazy_load_info *src_aux = (lazy_load_info *)aux;
					lazy_load_info *dst_aux = (lazy_load_info *)malloc(sizeof(lazy_load_info));
					if (dst_aux == NULL) {
						return false;
					}

					memcpy(dst_aux , src_aux, sizeof(lazy_load_info));
					dst_aux->file = file_reopen(src_aux->file);
					if(!vm_alloc_page_with_initializer(page->uninit.type, page->va, page->writable, page->uninit.init, dst_aux)) {
						free(dst_aux);
						return false;
					}

				} else {
					if (!vm_alloc_page_with_initializer(page->uninit.type, page->va, page->writable, page->uninit.init, page->uninit.aux))
						return false;
				}
				

				break;
			
			case VM_ANON:
				if (!vm_alloc_page(VM_ANON, page->va, page->writable))
					return false;
				

				if (!vm_claim_page(page->va)) 
					return false;
				
				memcpy(spt_find_page(dst, page->va)->frame->kva, page->frame->kva, PGSIZE);
				
				break;

			default:
				break;
		}
   }

   return true;
}

void hash_page_destroy (struct hash_elem *e, void *aux UNUSED) {
    struct page *page = hash_entry(e, struct page, hash_elem);

    if (page->frame != NULL) {

        // palloc_free_page(page->frame->kva);
        // free(page->frame);
    }

    vm_dealloc_page(page);
}

// static void hash_page_destroy (struct hash_elem *elem, void *aux UNUSED) {
// 	struct page *page = hash_entry (elem, struct page, hash_elem);
// 	vm_dealloc_page (page);
// }

void supplemental_page_table_kill (struct supplemental_page_table *spt) {

	if (spt == NULL) {
		return;
	}

	hash_destroy(&spt->hash_table, hash_page_destroy);

}


static uint64_t page_hash (const struct hash_elem *e, void *aux UNUSED) {
	const struct page *page; 
	page = hash_entry (e, struct page, hash_elem);
	return hash_bytes (&page->va, sizeof page->va);
}

static bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED) {
	const struct page *page_a, *page_b; 
	page_a = hash_entry (a, struct page, hash_elem);
	page_b = hash_entry (b, struct page, hash_elem);
	return page_a->va < page_b->va;
}
