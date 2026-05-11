#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint argc, sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct inode *elf_ip_ref = 0;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pgdir = setupkvm()) == 0)
    goto bad;

  sz = 0;
  int nseg = 0;
  struct {
    uint vaddr; uint memsz; uint filesz; uint off;
  } segs[4];

  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    if(nseg >= 4)
      goto bad;
    segs[nseg].vaddr  = ph.vaddr;
    segs[nseg].memsz  = ph.memsz;
    segs[nseg].filesz = ph.filesz;
    segs[nseg].off    = ph.off;
    nseg++;
    if(ph.vaddr + ph.memsz > sz)
      sz = ph.vaddr + ph.memsz;
  }

  elf_ip_ref = idup(ip);
  iunlockput(ip);
  end_op();
  ip = 0;

  // Allocate five pages at the next page boundary.
  // Make the first inaccessible.  Use the other as the user stack.
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pgdir, sz, sz + PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - PGSIZE));

  if((sz = allocuvm(pgdir, sz + PGSIZE*3, sz + PGSIZE*4)) == 0)
    goto bad;
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*4;  // argv pointer

  sp -= (3+argc+1) * 4;
  if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Commit to the user image.
  oldpgdir = curproc->pgdir;

  if(curproc->elf_ip)
    iput(curproc->elf_ip);

  curproc->pgdir        = pgdir;
  curproc->sz           = sz;
  curproc->stack_top    = sz;
  curproc->stack_bottom = sz - PGSIZE;
  curproc->tf->eip      = elf.entry;
  curproc->tf->esp      = sp;

  curproc->elf_ip   = elf_ip_ref;
  curproc->elf_end  = curproc->stack_top - 5 * PGSIZE;
  curproc->elf_nseg = nseg;
  for(i = 0; i < nseg; i++){
    curproc->elf_segs[i].vaddr  = segs[i].vaddr;
    curproc->elf_segs[i].memsz  = segs[i].memsz;
    curproc->elf_segs[i].filesz = segs[i].filesz;
    curproc->elf_segs[i].off    = segs[i].off;
  }

  switchuvm(curproc);
  freevm(oldpgdir);
  return 0;

 bad:
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  if(elf_ip_ref)
    iput(elf_ip_ref);
  return -1;
}
