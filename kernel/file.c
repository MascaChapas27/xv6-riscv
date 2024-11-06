//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}

// Escribe en un punto específico de un nodo-i en vez de al final
int
inodeinsert(struct file *f, uint64 addr, int n, uint64 offset)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, offset+i, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("inodeinsert: called with a non-inode file");
  }

  return ret;
}

void *
mmap(void *addr, int length, int prot, int flags, struct file* f, int offset){

  // No se pueden mapear 0 bytes o una longitud que no sea múltiplo del tamaño de página
  if(length == 0 || length % PGSIZE != 0){
    return ((void*)(char*) -1);
  }

  // Si un fichero se mapea de forma compartida y no se puede escribir, tampoco se puede
  // en su mapeo
  if(!f->writable && (flags & MAP_SHARED) && (prot & PROT_WRITE)){
    return ((void*)(char*) -1);
  }

  struct proc* p = myproc();

  int vmaIndex = 0;

  // Encontrar una VMA vacía
  while(p->vmas[vmaIndex].used && vmaIndex < MAX_VMAS){
    vmaIndex++;
  }

  // No hay VMAs disponibles
  if(vmaIndex >= MAX_VMAS){
    return ((void*)(char*) -1);
  }

  struct VMA * chosenVMA = &(p->vmas[vmaIndex]);

  // Se rellena todo lo que sabemos de momento
  chosenVMA->length = length;
  chosenVMA->prot = prot;
  chosenVMA->flags = flags;
  chosenVMA->offset = offset;
  chosenVMA->mappedFile = f;
  chosenVMA->used = 1;

  // Se comprueba la dirección de memoria de la VMA más abajo (se comienza por el final quitándole 2
  // páginas, ya que nos saltamos el trampolín y el trapframe. Nos estaríamos colocando justo en el
  // comienzo del trapframe)
  void* addrLowestVMA = (void*)(MAXVA - 2*PGSIZE);

  for(int i=0;i<MAX_VMAS;i++){
    if(i == vmaIndex) continue;

    if(p->vmas[i].used && p->vmas[i].addrBegin < addrLowestVMA){
      addrLowestVMA = p->vmas[i].addrBegin;
    }
  }

  // En addrLowestVMA tenemos la dirección alineada al tamaño de página donde está la VMA de más
  // abajo. Colocaremos la siguiente justo debajo. Esto se puede hacer así porque hemos comprobado
  // al principio que length es múltiplo del tamaño de página y distinto de cero
  chosenVMA->addrBegin = addrLowestVMA-length;
  printf("DEBUG: mmapeado fichero a %p\n",chosenVMA->addrBegin);

  // Es importante aumentar el número de referencias del fichero para que no sea liberado cuando
  // se cierre pero aún permanezca la VMA mapeada
  filedup(f);

  // En vez de mapear aquí el contenido del fichero, se deja para después (lazy alloc)
  return chosenVMA->addrBegin;
}

int
munmap(void *addr, int length){

  // Si la longitud final de la VMA llega a 0, poner la VMA a unused y hacer fileclose()

  struct proc* p = myproc();

  int vmaIndex = 0;

  // Encontrar la VMA
  while(vmaIndex < MAX_VMAS && !(p->vmas[vmaIndex].used && (p->vmas[vmaIndex].addrBegin <= addr && addr < (void*)((uint64)p->vmas[vmaIndex].addrBegin + (uint64)p->vmas[vmaIndex].length)))){
    vmaIndex++;
  }

  // La dirección no pertenece a ninguna VMA
  if(vmaIndex >= MAX_VMAS){
    return -1;
  }

  struct VMA * chosenVMA = &(p->vmas[vmaIndex]);

  // No se pueden hacer huecos enmedio
  if(addr != chosenVMA->addrBegin && (addr+length != chosenVMA->addrBegin+chosenVMA->length)){
    return -1;
  }

  char data[PGSIZE];

  if(addr == chosenVMA->addrBegin)

  for(uint64 i=0;i*PGSIZE < length;i++){
    
    // Si la página está sucia, se escribe en el fichero
    if(*walk(p->pagetable,(uint64)(addr+i*PGSIZE),0) & PTE_D){
      // Se copian los datos del usuario al kernel y se escriben en el fichero
      copyin(p->pagetable,data,(uint64)addr,PGSIZE);
      inodeinsert(chosenVMA->mappedFile,data,PGSIZE,chosenVMA->mappedFile->off+i*PGSIZE);
    }
  }

  // Se modifica la longitud y la dirección de inicio si es necesario
  if(addr == chosenVMA->addrBegin){
    chosenVMA->addrBegin = addr+length;
    chosenVMA->length-=length;
  } else if (addr+length != chosenVMA->addrBegin+chosenVMA->length){
    chosenVMA->length-=length;
  } else {
    // No se pueden hacer huecos enmedio
    return -1;
  }

  // Si el mapeo se queda vacío, se borra
  if(chosenVMA->length == 0){
    chosenVMA->used = 0;
    fileclose(chosenVMA->mappedFile);
  }

  return 0;
}