#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "lib/kernel/list.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "threads/pte.h"
#include "vm/swap.h"
#include "vm/frame.h"

struct list frame_table;

/* Synchronization lock for the frame table */
static struct lock frame_table_lock;

/* Lock to ensure atomicity during frame eviction */
static struct lock eviction_lock;

/* Functions for managing frame table entries */
static bool add_frame_to_table(void *);
static void remove_frame_from_table(void *);
/* Retrieve the frame table entry for a given frame */
static struct frame_table_entry *get_frame_table_entry(void *);

/* Functions for frame eviction */
static struct frame_table_entry *select_frame_for_eviction(void); // Select a frame to evict
/* Save the content of the evicted frame for future use */
static bool save_evicted_frame_content(struct frame_table_entry *);


/* Initialize the frame table and necessary data structures */
void
frame_table_init()
{
  list_init(&frame_table);
  lock_init(&frame_table_lock);
  lock_init(&eviction_lock);
}

/* Allocate a page from the USER_POOL and add an entry to the frame table */
void *
allocate_frame(enum palloc_flags flags)
{
  void *frame = NULL;

  /* Try to allocate a page from the user pool */
  if (flags & PAL_USER)
  {
    if (flags & PAL_ZERO)
      frame = palloc_get_page(PAL_USER | PAL_ZERO);
    else
      frame = palloc_get_page(PAL_USER);
  }

  /* If allocation succeeds, add to the frame table.
     Otherwise, evict a page to swap space and fail the allocator for now. */
  if (frame != NULL)
    add_frame_to_table(frame);
  else if ((frame = evict_frame()) == NULL)
    PANIC("Eviction failed");

  return frame;
}

/* Free a frame and remove its entry from the frame table */
void
free_frame(void *frame)
{
  /* Remove the frame table entry */
  remove_frame_from_table(frame);
  /* Free the frame */
  palloc_free_page(frame);
}

/* Set the PTE attribute for a given frame */
void
set_frame_user_page(void *frame, uint32_t *pte, void *upage)
{
  struct frame_table_entry *fte;
  fte = get_frame_table_entry(frame);
  if (fte != NULL)
  {
    fte->pte = pte;
    fte->user_page = upage;
  }
}

/* Evict a frame and save its content for later use */
void *
evict_frame()
{
  bool result;
  struct frame_table_entry *fte;
  struct thread *t = thread_current();

  lock_acquire(&eviction_lock);

  fte = select_frame_for_eviction();
  if (fte == NULL)
    PANIC("No frame available for eviction.");

  result = save_evicted_frame_content(fte);
  if (!result)
    PANIC("Failed to save evicted frame content");
  
  fte->tid = t->tid;
  fte->pte = NULL;
  fte->user_page = NULL;

  lock_release(&eviction_lock);

  return fte->frame;
}

/* Select a frame to evict */
static struct frame_table_entry *
select_frame_for_eviction()
{
  struct frame_table_entry *fte;
  struct thread *t;
  struct list_elem *e;

  struct frame_table_entry *candidate_frame = NULL;

  int rounds = 1;
  bool found = false;
  /* Iterate through the frame table to find a suitable frame to evict */
  while (!found)
  {
    /* Traverse the frame table to locate a frame to evict.
       Try to find a (0,0) class frame. If not found, set the accessed bit to 0.
       The maximum number of rounds is 2, ensuring we find a suitable frame. */
    e = list_head(&frame_table);
    while ((e = list_next(e)) != list_tail(&frame_table))
    {
      fte = list_entry(e, struct frame_table_entry, elem);
      t = thread_get_by_id(fte->tid);
      bool accessed = pagedir_is_accessed(t->pagedir, fte->user_page);
      if (!accessed)
      {
        candidate_frame = fte;
        list_remove(e);
        list_push_back(&frame_table, e);
        break;
      }
      else
      {
        pagedir_set_accessed(t->pagedir, fte->user_page, false);
      }
    }

    if (candidate_frame != NULL)
      found = true;
    else if (rounds++ == 2)
      found = true;
  }

  return candidate_frame;
}

/* Save the content of an evicted frame for future use */
static bool
save_evicted_frame_content(struct frame_table_entry *fte)
{
  struct thread *t;
  struct suppl_pte *spte;

  /* Get the thread corresponding to the frame */
  t = thread_get_by_id(fte->tid);

  /* Get the supplemental page table entry for the frame's user page */
  spte = get_suppl_pte(&t->suppl_page_table, fte->user_page);

  /* If no supplemental page table entry is found, create one */
  if (spte == NULL)
  {
    spte = calloc(1, sizeof *spte);
    spte->user_vaddr = fte->user_page;
    spte->type = SWAP;
    if (!insert_suppl_pte(&t->suppl_page_table, spte))
      return false;
  }

  size_t swap_slot_index;
  /* If the page is dirty and is a memory-mapped file, write it back to the file.
     Otherwise, if the page is dirty, move it to the swap space.
     If the page is clean and not a file, it is a stack page and needs to be moved to swap. */
  if (pagedir_is_dirty(t->pagedir, spte->user_vaddr) && (spte->type == MMF))
  {
    write_page_back_to_file_wo_lock(spte);
  }
  else if (pagedir_is_dirty(t->pagedir, spte->user_vaddr) || (spte->type != FILE))
  {
    swap_slot_index = vm_swap_out(spte->user_vaddr);
    if (swap_slot_index == SWAP_ERROR)
      return false;

    spte->type = spte->type | SWAP;
  }

  memset(fte->frame, 0, PGSIZE);
  /* Update swap attributes */
  spte->swap_slot_index = swap_slot_index;
  spte->swap_writable = *(fte->pte) & PTE_W;

  spte->is_loaded = false;

  /* Unmap the page from the user's page directory and free the frame */
  pagedir_clear_page(t->pagedir, spte->user_vaddr);

  return true;
}

/* Add an entry to the frame table */
static bool
add_frame_to_table(void *frame)
{
  struct frame_table_entry *fte;
  fte = calloc(1, sizeof *fte);

  if (fte == NULL)
    return false;

  fte->tid = thread_current()->tid;
  fte->frame = frame;

  lock_acquire(&frame_table_lock);
  list_push_back(&frame_table, &fte->elem);
  lock_release(&frame_table_lock);

  return true;
}

/* Remove an entry from the frame table and free the memory */
static void
remove_frame_from_table(void *frame)
{
  struct frame_table_entry *fte;
  struct list_elem *e;

  lock_acquire(&frame_table_lock);
  e = list_head(&frame_table);
  while ((e = list_next(e)) != list_tail(&frame_table))
  {
    fte = list_entry(e, struct frame_table_entry, elem);
    if (fte->frame == frame)
    {
      list_remove(e);
      free(fte);
      break;
    }
  }
  lock_release(&frame_table_lock);
}

/* Retrieve the frame table entry for a given frame */
static struct frame_table_entry *
get_frame_table_entry(void *frame)
{
  struct frame_table_entry *fte;
  struct list_elem *e;

  lock_acquire(&frame_table_lock);
  e = list_head(&frame_table);
  while ((e = list_next(e)) != list_tail(&frame_table))
  {
    fte = list_entry(e, struct frame_table_entry, elem);
    if (fte->frame == frame)
      break;
    fte = NULL;
  }
  lock_release(&frame_table_lock);

  return fte;
}
