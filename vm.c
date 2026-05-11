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
#include "buf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// ── Swap slot bitmap ────────────────────────────────────────────
static struct {
  struct spinlock lock;
  uint bitmap[NSWAP_SLOT / 32];  // 8192 slots / 32 = 256 uints
} swap_slots;

// ── FIFO queue of resident user pages (for eviction) ────────────
#define FIFO_SIZE 8192
struct fifo_entry {
  struct proc *proc;
  int pid;           // [방어 로직] 프로세스 생존 및 재사용 확인용
  pde_t *pgdir;      // [방어 로직] exec() 호출로 인한 pgdir 교체 확인용
  uint va;
};
static struct {
  struct spinlock lock;
  struct fifo_entry entries[FIFO_SIZE];
  int head, tail, count;
} fifo;

// ── Forward declarations ─────────────────────────────────────────
static int  alloc_swap_slot(void);
static void free_swap_slot(int slot);
static void fifo_enqueue(struct proc *p, uint va);
static void write_swap_slot(int slot, char *page);
static void read_swap_slot(int slot, char *page);
static int  swap_out(void);
static char *alloc_page_or_swap(void);
static pte_t *walkpgdir(pde_t *pgdir, const void *va, int alloc);

// ── Swap initialization ───────────────────────────────────────────
void
swapinit(void)
{
  initlock(&swap_slots.lock, "swap_slots");
  memset(swap_slots.bitmap, 0, sizeof(swap_slots.bitmap));

  // [핵심 방어] 0번 슬롯을 강제로 할당 처리하여 Lazy 상태(PTE=0)와 구분을 명확히 함
  swap_slots.bitmap[0] |= 1;

  initlock(&fifo.lock, "fifo");
  fifo.head = fifo.tail = fifo.count = 0;
}

// ── Swap slot allocation / free ──────────────────────────────────
static int
alloc_swap_slot(void)
{
  int i, j;
  acquire(&swap_slots.lock);
  for(i = 0; i < NSWAP_SLOT / 32; i++){
    if(swap_slots.bitmap[i] != 0xFFFFFFFF){
      for(j = 0; j < 32; j++){
        if(!(swap_slots.bitmap[i] & (1u << j))){
          swap_slots.bitmap[i] |= (1u << j);
          release(&swap_slots.lock);
          return i * 32 + j;
        }
      }
    }
  }
  release(&swap_slots.lock);
  return -1;
}

static void
free_swap_slot(int slot)
{
  acquire(&swap_slots.lock);
  swap_slots.bitmap[slot / 32] &= ~(1u << (slot % 32));
  release(&swap_slots.lock);
}

// ── FIFO queue ───────────────────────────────────────────────────
static void
fifo_enqueue(struct proc *p, uint va)
{
  acquire(&fifo.lock);
  if(fifo.count < FIFO_SIZE){
    fifo.entries[fifo.tail].proc = p;
    fifo.entries[fifo.tail].pid = p->pid;     // 프로세스 검증용
    fifo.entries[fifo.tail].pgdir = p->pgdir; // 프로세스 검증용
    fifo.entries[fifo.tail].va   = va;
    fifo.tail = (fifo.tail + 1) % FIFO_SIZE;
    fifo.count++;
  }
  release(&fifo.lock);
}

// ── Disk I/O (4KB = 8 sectors × 512B) ───────────────────────────
static void
write_swap_slot(int slot, char *page)
{
  struct buf *b;
  int i;
  for(i = 0; i < 8; i++){
    b = bread(SWAPDEV, SWAP_BASE + slot * 8 + i);
    memmove(b->data, page + i * 512, 512);
    bwrite(b);
    brelse(b);
  }
}

static void
read_swap_slot(int slot, char *page)
{
  struct buf *b;
  int i;
  for(i = 0; i < 8; i++){
    b = bread(SWAPDEV, SWAP_BASE + slot * 8 + i);
    memmove(page + i * 512, b->data, 512);
    brelse(b);
  }
}

// ── Swap-out: evict FIFO head victim to disk ─────────────────────
static int
swap_out(void)
{
  struct fifo_entry victim;
  pte_t *pte;
  int slot;
  uint pa;

  for(;;){
    acquire(&fifo.lock);
    if(fifo.count == 0){
      release(&fifo.lock);
      return -1;
    }
    victim = fifo.entries[fifo.head];
    fifo.head = (fifo.head + 1) % FIFO_SIZE;
    fifo.count--;
    release(&fifo.lock);

    // [핵심 방어 로직] 프로세스가 종료되었거나, pid/pgdir이 변경되어 재사용된 경우 스킵
    if(victim.proc->state == ZOMBIE ||
       victim.proc->state == UNUSED ||
       victim.proc->pid != victim.pid ||
       victim.proc->pgdir != victim.pgdir) {
      continue;
    }

    pte = walkpgdir(victim.proc->pgdir, (void*)victim.va, 0);
    if(pte == 0 || !(*pte & PTE_P))
      continue;

    slot = alloc_swap_slot();
    if(slot < 0)
      return -1;

    pa = PTE_ADDR(*pte);
    write_swap_slot(slot, P2V(pa));

    *pte = (slot << 12) | PTE_SWAPPED;
    // Flush current process TLB; victim's TLB flushed on next switchuvm.
    lcr3(V2P(myproc()->pgdir));
    kfree(P2V(pa));
    return 0;
  }
}

static char *
alloc_page_or_swap(void)
{
  char *mem = kalloc();
  if(mem == 0){
    if(swap_out() < 0) return 0;
    mem = kalloc();
  }
  return mem;
}

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
    } else if(*pte != 0){
      // swapped 페이지 해제 연동 (Obj 2)
      if(*pte & PTE_SWAPPED){
        int slot = (int)(*pte >> 12);
        free_swap_slot(slot);
      }
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
  uint i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      continue;  // lazy heap page: child inherits lazy (no PTE) too
    if(!(*pte & PTE_P)){
      if(*pte & PTE_SWAPPED){
        // swapped 페이지: 자식 프로세스를 위해 새 swap slot에 복사
        int old_slot = (int)(*pte >> 12);
        int new_slot = alloc_swap_slot();
        if(new_slot < 0) goto bad;
        // 자식의 페이지 테이블을 alloc_page_or_swap으로 선제 할당
        // (kalloc 실패 시 swap_out이 지원되도록)
        pde_t *pde_d = &d[PDX(i)];
        if(!(*pde_d & PTE_P)){
          char *pgtab = alloc_page_or_swap();
          if(pgtab == 0){ free_swap_slot(new_slot); goto bad; }
          memset(pgtab, 0, PGSIZE);
          *pde_d = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
        }
        char *tmp = alloc_page_or_swap();
        if(tmp == 0){ free_swap_slot(new_slot); goto bad; }
        read_swap_slot(old_slot, tmp);
        write_swap_slot(new_slot, tmp);
        kfree(tmp);
        pte_t *cpte = walkpgdir(d, (void*)i, 1);
        if(cpte == 0){ free_swap_slot(new_slot); goto bad; }
        *cpte = ((uint)new_slot << 12) | PTE_SWAPPED;
      }
      continue;
    }
    flags = PTE_FLAGS(*pte);
    // 자식의 페이지 테이블을 alloc_page_or_swap으로 선제 할당
    // (물리 메모리 고갈 시 mappages 내부 kalloc 실패를 방지)
    pde_t *pde_d = &d[PDX(i)];
    if(!(*pde_d & PTE_P)){
      char *pgtab = alloc_page_or_swap();
      if(pgtab == 0) goto bad;
      memset(pgtab, 0, PGSIZE);
      *pde_d = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
    }
    if((mem = alloc_page_or_swap()) == 0)
      goto bad;
    // alloc_page_or_swap이 swap_out을 통해 지금 복사 중인 페이지를
    // 스왑아웃했을 수 있으므로 부모 PTE를 다시 확인
    {
      pte_t *cur_pte = walkpgdir(pgdir, (void*)i, 0);
      if(cur_pte && (*cur_pte & PTE_P)){
        memmove(mem, (char*)P2V(PTE_ADDR(*cur_pte)), PGSIZE);
      } else if(cur_pte && (*cur_pte & PTE_SWAPPED)){
        // 복사 중 스왑아웃됨 → swap에서 읽어 자식에게 복사
        read_swap_slot(*cur_pte >> 12, mem);
      } else {
        kfree(mem);
        goto bad;
      }
    }
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

int
handle_pagefault(uint fault_addr, uint err)
{
  struct proc *p = myproc();
  char *mem;
  uint va;
  pte_t *pte;

  if(fault_addr >= KERNBASE)
    return -1;

  va = PGROUNDDOWN(fault_addr);
  uint guard_va = p->stack_top - 5 * PGSIZE;

  // ── 0. Swap-in ───────────────────────────────────────────────────
  pte = walkpgdir(p->pgdir, (char*)va, 0);
  if(pte && !(*pte & PTE_P) && (*pte & PTE_SWAPPED)){
    int slot = (int)(*pte >> 12);
    mem = alloc_page_or_swap();
    if(mem == 0) return -1;
    read_swap_slot(slot, mem);
    free_swap_slot(slot);
    *pte = V2P(mem) | PTE_W | PTE_U | PTE_P;
    lcr3(V2P(p->pgdir));
    fifo_enqueue(p, va);
    return 0;
  }

  // ── 1. ELF demand paging: [0, elf_end) (Obj 3) ───────────────────
  if(fault_addr < guard_va && p->elf_ip != 0){
    int s;
    for(s = 0; s < p->elf_nseg; s++){
      uint seg_start = p->elf_segs[s].vaddr;
      uint seg_end   = p->elf_segs[s].vaddr + p->elf_segs[s].memsz;
      if(fault_addr >= seg_start && fault_addr < seg_end)
        break;
    }
    if(s >= p->elf_nseg)
      return -1;

    mem = alloc_page_or_swap();
    if(mem == 0) return -1;
    memset(mem, 0, PGSIZE);

    uint seg_off  = va - p->elf_segs[s].vaddr;
    uint file_off = p->elf_segs[s].off + seg_off;

    if(seg_off < p->elf_segs[s].filesz){
      uint n = PGSIZE;
      if(seg_off + n > p->elf_segs[s].filesz)
        n = p->elf_segs[s].filesz - seg_off;  // clamp at filesz; BSS is already zeroed
      ilock(p->elf_ip);
      if(readi(p->elf_ip, mem, file_off, n) != (int)n){
        iunlock(p->elf_ip);
        kfree(mem);
        return -1;
      }
      iunlock(p->elf_ip);
    }
    if(mappages(p->pgdir, (char*)va, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      kfree(mem);
      return -1;
    }
    fifo_enqueue(p, va);
    return 0;
  }

  // ── 2. Guard page ─────────────────────────────────────────────────
  if(fault_addr >= guard_va && fault_addr < guard_va + PGSIZE)
    return -1;

  // ── 3. Stack growth: [guard+PGSIZE, stack_bottom) ────────────────
  if(fault_addr >= guard_va + PGSIZE && fault_addr < p->stack_bottom){
    mem = alloc_page_or_swap();
    if(mem == 0) return -1;
    memset(mem, 0, PGSIZE);
    if(mappages(p->pgdir, (char*)va, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      kfree(mem);
      return -1;
    }
    p->stack_bottom = va;
    fifo_enqueue(p, va);
    return 0;
  }

  // ── 4. Heap lazy allocation: [stack_top, proc->sz) ───────────────
  if(fault_addr >= p->stack_top && fault_addr < p->sz){
    mem = alloc_page_or_swap();
    if(mem == 0) return -1;
    memset(mem, 0, PGSIZE);
    if(mappages(p->pgdir, (char*)va, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      kfree(mem);
      return -1;
    }
    fifo_enqueue(p, va);
    return 0;
  }

  return -1;
}
