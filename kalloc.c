// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file 
                   // defined by the kernel linker script in kernel.l
struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

struct spinlock lru_lock;

struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head = 0;
int num_free_pages = 0;
int num_lru_pages = 0;
char* bitmap;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  initlock(&lru_lock, "lru_lock");
  //num_free_pages = 0;
  //num_lru_pages = 0;
  //page_lru_head = 0;
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE) {
    kfree(p);
    pages[num_free_pages].vaddr = p;
    num_free_pages++;
  }

}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{
  struct run *r;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;
//try_again:

  if (kmem.use_lock)
    acquire(&kmem.lock);

  r = kmem.freelist;
//  if(!r && reclaim())
//	  goto try_again;
  if (r) {
    kmem.freelist = r->next;

  } else if (!r) {
	if (kmem.use_lock)
		release(&kmem.lock);
	r = (struct run*)swapout();
	if (!r) {
		cprintf("OOM ERROR\n");
		return 0; 
	}
	return (char*)r;	  		  
  }
	   
  if (kmem.use_lock)
    release(&kmem.lock);
  
  return (char*)r;
}

int
freemem(void) {
	struct run *r;
	int pnum = 0;

	if (kmem.use_lock)
		acquire(&kmem.lock);
	
	r = kmem.freelist;
	if (r) {
		while (r->next != 0) {
			pnum += 1;
			r = r->next;
			}
		}
	
	if (kmem.use_lock)
		release(&kmem.lock);
	
	return pnum;
	}


void
lru_insert(char* mem, pde_t *pgdir, char* vaddr) {
	for (int n = 0; n < PHYSTOP / PGSIZE; n++) {
		if (pages[n].vaddr == mem && pages[n].pgdir == 0) {
			acquire(&lru_lock);
			pages[n].vaddr = vaddr;
			pages[n].pgdir = pgdir;

			if (num_lru_pages == 0) {
				pages[n].prev = &pages[n];
				pages[n].next = &pages[n];
				page_lru_head = &pages[n];
			} else {
				pages[n].prev = page_lru_head->prev;
				page_lru_head->prev->next =&pages[n];
				page_lru_head->prev = &pages[n];
				pages[n].next = page_lru_head;	
				page_lru_head = &pages[n];
			}
			num_lru_pages++;
			release(&lru_lock);
			break;
		}	
	}	
}

void
lru_delete(char* mem, pde_t *pgdir, char* vaddr) {
	for (int n = 0; n < PHYSTOP / PGSIZE; n++) {
		if (pages[n].vaddr == vaddr && pages[n].pgdir == pgdir) {
			acquire(&lru_lock);
			pages[n].vaddr = mem;
			pages[n].pgdir = 0;	
		
		
			if (num_lru_pages == 1) {
				pages[n].prev = 0;
				pages[n].next = 0;
				page_lru_head = 0;
			} else {
				pages[n].prev->next = pages[n].next;
				pages[n].next->prev = pages[n].prev;
				pages[n].prev = 0;
				pages[n].next = 0;
			}
			num_lru_pages--;
			release(&lru_lock);
			break;
		}
	}	
		
}
