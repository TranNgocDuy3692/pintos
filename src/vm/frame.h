#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/thread.h"

/* Structure representing a frame table entry */
struct frame_table_entry {
  void *frame;              /* Pointer to the frame */
  tid_t tid;                /* Thread ID associated with the frame */
  uint32_t *pte;            /* Page table entry */
  void *user_page;          /* User virtual address */
  struct list_elem elem;    /* List element for frame table list */
};

extern struct list frame_table;

/* Frame allocation functions */
void frame_table_init(void);
void *allocate_frame(enum palloc_flags flags);
void free_frame(void *);

/* Frame table management functions */
void set_frame_user_page(void *, uint32_t *, void *);

/* Evict a frame, saving its content to a swap slot or file */
void *evict_frame(void);

#endif /* VM_FRAME_H */
