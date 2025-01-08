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
  if(!(f->writable) && (flags & MAP_SHARED) && (prot & PROT_WRITE)){
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

  // Se comprueba la dirección de memoria de la VMA más reciente; aquella con VA más baja.
  // Empieza buscando por la VA más alta, quitando la página trampolín y el trapframe.
  // Nos estaríamos colocando justo en el comienzo del trapframe.
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
  printf("DEBUG: mmap: Lazy mmap of pid %d at idx %d, addrBegin: %p, len: %d, pages: %d\n",p->pid, vmaIndex, chosenVMA->addrBegin, chosenVMA->length, chosenVMA->length/PGSIZE);

  // Es importante aumentar el número de referencias del fichero para que no sea liberado cuando
  // se cierre pero aún permanezca la VMA mapeada
  filedup(f);

  // En vez de mapear aquí el contenido del fichero, se deja para después (lazy alloc)
  return chosenVMA->addrBegin;
}

int
munmap(void *addr, int length){

  if(length == 0) return 0;
  // Evitar con PGSIZE que un desmapeo de 1 pag desmapee 2.
  if(length % PGSIZE == 0) length -= 1;

  // #1. Obtener la VMA que contiene la dirección addr
  struct proc* p = myproc();
  int idx = 0;
  struct VMA *v;

  // Direcciones del primer y último bloque que contiene el rango a liberar
  uint64 start_pg = PGROUNDDOWN((uint64)addr);
  uint64 end_pg = PGROUNDDOWN((uint64)addr+length); 

  // Buscamos la VMA que contiene el bloque en cuestión
  while(idx < MAX_VMAS){
    v = &p->vmas[idx];
    if(v->used == 1){
      uint64 addrFinal = (uint64)v->addrBegin + v->length;
      uint64 addrInicial = (uint64)v->addrBegin;
      if(addrInicial <= start_pg && end_pg <= addrFinal) // Encontramos la vma
      break;
    }
    idx++;
  }

  // La dirección no pertenece a ninguna VMA
  if(idx >= MAX_VMAS){
    return -1;
  }

  // No permitir agujeros en la VMA
  if(addr != v->addrBegin && 
      (addr+length != v->addrBegin + v->length)){
    return -1;
  } 

  // #2 Escribir en disco si es mapeo compartido
  if(v->flags & MAP_SHARED){
    struct file *f = v->mappedFile;
    begin_op();
    ilock(f->ip);
    writei(f->ip, 1, start_pg, v->offset+(start_pg-(uint64)(v->addrBegin)), v->length);
    iunlock(f->ip);
    end_op();
  }

  // #3 Borrar mapeo con addr y length
  for(uint64 i = start_pg; i <= end_pg; i+=PGSIZE){
    // Lazy alloc puede dar lugar a la existencia de páginas no 
    // válidas si aún no han sido accedidas, debemos comprobar eso
    uint64 pa = 0;
    if((pa = walkaddr(p->pagetable, i)) != 0) {
      if(getref((void*)pa) == 1){
        // El último param hace kfree. Solo usarlo si ref == 1
        uvmunmap(p->pagetable, i, 1, 1);
      }
      else{
        // Desmapear la página del proceso, si no, quedan hojas y da panic en freewalk.
        decref((void*)pa);
        uvmunmap(p->pagetable, i, 1, 0);
      }
      printf("DEBUG: munmap: Valid PTE fre'd of pid %d at idx %d, dir: %p\n",p->pid, idx, (void*)i);
    } else {
      printf("DEBUG: munmap: Lazy PTE fre'd of pid %d at idx %d, dir: %p\n",p->pid, idx, (void*)i);
    }
    // Si borramos la primera página de la vma, hay que poner la siguiente como dir de inicio
    // En caso de borrar todo, no pasa nada por que apunte a una dir incorrecta, ya que se borrará
    // todo al liberar la estructura en el el punto #4
    if(i == (uint64)v->addrBegin) {
      v->addrBegin += PGSIZE;
      v->offset += PGSIZE;
    }
    v->length = v->length - PGSIZE;
  }

  // #4. Liberar fichero si se borra mapeo entero
  if(v->length == 0){
    v->used = 0;
    v->addrBegin = 0;
    v->prot = 0;
    v->flags = 0;
    fileclose(v->mappedFile);
    v->mappedFile = 0;
  }

  return 0;
}

/**
 * Copy p vma table used entries in np vma table
 * 
 * @returns 0 on success, -1 on error.
 */
int
vmacopy(struct proc *p, struct proc * np){

  if(!p || !np) return -1;

  for(int i=0; i < MAX_VMAS; i++){
    // Found used vma entry in p, dup in np
    // Increment file reference, since another proc points to the file now
    if(p->vmas[i].used == 1){
      struct VMA *v = &p->vmas[i];
      struct VMA *nv = &np->vmas[i];
      nv->addrBegin = v->addrBegin;
      nv->flags = v->flags;
      nv->length = v->length;
      nv->mappedFile = filedup(v->mappedFile);
      nv->offset = v->offset;
      nv->prot = v->prot;
      nv->used = v->used;

      // Mapear dirección de la PA del padre. Incrementar referencia de las páginas físicas empleadas
      // De hecho hay que mapear también la PA en la PT porque uvmcopy solo mapea por abajo hasta sz,
      // por lo que nunca llega a mapear las páginas. Igual a la larga esto es un problema si el heap
      // se hiciera grande.
      
      // Por cada VMA, recorrer todas sus páginas y mapearlas en el nuevo proceso.
      int len = v->length;
      if(len % PGSIZE == 0) len -= 1;
      uint64 start_pg = PGROUNDDOWN((uint64)v->addrBegin);
      uint64 end_pg = PGROUNDDOWN((uint64)v->addrBegin + len);
      uint64 pa = 0;
      // Copy On Write:
      // Cambiamos el permiso de escritura a 0 siempre para forzar un fallo de página al intentar
      // escribir en la PA del padre, de tal forma que PTE_R == 0 y PROT_WRITE == 1, caso en el
      // que crearemos una nueva PA para el hijo.
      int perm = PTE_U | (v->prot & PROT_READ ? PTE_R : 0);

      // Igual a lo que hace munmap en #3 
      for(uint64 i = start_pg; i <= end_pg; i+=PGSIZE){
        if((pa = walkaddr(p->pagetable, i)) != 0) {
          // Incrementar referencia a la PA
          incref((void*)pa);
          mappages(np->pagetable, i, PGSIZE, pa, perm);
          printf("DEBUG: vmacopy: Valid PTE mapped from p to np, dir: %p \n", (void*)i);
        } else {
          printf("DEBUG: vmacopy: Lazy PTE mapped from p to np (nothing done), dir %p \n", (void*)i);
        }
      }
    }
  }

  return 0;
}