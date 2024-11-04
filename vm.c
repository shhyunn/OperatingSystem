#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

struct mmap_area mmap_area_arr[64] = {0};
extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;
  cprintf("start init uvm...\n");
  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;
	cprintf("start copyuvm...\n");
  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

uint mmap(uint addr, int length, int prot, int flags, int fd, int offset) {

  //initialize variables
  int base_addr;
  struct proc *p;
  struct file *f;
  pde_t *d;
  char *mem;
  uint pnum, mid;

  //process & page table
  p = myproc();
  d = p->pgdir;

  //error case
  if (((flags & MAP_ANONYMOUS) != MAP_ANONYMOUS) && fd == -1) {
     return -1;
  } else if ((fd != -1) && (((prot & PROT_READ) != p->ofile[fd]->readable))) {
    return -1;
  } else if (fd != -1 && ((prot & PROT_WRITE) == PROT_WRITE)) {
	if (p->ofile[fd]->writable == 0)
		return -1; 	  
  } 
  if ((addr % PGSIZE) != 0) {
	  return 0;
  }
  //normal case
 
  for (mid = 0; mid < 64; mid++) {
    if (mmap_area_arr[mid].addr == 0) { //empty mmap area pointer
      mmap_area_arr[mid].f = p->ofile[fd];
      mmap_area_arr[mid].p = p;
      mmap_area_arr[mid].addr = MMAPBASE + addr;
      mmap_area_arr[mid].length = length;
      mmap_area_arr[mid].offset = offset;
      mmap_area_arr[mid].prot = prot;
      mmap_area_arr[mid].flags = flags;
      //cprintf("original : %d\n", mmap_area_arr[mid].addr);
      break;
    }
  }

  //deal with flags
  base_addr = MMAPBASE + addr;
  if (flags == 0) { //if flag is 0, no map populate, no anonymous. so only record map areas.
    return base_addr;

  } else if (flags & MAP_POPULATE) { // if flag is map populate, connect virtual memory and physical memory with page table.
    for (pnum = 0; pnum < length; pnum += PGSIZE) {
      mem = kalloc();
      if (mem == 0) { //failed to allocate page
	return -1;

      }

      if (flags & MAP_ANONYMOUS) { //if flag is map populate and map anonymous, physical page is set 0.
        memset(mem, 0, PGSIZE);

      } else {
        //read file data from file pointer
        f = p->ofile[fd] ;
        f->off = offset;
        fileread(f, mem, PGSIZE);
      }

      //mapping virtual address and physical pages
	int pte_flag;
	
	//cprintf("flag result is %d\n", prot & PROT_READ);
	
        if (prot & PROT_WRITE)
		pte_flag = PTE_W | PTE_P | PTE_U;
	else
		pte_flag = PTE_P | PTE_U;	
	
	//cprintf("map addr : %x", base_addr+pnum);
	
	//cprintf("pte_flag : %d\n", pte_flag);
	//cprintf("i'm mappages///\n");
      if (mappages(d, (void*)(base_addr + pnum), PGSIZE, V2P(mem), pte_flag) < 0) {
          kfree(mem);
          return -1;
      }

      //cprintf("mem addr : %x\n", mem);
    }
  }

  return base_addr;

}

void forkmmap(struct proc *p1, struct proc *p2) {
	//cprintf("enter forkmmap..\n");	
	//p1 : curproc, p2 : np
	uint mid;
	int fparr[64] = {-1};
	int fpid = 0;
	int fzarr[64] = {-1};
	int fzid = 0;
	//cprintf("first addr is %d \n", mmap_area_arr[0].addr);

	for (mid = 0; mid < 64; mid++) {
		if (mmap_area_arr[mid].addr != 0 && mmap_area_arr[mid].p == p1) {
			fparr[fpid] = mid;
			fpid++;

		} else if (mmap_area_arr[mid].addr == 0) {
			fzarr[fzid] = mid;
			fzid++;	
		}
				
	}
	//cprintf("fp is : %d, fz is : %d\n", fp, fz);
	int fz = 0;
	int fp = 0;
	if (fparr[0] != -1 && fzarr[0] != -1) {
	    	for (int zid = 0, pid = 0; zid < fzid && pid < fpid; zid++, pid++) {
			fz = fzarr[zid];
			fp = fparr[pid];
			mmap_area_arr[fz].f = mmap_area_arr[fp].f;
			mmap_area_arr[fz].addr = mmap_area_arr[fp].addr;
			mmap_area_arr[fz].length = mmap_area_arr[fp].length;
			mmap_area_arr[fz].offset = mmap_area_arr[fp].offset;
			mmap_area_arr[fz].prot = mmap_area_arr[fp].prot;
			mmap_area_arr[fz].flags = mmap_area_arr[fp].flags;
			mmap_area_arr[fz].p = p2;
			
			struct mmap_area mmarea = mmap_area_arr[fz];
			
			if (mmarea.flags & MAP_POPULATE) {
				char* mem;
				for (uint pnum = 0; pnum < mmarea.length; pnum += PGSIZE) {
					mem = kalloc();
					if (mmarea.flags & MAP_ANONYMOUS) {
						memset(mem, 0, PGSIZE);	
					} else {
						struct file *f = mmarea.f;
						f->off = mmarea.offset;
						fileread(f, mem, PGSIZE);	
					}

					int pte_flag;
					if (mmarea.prot & PROT_WRITE)
						pte_flag = PTE_W | PTE_P | PTE_U;
					else
						pte_flag = PTE_P | PTE_U;

					if (mappages(mmarea.p->pgdir, (void*)(mmarea.addr + pnum), PGSIZE, V2P(mem), pte_flag) < 0)
						kfree(mem);
				//cprintf("fork success...\n");
					
				}	
			}
		}
	}
	return;	
}

int munmap(int addr) {
	uint mid;
	if (((addr-0x40000000) % PGSIZE) != 0) {
		return 0;
	}

	//cprintf("hi ~ I'm munmap!\n");
	for (mid = 0; mid < 64; mid++) {
		if (mmap_area_arr[mid].addr != 0) {
		
			struct mmap_area mmarea = mmap_area_arr[mid];
			if (mmarea.addr == addr && mmarea.p == myproc()) {
				//cprintf("addr : %x\n", addr);
				memset(&mmap_area_arr[mid], 0, sizeof(mmap_area_arr[mid]));
				struct proc *p = mmarea.p;
				pde_t *d = p->pgdir;
				int base_addr = addr;
				pte_t *pte;

				for (int pnum = 0; pnum < mmarea.length; pnum += PGSIZE) {
					pte = walkpgdir(d, (void*)(base_addr+pnum), 0);
					if (*pte & PTE_P) {
						uint pa = PTE_ADDR(*pte);
						char* va = P2V(pa);
						memset(va, 1, PGSIZE);
						kfree(va);
						
						*pte = 0;
					}		
				}

				return 1;			
			}	
		}		
	}
	//cprintf("I'm not exited..\n");	
	return -1;
}

int pfhandler(struct trapframe *tf) {
	uint fault_addr = rcr2();
	//uint read = tf->err & 2;
	uint write = (tf->err & 2) == 1;
	uint mid;
	//cprintf("i'm page fault handler..\n");
	//cprintf("fault addr : %x\n", fault_addr);
	for (mid = 0; mid < 64; mid++) {

		if (mmap_area_arr[mid].addr != 0) {
			struct mmap_area mmarea = mmap_area_arr[mid];

			if (mmarea.addr <= fault_addr && fault_addr < (mmarea.addr + mmarea.length) && mmarea.p == myproc()) {
				//cprintf("start addr : %x\n", mmarea.addr);
				//cprintf("end addr : %x\n", mmarea.addr + mmarea.length);
				if (write && !((mmarea.prot & PROT_WRITE) == PROT_WRITE))
					return -1;
				
				int base_addr = fault_addr;
				//cprintf("allocated\n");

				char* mem = kalloc();
				//cprintf("after allocated\n");
				memset(mem, 0, PGSIZE);
				//cprintf("mem setting..\n");
				if (!(mmarea.flags & MAP_ANONYMOUS)) {
					//cprintf("before file..\n");
					struct file *f = mmarea.f;
					f->off = mmarea.offset;
					//cprintf("before before file..\n");
					fileread(f, mem, PGSIZE);				
					
				}
				pde_t *d = mmarea.p->pgdir;
				//cprintf("after dic..\n");
				//cprintf("page dic setting...\n");
				int pte_flag;
				if (mmarea.prot & PROT_WRITE)
					pte_flag = PTE_W | PTE_P | PTE_U;	
				else
					pte_flag = PTE_P | PTE_U;
				
				//cprintf("before mappage\n");

				if (mappages(d, (void*)base_addr, PGSIZE, V2P(mem), pte_flag) < 0) {
					kfree(mem);
					return -1;
					
				}
				//cprintf("after mappage\n");
				//cprintf("pf handler process succeed!!\n");

				return 0;			
			}
		} 
	}

	
	return -1;
	}
