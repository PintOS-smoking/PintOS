/* vm.c: Generic interface for virtual memory objects. */

#include <string.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "filesys/file.h"
#include "vm/file.h"

struct frame_table frame_table;

void frame_table_add (struct frame *frame);
void vm_frame_free (struct frame *frame);
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
	list_init (&frame_table.frames);
	lock_init (&frame_table.lock);
	frame_table.clock_hand = NULL;
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
static bool page_less (const struct hash_elem *a, const struct hash_elem *b, void *aux);
static bool should_grow_stack (struct intr_frame *f, void *addr, bool user);
static bool vm_stack_growth (void *addr);
static void spt_destroy_page (struct hash_elem *elem, void *aux);
static bool copy_uninit_page (struct supplemental_page_table *dst, struct page *src_page);
static bool copy_anon_page (struct supplemental_page_table *dst, struct page *src_page);
static bool copy_file_page(struct supplemental_page_table *dst_spt, struct page *src_page);

#define STACK_LIMIT (1 << 20)
#define STACK_HEURISTIC 8

bool vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,	vm_initializer *init, void *aux) {
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
		page->cow = false;
		page->owner = thread_current ();

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
struct page *spt_find_page (struct supplemental_page_table *spt, void *va) {
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

static struct frame *vm_get_victim (void) {
    struct frame *victim = NULL;

    lock_acquire(&frame_table.lock);

    if (list_empty(&frame_table.frames)) {
        lock_release(&frame_table.lock);
        return NULL;
    }

    if (frame_table.clock_hand == NULL || frame_table.clock_hand == list_end(&frame_table.frames)) 
        frame_table.clock_hand = list_begin(&frame_table.frames);
    
    struct list_elem *cur = frame_table.clock_hand;
    size_t n = list_size(&frame_table.frames);

    for (size_t i = 0; i < n; i++) {
        struct frame *frame = list_entry(cur, struct frame, frame_elem);

        if (!frame->pinned && frame->refs == 0) {
            victim = frame;
            cur = list_next(cur);
            if (cur == list_end(&frame_table.frames))
                cur = list_begin(&frame_table.frames);

            frame_table.clock_hand = cur;
            break;
        }

        cur = list_next(cur);
		
        if (cur == list_end(&frame_table.frames))
            cur = list_begin(&frame_table.frames);
    }

    lock_release(&frame_table.lock);
    return victim;    
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame (void) {
	struct frame *victim = vm_get_victim ();
	struct page *page;

	if (victim == NULL)
		PANIC ("no frame");

	page = victim->page;

	if (page != NULL) {
		if (!swap_out (page))
			PANIC ("swap out failed");

		page->frame = NULL;
	}

	victim->page = NULL;
	victim->pinned = false;

	return victim;
}

static struct frame * vm_get_frame (void) {
    struct frame *frame = NULL;
    void *kva;

    kva = palloc_get_page(PAL_USER);
    if (kva == NULL) {
        frame = vm_evict_frame ();
        if (frame == NULL)
            PANIC ("cannot evict frame");
        frame->refs = 0;
        return frame;
    }

    frame = malloc(sizeof(struct frame));

    if (frame == NULL) {
        palloc_free_page(kva);
        return NULL;
    }

    frame->kva = kva;
    frame->page = NULL;
    frame->pinned = false;
	frame->on_table = false;
	frame->refs = 0;

    ASSERT (frame != NULL);
    ASSERT (frame->page == NULL);
    return frame;
}

static bool vm_stack_growth (void *addr) {
	struct supplemental_page_table *spt;
	void *stack_bottom = pg_round_down (addr);

	spt = &thread_current ()->spt;

	if (spt_find_page (spt, stack_bottom) != NULL) 
		return true;
	
	return vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

bool vm_try_handle_fault (struct intr_frame *f, void *addr,	bool user, bool write, bool not_present) {
	struct supplemental_page_table *spt;
	struct page *page;
	void *page_addr;

	spt = &thread_current ()->spt;

	if (addr == NULL || is_kernel_vaddr (addr))
		return false;

	page_addr = pg_round_down (addr);
	page = spt_find_page (spt, page_addr);

	if (page == NULL) {
		if (!should_grow_stack (f, addr, user) || !vm_stack_growth (page_addr))
			return false;

		page = spt_find_page (spt, page_addr);
		
		if (page == NULL)
			return false;
	}

	if (write && !page->writable) {
		struct frame *frame = page->frame;
		struct frame *new_frame;

		if (frame == NULL || !page->cow)
			return false;

		new_frame = vm_get_frame ();
		if (new_frame == NULL)
			return false;

		memcpy (new_frame->kva, frame->kva, PGSIZE);
		new_frame->page = page;
		new_frame->pinned = true;

		if (!pml4_set_page (thread_current ()->pml4, page->va, new_frame->kva, true)) {
			goto fail;
		}

		frame_table_add (new_frame);
		new_frame->pinned = false;

		if (frame->refs > 0)
			frame->refs--;

		page->frame = new_frame;
		page->writable = true;
		page->cow = false;

		return true;

fail:
		new_frame->page = NULL;
		new_frame->on_table = false;
		new_frame->refs = 0;
		vm_frame_free (new_frame);
		return false;
	}

	if (!not_present)
		return false;

	return vm_do_claim_page (page);
}


/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page (void *va) {
	struct page *page = NULL;
	
	page = spt_find_page(&thread_current()->spt, va);

	if (page == NULL) 
		return false;

	page->cow = false;
	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page (struct page *page) {
    struct frame *frame;
	struct thread *owner;
	bool new_frame = false;
	
	if (page == NULL) 
		return false;

	frame = page->frame;
	if (frame == NULL) {
		frame = vm_get_frame ();
		if (frame == NULL)
			return false;
		new_frame = true;
		frame->page = page;
		page->frame = frame;
	}

	owner = page->owner;
	if (owner == NULL) {
		owner = thread_current ();
		page->owner = owner;
	}

	frame->pinned = true;

	if (!pml4_set_page(owner->pml4, page->va, frame->kva, page->writable)){
		frame->pinned = false;
		if (new_frame) {
			frame->page = NULL;
			page->frame = NULL;
			vm_frame_free (frame);
		}
		return false;
	}
	
	if (!swap_in (page, frame->kva)) {
		pml4_clear_page (owner->pml4, page->va);
		frame->pinned = false;
		if (new_frame) {
			frame->page = NULL;
			page->frame = NULL;
			vm_frame_free (frame);
		}
		return false;
	}
	
	if (!frame->on_table)
		frame_table_add (frame);

	frame->pinned = false;
    return true;
}

/* Initialize new supplemental page table */
void supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init(&spt->hash_table, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy (struct supplemental_page_table *dst, struct supplemental_page_table *src) {
	struct hash_iterator i;

	hash_first (&i, &src->hash_table);

	while (hash_next (&i)) {
		struct page *src_page;
		enum vm_type type;

		src_page = hash_entry (hash_cur (&i), struct page, hash_elem);
		type = page_get_type (src_page);

		switch (type) {

		case VM_UNINIT:
			if (!copy_uninit_page (dst, src_page))
				return false;
			break;

		case VM_ANON:
			if (!copy_anon_page (dst, src_page))
				return false;
			break;
		
		case VM_FILE:
			if (!copy_file_page(dst, src_page))
				return false;
			break;

		default:
			break;
		}
	}

	return true;
}

static void share_page_frame (struct page *dst_page, struct page *src_page) {
	struct frame *frame = src_page->frame;

	if (frame == NULL)
		return;

	frame->refs++;
	dst_page->frame = frame;
	dst_page->operations = src_page->operations;
	dst_page->cow = true;
	dst_page->writable = false;
	src_page->cow = true;
	src_page->writable = false;
	if (src_page->owner != NULL)
		pml4_set_page (src_page->owner->pml4, src_page->va, frame->kva, false);

	switch (page_get_type (src_page)) {
	case VM_ANON:
		dst_page->anon = src_page->anon;
		break;

	case VM_FILE:
		dst_page->file = src_page->file;
		break;
		
	default:
		break;
	}
}

static bool copy_uninit_page (struct supplemental_page_table *dst, struct page *src_page) {
	struct uninit_page *uninit = &src_page->uninit;
	void *aux = uninit->aux;
	lazy_load_info *dst_info = NULL;

	if (aux != NULL) {
		lazy_load_info *src_info = aux;
		dst_info = malloc (sizeof *dst_info);

		if (dst_info == NULL)
			return false;
		
		memcpy (dst_info, src_info, sizeof *dst_info);

		if (src_info->file != NULL) {
			dst_info->file = file_reopen (src_info->file);
			if (dst_info->file == NULL)
				goto fail;
		}
		aux = dst_info;
	}

	if (!vm_alloc_page_with_initializer (uninit->type, src_page->va, src_page->writable, uninit->init, aux))
		goto fail;

	return true;

fail:
	if (dst_info != NULL) {
		if (dst_info->file != NULL)
			file_close (dst_info->file);
		free (dst_info);
	}
	return false;
}


static bool copy_anon_page (struct supplemental_page_table *dst, struct page *src_page) {
	struct page *dst_page;

	if (!vm_alloc_page (VM_ANON, src_page->va, src_page->writable))
		return false;

	if (src_page->frame == NULL)
		return true;

	dst_page = spt_find_page (dst, src_page->va);
	if (dst_page == NULL)
		return false;

	share_page_frame (dst_page, src_page);
	return true;
}

static bool copy_file_page(struct supplemental_page_table *dst_spt, struct page *src_page){
    void *va = src_page->va;
    bool writable = src_page->writable;
    struct file_page *src_fp = &src_page->file;

    struct file_page *aux = malloc(sizeof *aux);
    if (aux == NULL)
        return false;

    *aux = *src_fp;   

    if (!vm_alloc_page_with_initializer(VM_FILE, va, writable, lazy_load_file, aux)) {
        free(aux);
        return false;
    }

    struct page *child_page = spt_find_page(dst_spt, va);
    if (child_page == NULL)
        return false;   

    if (src_page->frame != NULL)
        share_page_frame (child_page, src_page);

    return true;
}

void supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	if (spt == NULL)
		return;

	hash_destroy (&spt->hash_table, spt_destroy_page);
}

static void spt_destroy_page (struct hash_elem *elem, void *aux UNUSED) {
	struct page *page = hash_entry (elem, struct page, hash_elem);

	vm_dealloc_page (page);
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

static bool should_grow_stack (struct intr_frame *f, void *addr, bool user) {
	uint8_t *rsp = NULL;
	uint8_t *fault_addr = addr;

	if (user)
		rsp = (uint8_t *) f->rsp;
	else
		rsp = (uint8_t *) thread_current ()->user_rsp;

	if (rsp == NULL)
		return false;

	if (fault_addr >= (uint8_t *) USER_STACK)
		return false;

	if (fault_addr < (uint8_t *) USER_STACK - STACK_LIMIT)
		return false;

	if (fault_addr < rsp - STACK_HEURISTIC)
		return false;

	return true;
}
