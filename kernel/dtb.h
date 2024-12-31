/// kernel/dtb.h

#ifndef _DTB_H_
#define _DTB_H_

#include "types.h"

#define MAX_CPUS 8 // Define el número máximo de harts soportados

// Declaración de la variable global para la dirección del DTB
extern uint64 dtb_pa;

// Estructura para almacenar información de cada CPU
struct cpu_info {
    uint64 reg;      // Dirección base de la CPU
    uint32 phandle;  // Phandle de la CPU
    // Agrega más campos según sea necesario
};

// Array para almacenar información de múltiples CPUs
extern struct cpu_info cpu_info_array[MAX_CPUS];

// Contador de CPUs detectadas
extern int cpu_count;

// Declaración de la función principal de inicialización del Device Tree
void dtb_init(void);

// Declaración de funciones utilizadas en dtb.c
void parse_fdt(void *dt_struct_ptr, uint32 struct_size);
void process_uart_prop(const char *prop_name, void *prop_value, uint32 len);
void process_virtio_prop(const char *prop_name, void *prop_value, uint32 len);
void process_plic_prop(const char *prop_name, void *prop_value, uint32 len);
void process_cpu_prop(const char *prop_name, void *prop_value, uint32 len, struct cpu_info *cpu);

// Declaración de funciones auxiliares de cadenas
int strcmp_custom(const char *p, const char *q);
int strncmp_custom(const char *p, const char *q, int n);
void strncpy_custom(char *dest, const char *src, int n);
uint strlen_custom(const char *s);

// Declaración de funciones de inicialización de harts
void init_secondary_hart(void);

#endif // _DTB_H_
