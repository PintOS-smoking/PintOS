#include "vm/vm.h"
#include "threads/malloc.h"

extern struct frame_table frame_table;

void frame_table_add (struct frame *frame) {
	if (frame == NULL || frame->on_table)
		return;

	lock_acquire (&frame_table.lock);

	if (!frame->on_table) {
		list_push_back (&frame_table.frames, &frame->frame_elem);
		frame->on_table = true;

		if (frame_table.clock_hand == NULL)
			frame_table.clock_hand = &frame->frame_elem;
	}

	lock_release (&frame_table.lock);
}

static void frame_table_remove (struct frame *frame) {
	bool hand;
	struct list_elem *next;
	bool empty;

	if (frame == NULL)
		return;

	lock_acquire (&frame_table.lock);

	if (!frame->on_table) {
		lock_release (&frame_table.lock);
		return;
	}

	hand = frame_table.clock_hand == &frame->frame_elem;
	next = list_next (&frame->frame_elem);
	list_remove (&frame->frame_elem);

	empty = list_empty (&frame_table.frames);
    
	if (empty)
		frame_table.clock_hand = NULL;

    if (!empty && hand) {
        frame_table.clock_hand = (next == list_end (&frame_table.frames))
        ? list_begin (&frame_table.frames) : next;
    }

	frame->on_table = false;
	lock_release (&frame_table.lock);
}

void vm_frame_free (struct frame *frame) {
	if (frame == NULL)
		return;

	frame_table_remove (frame);
	palloc_free_page (frame->kva);
	free (frame);
}
