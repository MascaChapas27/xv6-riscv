// kernel/dtb.c

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "dtb.h"

// Definición de la variable global para almacenar la dirección del DTB
uint64 dtb_pa = 0;

// Definición de la estructura para almacenar información de cada CPU
struct cpu_info cpu_info_array[MAX_CPUS];
int cpu_count = 0;

// Variable global con la dirección base del UART
uint64 UART0;

// Variable global con el número de interrupción de UART
uint64 UART0_IRQ;

// Variable global con la dirección base del VIRTIO
uint64 VIRTIO0;

// Variable global con el número de interrupción de VIRTIO
uint64 VIRTIO0_IRQ;

// Variable global con la dirección base del PLIC
uint64 PLIC;

// Estructura que representa la cabecera del Device Tree Blob (DTB)
struct fdt_header {
    uint32 magic;
    uint32 totalsize;
    uint32 off_dt_struct;
    uint32 off_dt_strings;
    uint32 off_mem_rsvmap;
    uint32 version;
    uint32 last_comp_version;
    uint32 boot_cpuid_phys;
    uint32 size_dt_strings;
    uint32 size_dt_struct;
};

// Definiciones de constantes utilizadas en el parser
#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  0x1
#define FDT_END_NODE    0x2
#define FDT_PROP        0x3
#define FDT_NOP         0x4
#define FDT_END         0x9

// Definición para la profundidad máxima del árbol de nodos
#define MAX_DEPTH 10

// Definición para el tamaño máximo del nombre del nodo
#define MAX_NODE_NAME 128

// Definiciones de alineación
#define ALIGN4(x) (((x) + 3) & ~3)

// Variables estáticas para almacenar offsets y punteros
static uint64 dt_struct;
static uint64 dt_strings;
static uint32 dt_totalsize;

// Número de address cells y size cells de cada nivel
static uint32 addressCells[MAX_DEPTH];
static uint32 sizeCells[MAX_DEPTH];

// Variables para manejar la pila de nodos
static char node_stack[MAX_DEPTH][MAX_NODE_NAME];
static int current_depth = 0;

// Declaración de funciones auxiliares
int strcmp_custom(const char *p, const char *q);
int strncmp_custom(const char *p, const char *q, int n);
void strncpy_custom(char *dest, const char *src, int n);
uint strlen_custom(const char *s);

// Función para convertir uint32 de big-endian a little-endian
uint32
swap_uint32(uint32 val)
{
    return ((val >> 24) & 0x000000ff) |
           ((val >> 8)  & 0x0000ff00) |
           ((val << 8)  & 0x00ff0000) |
           ((val << 24) & 0xff000000);
}

// Función para convertir uint64 de big-endian a little-endian
uint64
swap_uint64(uint64 val)
{
    return ((val & 0x00000000000000FFULL) << 56) |
           ((val & 0x000000000000FF00ULL) << 40) |
           ((val & 0x0000000000FF0000ULL) << 24) |
           ((val & 0x00000000FF000000ULL) << 8)  |
           ((val & 0x000000FF00000000ULL) >> 8)  |
           ((val & 0x0000FF0000000000ULL) >> 24) |
           ((val & 0x00FF000000000000ULL) >> 40) |
           ((val & 0xFF00000000000000ULL) >> 56);
}

// Obtiene una dirección de memoria en base a una o dos address cells
uint64
obtainAddress(void *ptr, uint32 len){
    uint64 addr = 0;

    if (len >= 8) { // 2 * 4 bytes para address cells
        uint32 addr_high = swap_uint32(((uint32 *)ptr)[0]);
        uint32 addr_low = swap_uint32(((uint32 *)ptr)[1]);
        addr = ((uint64)addr_high << 32) | addr_low;
    }
    else if (len >= 4) { // Solo una address cell
        addr = swap_uint32(*(uint32 *)ptr);
    }

    return addr;
}

// Función principal de inicialización del Device Tree
void
dtb_init(void)
{
    if (dtb_pa == 0) {
        panic("DTB address not set");
    }

    struct fdt_header *header = (struct fdt_header *)dtb_pa;

    // Verificar el magic number para asegurar que es un DTB válido
    if (swap_uint32(header->magic) != FDT_MAGIC) {
        panic("Invalid FDT magic number");
    }

    // Obtener el tamaño total del DTB y verificar su integridad mínima
    dt_totalsize = swap_uint32(header->totalsize);
    if (dt_totalsize < sizeof(struct fdt_header)) {
        panic("DTB totalsize too small");
    }

    // Obtener los offsets a las secciones dt_struct y dt_strings
    dt_struct = (uint64)dtb_pa + swap_uint32(header->off_dt_struct);
    dt_strings = (uint64)dtb_pa + swap_uint32(header->off_dt_strings);

    // Parsear el árbol de dispositivos
    parse_fdt((void *)dt_struct, dt_totalsize - swap_uint32(header->off_dt_struct));
}

// Parser del Device Tree
void
parse_fdt(void *dt_struct_ptr, uint32 struct_size)
{
    uint8 *end = (uint8 *)dt_struct_ptr + struct_size;
    uint32 *token = (uint32 *)dt_struct_ptr;

    struct cpu_info *current_cpu = 0;

    while ((uint8 *)token < end) {

        uint32 tag = swap_uint32(*token++);

        if (tag == FDT_BEGIN_NODE) {
            // Inicio de un nuevo nodo
            char *name = (char *)token;
            int name_len = strlen_custom(name) + 1; // Incluir el terminador nulo
            if (current_depth >= MAX_DEPTH) {
                panic("Device Tree too deeply nested");
            }
            strncpy_custom(node_stack[current_depth], name, sizeof(node_stack[current_depth]) - 1);
            node_stack[current_depth][sizeof(node_stack[current_depth]) - 1] = '\0';

            // Verificar si el nodo es un CPU
            if (strncmp_custom(name, "cpu@", 4) == 0) {
                if (cpu_count >= MAX_CPUS) {
                    panic("Too many CPUs in Device Tree");
                }
                current_cpu = &cpu_info_array[cpu_count];
                cpu_count++;
            }

            current_depth++;

            // Se ponen a cero los address cells y size cells del nivel actual
            addressCells[current_depth] = 0;
            sizeCells[current_depth] = 0;

            // Avanzar el puntero token al siguiente token después del nombre del nodo
            token += (ALIGN4(name_len) / 4);
        }
        else if (tag == FDT_END_NODE) {
            // Fin del nodo actual
            if (current_depth <= 0) {
                panic("Device Tree depth underflow");
            }

            // Verificar si estamos saliendo de un nodo CPU
            char *current_node = node_stack[current_depth - 1];
            if (strncmp_custom(current_node, "cpu@", 4) == 0) {
                current_cpu = 0;
            }

            current_depth--;
            node_stack[current_depth][0] = '\0'; // Limpiar el nombre del nodo
        }
        else if (tag == FDT_PROP) {
            // Propiedad del nodo
            if ((uint8 *)token + 8 > end) {
                panic("DTB parse out of bounds while reading FDT_PROP header");
            }
            uint32 len = swap_uint32(*token++);
            uint32 nameoff = swap_uint32(*token++);
            char *prop_name = (char *)(dt_strings + nameoff);
            void *prop_value = (void *)token;

            // Verificar que no se exceda el tamaño del DTB al leer la propiedad
            if ((uint8 *)token + ALIGN4(len) > end) {
                panic("DTB parse out of bounds while reading property value");
            }

            // Procesar la propiedad si estamos en el nodo de interés
            if (current_depth > 0) {
                char *current_node = node_stack[current_depth - 1];

                // printf("Nodo: [%s] Propiedad: [%s]\n",current_node,prop_name);

                // Procesar propiedades de UART
                if (strncmp_custom(current_node, "serial", 6) == 0 || strncmp_custom(current_node, "uart", 4) == 0) {
                    process_uart_prop(prop_name, prop_value, len);
                }

                // Procesar propiedades de VIRTIO
                if (strncmp_custom(current_node, "virtio_mmio", 11) == 0) {
                    process_virtio_prop(prop_name, prop_value, len);
                }

                // Procesar propiedades de PLIC
                if (strncmp_custom(current_node, "interrupt-controller", 20) == 0) {
                    process_plic_prop(prop_name, prop_value, len);
                }

                // Procesar propiedades de CPU si estamos dentro de un nodo CPU
                if (current_cpu != 0) {
                    process_cpu_prop(prop_name, prop_value, len, current_cpu);
                }

                // Procesar número de address cells
                if(strcmp_custom(prop_name, "#address-cells") == 0){
                    addressCells[current_depth] = swap_uint32(((uint32 *)prop_value)[0]);
                }

                // Procesar número de size cells
                if(strcmp_custom(prop_name, "#size-cells") == 0){
                    sizeCells[current_depth] = swap_uint32(((uint32 *)prop_value)[0]);
                }

                // Procesar otras propiedades de nodos si es necesario
                // ...
            }

            // Avanzar el puntero token al siguiente token después de la propiedad
            token = (uint32 *)((uint8 *)token + ALIGN4(len));
        }
        else if (tag == FDT_NOP) {
            // No hacer nada (NOP)
        }
        else if (tag == FDT_END) {
            // Fin del Device Tree
            break;
        }
        else {
            panic("Unknown FDT tag");
        }
    }

    // Validar que se hayan cerrado todos los nodos
    if (current_depth != 0) {
        panic("Device Tree parsing ended with unclosed nodes");
    }
}

// Encuentra el número de #address-cells y #size-cells
void
findCells(uint32* argAddressCells, uint32* argSizeCells){
    *argAddressCells = addressCells[current_depth];
    *argSizeCells = sizeCells[current_depth];

    uint32 depth = current_depth;

    while(depth > 0 && *argAddressCells == 0 && *argSizeCells == 0){
        depth--;
        *argAddressCells = addressCells[depth];
        *argSizeCells = sizeCells[depth];
    }

    if(depth == 0){
        panic("No #address-cells and #size-cells found");
    }
}

// Procesar propiedades específicas del nodo UART
void
process_uart_prop(const char *prop_name, void *prop_value, uint32 len)
{
    // Se leen las propiedades
    if (strcmp_custom(prop_name, "reg") == 0) {
        uint32 currentAddressCells;
        uint32 currentSizeCells;

        findCells(&currentAddressCells,&currentSizeCells);

        // Se comprueba que la longitud es correcta
        if(len != 4*currentAddressCells + 4*currentSizeCells)
            panic("Invalid 'reg' property length for UART");

        UART0 = obtainAddress(prop_value,4*currentAddressCells);

    } else if (strcmp_custom(prop_name, "interrupts") == 0) {
        UART0_IRQ = obtainAddress(prop_value,4);
    }
}

// Procesar propiedades específicas del nodo VIRTIO
void
process_virtio_prop(const char *prop_name, void *prop_value, uint32 len)
{
    // Se leen las propiedades
    if (strcmp_custom(prop_name, "reg") == 0) {
        uint32 currentAddressCells;
        uint32 currentSizeCells;

        findCells(&currentAddressCells,&currentSizeCells);

        // Se comprueba que la longitud es correcta
        if(len != 4*currentAddressCells + 4*currentSizeCells)
            panic("Invalid 'reg' property length for VIRTIO");

        VIRTIO0 = obtainAddress(prop_value,4*currentAddressCells);

    } else if (strcmp_custom(prop_name, "interrupts") == 0) {
        VIRTIO0_IRQ = obtainAddress(prop_value,4);
    }
}

// Procesar propiedades específicas del nodo PLIC
void
process_plic_prop(const char *prop_name, void *prop_value, uint32 len)
{
    // Se leen las propiedades
    if (strcmp_custom(prop_name, "reg") == 0) {
        uint32 currentAddressCells;
        uint32 currentSizeCells;

        findCells(&currentAddressCells,&currentSizeCells);

        // Se comprueba que la longitud es correcta
        if(len != 4*currentAddressCells + 4*currentSizeCells)
            panic("Invalid 'reg' property length for PLIC");

        PLIC = obtainAddress(prop_value,4*currentAddressCells);
    }
}

// Procesar propiedades específicas de cada CPU
void
process_cpu_prop(const char *prop_name, void *prop_value, uint32 len, struct cpu_info *cpu)
{
    if (strcmp_custom(prop_name, "reg") == 0) {
        // Interpretar la propiedad 'reg' considerando #address-cells y #size-cells
        // Asumimos #address-cells=2 y #size-cells=0 (común para CPUs)
        if (len >= 8) { // 2 * 4 bytes para address cells
            uint32 addr_high = swap_uint32(((uint32 *)prop_value)[0]);
            uint32 addr_low = swap_uint32(((uint32 *)prop_value)[1]);
            uint64 addr = ((uint64)addr_high << 32) | addr_low;
            cpu->reg = addr;
        }
        else if (len >= 4) { // Solo una address cell
            uint32 addr = swap_uint32(*(uint32 *)prop_value);
            cpu->reg = (uint64)addr;
        }
        else {
            panic("Invalid 'reg' property length for CPU");
        }
    } else if (strcmp_custom(prop_name, "phandle") == 0 && len >= 4) {
        uint32 phandle = swap_uint32(*(uint32 *)prop_value);
        cpu->phandle = phandle;
    }
}

// Función para comparar cadenas
int
strcmp_custom(const char *p, const char *q)
{
    while (*p && (*p == *q)) {
        p++;
        q++;
    }
    return (uint8)*p - (uint8)*q;
}

// Función para comparar los primeros n caracteres de dos cadenas
int
strncmp_custom(const char *p, const char *q, int n)
{
    for (int i = 0; i < n; i++) {
        if (p[i] != q[i]) {
            return (uint8)p[i] - (uint8)q[i];
        }
        if (p[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

// Función para copiar cadenas con límite de longitud
void
strncpy_custom(char *dest, const char *src, int n)
{
    int i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dest[i] = src[i];
    dest[i] = '\0';
}

// Función para obtener la longitud de una cadena
uint
strlen_custom(const char *s)
{
    uint n = 0;
    while (s[n])
        n++;
    return n;
}
