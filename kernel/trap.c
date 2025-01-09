#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "file.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(killed(p))
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();

  } else if(r_scause() == 13 || r_scause() == 15){
    // Fallo de página al leer (13) o al escribir (15) mientras se ejecutaba código de usuario

    // Primero, se comprueba si la dirección que ha dado fallo (stval) está dentro de alguna VMA
    // (se saca la dirección del primer byte de la página en la que se encuentra para simplificar)
    void* faultAddr = (void*)(r_stval() & ~(PGSIZE-1));
    int vmaIndex = 0;
    while(vmaIndex < MAX_VMAS && !(p->vmas[vmaIndex].used && (p->vmas[vmaIndex].addrBegin <= faultAddr && faultAddr < (void*)((uint64)p->vmas[vmaIndex].addrBegin + (uint64)p->vmas[vmaIndex].length)))){
      vmaIndex++;
    }

    // La dirección no pertenece a ninguna VMA
    if(vmaIndex >= MAX_VMAS){
      printf("vma_address: %p \n", (void*)r_stval());
      panic("usertrap: addr not in vmas");
    }

    // Comprobar si el fallo viene dado por falta de permisos.
    struct VMA * v = &p->vmas[vmaIndex];
    if(v->prot & PROT_NONE){
      panic("usertrap: operation on non accesible memory");
    }
    if(!(v->prot & PROT_READ) && r_scause() == 13){
      panic("usertrap: Read on non readable memory");
    }
    if(!(v->prot & PROT_WRITE) && r_scause() == 15){
      panic("usertrap: Write on non writable memory");
    }
    

    pte_t *pte = walk(p->pagetable, (uint64)faultAddr, 1);
    uint64 pa = walkaddr(p->pagetable, (uint64)faultAddr);
    int perm = PTE_U | (p->vmas[vmaIndex].prot & PROT_READ ? PTE_R : 0) | (p->vmas[vmaIndex].prot & PROT_WRITE ? PTE_W : 0);
    //printf("DEBUG: usertrap: PROT_WRITE: %d. PTE_W: %ld. PA: %p.\n",v->prot & PROT_WRITE, *pte & PTE_W, (void*)pa);

   
    // Caso COW: Existe PA asociada a la VA, se puede escribir 
    // en el mapeo pero no en la VA asociada a la PTE del proceso.
    if(pa != 0 && (v->prot & PROT_WRITE) && !(*pte & PTE_W)){
      // Caso especial asociado a desmapeo por COW.
      // Se queda la PA antigua con una sola referencia, activar PTE_W.
      if(getref((void*)pa) == 1){
        if(DEBUG) printf("DEBUG: usertrap: COW, special case, activating PTE_W.\n");
        uvmunmap(p->pagetable, (uint64)faultAddr, 1, 0);
        mappages(p->pagetable, (uint64)faultAddr, PGSIZE, pa, perm);
        usertrapret(); // Salimos de usertrap
      }

      // Caso general.
      // Nueva PA para el proceso actual. -> activar PTE_W.
      // Decrementar referencia a la antigua PA.
      // Si tras esto la antigua PA solo tiene una referencia 
      // activar PTE_W (cuando intente escribir el otro proceso
      // que aún la usa, es el caso especial de arriba).
      if(DEBUG) printf("DEBUG: usertrap: COW, removing mapping with new PA.\n");
      uvmunmap(p->pagetable, (uint64)faultAddr, 1, 0);
      decref((void*)pa);
      if(DEBUG) printf("DEBUG: usertrap: Lazy alloc miss of pid %d at dir %p, mapping...\n", p->pid, faultAddr);
      char *newPa = (char*)kalloc();

      if(newPa == 0)
        panic("usertrap: kallocn't");

      memmove((void*)newPa, (void*)pa, PGSIZE);
      mappages(p->pagetable, (uint64)faultAddr, PGSIZE, (uint64)newPa, perm);
      if(DEBUG) printf("DEBUG: usertrap: mappages success. PA: %p\n", (void *)newPa);
      usertrapret(); // Salimos de usertrap
    }
    
    if(DEBUG) printf("DEBUG: usertrap: Lazy alloc miss of pid %d at dir %p, mapping...\n", p->pid, faultAddr);
    // Si la dirección pertenece a una VMA, primero sacamos una página física
    char *physPage = (char*)kalloc();

    if(physPage == 0)
      panic("usertrap: kallocn't");

    // Llenamos la página de ceros por si acaso
    memset(physPage,0,PGSIZE);
        
    // Ahora, la llenamos con los siguientes 4096 (como máximo) bytes de datos del fichero. Tenemos
    // que obtener el cerrojo del fichero primero
    struct inode* inodeptr = p->vmas[vmaIndex].mappedFile->ip;
    uint64 fileOffset = (uint64)(p->vmas[vmaIndex].offset + (faultAddr-p->vmas[vmaIndex].addrBegin));

    ilock(inodeptr);
    readi(inodeptr,0,(uint64)physPage,fileOffset,PGSIZE);
    iunlock(inodeptr);
    
    // Ahora que se ha conseguido leer el contenido a una página física, tenemos que mapearla a una
    // página virtual en el proceso
    if(mappages(p->pagetable,(uint64)faultAddr,PGSIZE,(uint64)physPage,perm) == 0){
      if(DEBUG) printf("DEBUG: usertrap: mappages success. PA: %p\n", (void *)physPage);
    } else {
      if(DEBUG) printf("DEBUG: usertrap: mappages error.\n");
    }
  

  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause 0x%ld pid=%d VA: %p\n", r_scause(), p->pid, (void*)(r_stval() & ~(PGSIZE-1)));
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if(killed(p))
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to userret in trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    // interrupt or trap from an unknown source
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + CLOCKTICKS);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

