#ifndef __MEMLAYOUT_H__
#define __MEMLAYOUT_H__

#include "dtb.h"
#include "memlayout_assembly.h"

// qemu puts UART registers here in physical memory.
extern uint64 UART0;
extern uint64 UART0_IRQ;

// virtio mmio interface
extern uint64 VIRTIO0;
extern uint64 VIRTIO0_IRQ;

// qemu puts platform-level interrupt controller (PLIC) here.
extern uint64 PLIC;
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

#endif