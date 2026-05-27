/*
 * QEMU model of the RISC-V vcore development board.
 *
 * This file implements the core logic for the vcore board, including:
 * 1. Board initialization and memory layout setup
 * 2. CPU and interrupt controller configuration
 * 3. Peripheral device instantiation (UARTs, Ethernet controllers)
 * 4. Device Tree (DTB) generation for guest OS
 * 5. Boot firmware loading and kernel setup
 *
 * Based on the RISC-V vcore board specifications provided by the user:
 * - Dual UARTs: NS16550 at 0x30009000, Xilinx UART Lite at 0x40600000
 * - Dual Ethernet: Xilinx AXI Ethernet at 0x40c00000, Synopsys DWMAC at 0x310a0000
 * - AXI DMA for Ethernet at 0x41e00000
 */

#include "qemu/osdep.h"              /* OS-dependent utilities */
#include "qemu/error-report.h"       /* Error reporting */
#include "qapi/error.h"              /* QAPI error handling */
#include "hw/core/boards.h"          /* Board infrastructure */
#include "hw/core/loader.h"          /* Firmware/kernel loading */
#include "hw/core/sysbus.h"          /* System bus utilities */
#include "target/riscv/cpu.h"        /* RISC-V CPU definitions */
#include "hw/riscv/riscv_hart.h"     /* RISC-V HART array support */
#include "hw/riscv/vcore.h"          /* vcore board definitions */
#include "hw/riscv/boot.h"           /* RISC-V boot utilities */
#include "hw/char/serial.h"          /* Generic serial support */
#include "hw/char/serial-mm.h"       /* Memory-mapped serial (NS16550) */
#include "hw/char/xilinx_uartlite.h" /* Xilinx UART Lite support */
#include "hw/intc/riscv_aclint.h"    /* RISC-V ACLINT interrupt controller */
#include "chardev/char.h"            /* Character device support */
#include "system/device_tree.h"      /* Device tree generation */
#include "system/system.h"           /* System utilities */
#include "net/net.h"                 /* Network support */
#include "hw/misc/unimp.h"           /* Unimplemented device placeholder */

#include <libfdt.h>                  /* Flattened Device Tree library */

/**
 * vcore_memmap - Memory map definition for the vcore board
 *
 * This array defines the physical address layout of all memory regions
 * and peripherals on the vcore board. The indices correspond to the
 * enum values defined in vcore.h.
 */
static const MemMapEntry vcore_memmap[] = {
    [VCORE_MROM] =          {     0x1000,     0xf000 }, /* 60KB MROM */
    [VCORE_CLINT] =         {  0x2000000,    0x10000 }, /* 64KB CLINT */
    [VCORE_UART_NS16550] =  {  0x30009000,   0x1000 },  /* 4KB NS16550 UART */
    [VCORE_UART_UARTLITE] = {  0x40600000,   0x1000 },  /* 4KB Xilinx UART Lite */
    [VCORE_ETH_AXI] =       {  0x40c00000,  0x10000 },  /* 64KB AXI Ethernet */
    [VCORE_ETH_AXI_DMA] =   {  0x41e00000,  0x10000 },  /* 64KB AXI DMA */
    [VCORE_ETH_DWMAC] =     {  0x310a0000,  0x10000 },  /* 64KB DWMAC Ethernet */
    [VCORE_DRAM] =          { 0x80000000,        0x0 }, /* DRAM (size from -m) */
};

/**
 * Interrupt line assignments for peripherals
 *
 * These define the external interrupt lines used by each peripheral.
 * All peripherals connect to the RISC-V machine-mode external interrupt (IRQ_M_EXT).
 */
#define UART_NS16550_IRQ    1   /* NS16550 UART interrupt */
#define UART_UARTLITE_IRQ   2   /* Xilinx UART Lite interrupt */
#define ETH_AXI_IRQ         3   /* AXI Ethernet interrupt */
#define ETH_AXI_DMA_IRQ0    4   /* AXI DMA channel 0 interrupt */
#define ETH_AXI_DMA_IRQ1    5   /* AXI DMA channel 1 interrupt */
#define ETH_DWMAC_IRQ       6   /* DWMAC Ethernet interrupt */

/**
 * create_fdt() - Generate the Device Tree Blob (DTB) for the vcore board
 * @s: Pointer to VCoreState structure
 * @memmap: Pointer to memory map array
 * @is_32_bit: True if target is 32-bit RISC-V
 *
 * This function creates a complete device tree that describes the vcore
 * board hardware to the guest operating system. It includes:
 * - CPU information
 * - Memory regions
 * - Interrupt controllers
 * - Peripheral devices (UARTs)
 */
static void create_fdt(VCoreState *s, const MemMapEntry *memmap, bool is_32_bit)
{
    void *fdt;                      /* Device tree buffer */
    int fdt_size;                   /* Size of device tree buffer */
    uint64_t addr, size;            /* Address and size for DT entries */
    unsigned long clint_addr;       /* CLINT base address */
    int cpu;                        /* CPU index iterator */
    MachineState *ms = MACHINE(s);  /* Parent machine state */
    uint32_t *clint_cells;          /* CLINT interrupt cells */
    uint32_t cpu_phandle, intc_phandle, phandle = 1; /* DT phandles */
    char *mem_name, *clint_name, *uart_name; /* DT node names */
    char *cpu_name, *intc_name;     /* DT node names */
    /* CLINT compatible strings for device tree */
    static const char * const clint_compat[2] = {
        "sifive,clint0", "riscv,clint0"
    };

    /* Create empty device tree */
    fdt = ms->fdt = create_device_tree(&fdt_size);
    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(1);
    }

    /* Root node properties */
    qemu_fdt_setprop_string(fdt, "/", "model", "vcore,qemu");
    qemu_fdt_setprop_string(fdt, "/", "compatible", "vcore,vcore-dev");
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);

    /* SOC node - contains all peripherals */
    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop(fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 0x2);

    /* CPUs node - describes RISC-V HARTs */
    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "timebase-frequency",
        RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0x0);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 0x1);

    /* Allocate CLINT interrupt cells array */
    clint_cells = g_new0(uint32_t, s->soc[0].num_harts * 4);

    /* Add each CPU and its interrupt controller to DT */
    for (cpu = s->soc[0].num_harts - 1; cpu >= 0; cpu--) {
        cpu_phandle = phandle++;

        /* Create CPU node */
        cpu_name = g_strdup_printf("/cpus/cpu@%d", s->soc[0].hartid_base + cpu);
        qemu_fdt_add_subnode(fdt, cpu_name);
        
        /* Set MMU type based on 32/64-bit */
        if (is_32_bit) {
            qemu_fdt_setprop_string(fdt, cpu_name, "mmu-type", "riscv,sv32");
        } else {
            qemu_fdt_setprop_string(fdt, cpu_name, "mmu-type", "riscv,sv48");
        }
        
        /* Write ISA string to DT */
        riscv_isa_write_fdt(&s->soc[0].harts[cpu], fdt, cpu_name);
        
        /* Set CPU properties */
        qemu_fdt_setprop_string(fdt, cpu_name, "compatible", "riscv");
        qemu_fdt_setprop_string(fdt, cpu_name, "status", "okay");
        qemu_fdt_setprop_cell(fdt, cpu_name, "reg", s->soc[0].hartid_base + cpu);
        qemu_fdt_setprop_string(fdt, cpu_name, "device_type", "cpu");
        qemu_fdt_setprop_cell(fdt, cpu_name, "phandle", cpu_phandle);

        /* Create CPU interrupt controller node */
        intc_name = g_strdup_printf("%s/interrupt-controller", cpu_name);
        qemu_fdt_add_subnode(fdt, intc_name);
        intc_phandle = phandle++;
        qemu_fdt_setprop_cell(fdt, intc_name, "phandle", intc_phandle);
        qemu_fdt_setprop_string(fdt, intc_name, "compatible", "riscv,cpu-intc");
        qemu_fdt_setprop(fdt, intc_name, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, intc_name, "#interrupt-cells", 1);

        /* Populate CLINT interrupt cells */
        clint_cells[cpu * 4 + 0] = cpu_to_be32(intc_phandle);
        clint_cells[cpu * 4 + 1] = cpu_to_be32(IRQ_M_SOFT);
        clint_cells[cpu * 4 + 2] = cpu_to_be32(intc_phandle);
        clint_cells[cpu * 4 + 3] = cpu_to_be32(IRQ_M_TIMER);

        g_free(intc_name);
        g_free(cpu_name);
    }

    /* Add memory node */
    addr = memmap[VCORE_DRAM].base;
    size = ms->ram_size;
    mem_name = g_strdup_printf("/memory@%lx", (long)addr);
    qemu_fdt_add_subnode(fdt, mem_name);
    qemu_fdt_setprop_cells(fdt, mem_name, "reg",
        addr >> 32, addr, size >> 32, size);
    qemu_fdt_setprop_string(fdt, mem_name, "device_type", "memory");
    g_free(mem_name);

    /* Add CLINT node */
    clint_addr = memmap[VCORE_CLINT].base;
    clint_name = g_strdup_printf("/soc/clint@%lx", clint_addr);
    qemu_fdt_add_subnode(fdt, clint_name);
    qemu_fdt_setprop_string_array(fdt, clint_name, "compatible",
        (char **)&clint_compat, ARRAY_SIZE(clint_compat));
    qemu_fdt_setprop_cells(fdt, clint_name, "reg",
        0x0, clint_addr, 0x0, memmap[VCORE_CLINT].size);
    qemu_fdt_setprop(fdt, clint_name, "interrupts-extended",
        clint_cells, s->soc[0].num_harts * sizeof(uint32_t) * 4);
    g_free(clint_name);
    g_free(clint_cells);

    /* Add NS16550 UART node */
    uart_name = g_strdup_printf("/soc/uart@%lx", (long)memmap[VCORE_UART_NS16550].base);
    qemu_fdt_add_subnode(fdt, uart_name);
    qemu_fdt_setprop_string(fdt, uart_name, "compatible", "ns16550a");
    qemu_fdt_setprop_cells(fdt, uart_name, "reg",
        0x0, memmap[VCORE_UART_NS16550].base, 0x0, memmap[VCORE_UART_NS16550].size);
    qemu_fdt_setprop_cell(fdt, uart_name, "clock-frequency", 115200);
    qemu_fdt_setprop_string(fdt, uart_name, "status", "okay");
    g_free(uart_name);

    /* Add Xilinx UART Lite node */
    uart_name = g_strdup_printf("/soc/uartlite@%lx", (long)memmap[VCORE_UART_UARTLITE].base);
    qemu_fdt_add_subnode(fdt, uart_name);
    qemu_fdt_setprop_string(fdt, uart_name, "compatible", "xlnx,xps-uartlite-1.00.a");
    qemu_fdt_setprop_cells(fdt, uart_name, "reg",
        0x0, memmap[VCORE_UART_UARTLITE].base, 0x0, memmap[VCORE_UART_UARTLITE].size);
    qemu_fdt_setprop_cell(fdt, uart_name, "clock-frequency", 115200);
    qemu_fdt_setprop_string(fdt, uart_name, "status", "okay");
    g_free(uart_name);

    /* Add chosen node (specifies stdout path) */
    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", "/soc/uart@30009000");
}

/**
 * vcore_board_init() - Main initialization function for the vcore board
 * @machine: Pointer to MachineState structure
 *
 * This function is called during QEMU initialization to set up the vcore board.
 * It performs the following steps:
 * 1. Initialize CPU cores (RISC-V HARTs)
 * 2. Set up interrupt controllers (CLINT)
 * 3. Configure memory regions (RAM, ROM)
 * 4. Create peripheral devices (UARTs, Ethernet, DMA)
 * 5. Load firmware and prepare kernel boot
 * 6. Generate device tree
 */
static void vcore_board_init(MachineState *machine)
{
    const MemMapEntry *memmap = vcore_memmap;  /* Memory map reference */
    VCoreState *s = VCORE_MACHINE(machine);    /* vcore state cast */
    MemoryRegion *system_memory = get_system_memory(); /* System memory region */
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);  /* MROM region */
    hwaddr firmware_end_addr = memmap[VCORE_DRAM].base; /* Firmware end address */
    hwaddr firmware_load_addr = memmap[VCORE_DRAM].base; /* Firmware load address */
    vaddr kernel_start_addr;   /* Kernel start address */
    char *firmware_name;       /* Firmware filename */
    uint64_t fdt_load_addr;    /* FDT load address in memory */
    uint64_t kernel_entry;     /* Kernel entry point */
    int i, base_hartid = 0, hart_count = 1;  /* CPU configuration */
    RISCVBootInfo boot_info;   /* Boot information structure */
    DeviceState *dev, *eth0, *dma;  /* Device handles */
    Object *ds, *cs;           /* Stream interface objects */

    /* Initialize CPU sockets (currently single socket) */
    for (i = 0; i < VCORE_SOCKETS_MAX; i++) {
        hart_count = machine->smp.cpus;

        /* Create RISC-V HART array for this socket */
        object_initialize_child(OBJECT(machine), "soc", &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_fatal);

        /* Create ACLINT SWI (Software Interrupt) controller */
        riscv_aclint_swi_create(
            memmap[VCORE_CLINT].base + i * memmap[VCORE_CLINT].size,
            base_hartid, hart_count, false);
        
        /* Create ACLINT MTIMER (Timer) controller */
        riscv_aclint_mtimer_create(
            memmap[VCORE_CLINT].base + i * memmap[VCORE_CLINT].size +
                RISCV_ACLINT_SWI_SIZE,
            RISCV_ACLINT_DEFAULT_MTIMER_SIZE, base_hartid, hart_count,
            RISCV_ACLINT_DEFAULT_MTIMECMP, RISCV_ACLINT_DEFAULT_MTIME,
            RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ, false);
    }

    /* Map main RAM to system memory */
    memory_region_add_subregion(system_memory, memmap[VCORE_DRAM].base,
        machine->ram);

    /* Create and map MROM (mask ROM for boot firmware) */
    memory_region_init_rom(mask_rom, NULL, "riscv.vcore.mrom",
                           memmap[VCORE_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[VCORE_MROM].base,
                                mask_rom);

    /* Initialize NS16550 UART (primary serial port, connected to serial_hd(0)) */
    serial_mm_init(system_memory, memmap[VCORE_UART_NS16550].base, 0,
                   qdev_get_gpio_in(DEVICE(&s->soc[0].harts[0]), IRQ_M_EXT),
                   115200, serial_hd(0), DEVICE_LITTLE_ENDIAN);

    /* Initialize Xilinx UART Lite (secondary serial port, connected to serial_hd(1)) */
    dev = qdev_new(TYPE_XILINX_UARTLITE);
    qdev_prop_set_enum(dev, "endianness", ENDIAN_MODE_LITTLE);
    qdev_prop_set_chr(dev, "chardev", serial_hd(1));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, memmap[VCORE_UART_UARTLITE].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(DEVICE(&s->soc[0].harts[0]), IRQ_M_EXT));

    /* Initialize Xilinx AXI Ethernet controller */
    eth0 = qdev_new("xlnx.axi-ethernet");
    dma = qdev_new("xlnx.axi-dma");

    /* Add devices to machine object hierarchy */
    object_property_add_child(qdev_get_machine(), "xilinx-eth", OBJECT(eth0));
    object_property_add_child(qdev_get_machine(), "xilinx-dma", OBJECT(dma));

    /* Get stream interfaces from DMA for Ethernet connection */
    ds = object_property_get_link(OBJECT(dma),
                                  "axistream-connected-target", NULL);
    cs = object_property_get_link(OBJECT(dma),
                                  "axistream-control-connected-target", NULL);
    
    /* Configure Ethernet device */
    qemu_configure_nic_device(eth0, true, NULL);
    qdev_prop_set_uint32(eth0, "rxmem", 0x1000);
    qdev_prop_set_uint32(eth0, "txmem", 0x1000);
    object_property_set_link(OBJECT(eth0), "axistream-connected", ds, &error_abort);
    object_property_set_link(OBJECT(eth0), "axistream-control-connected", cs, &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(eth0), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(eth0), 0, memmap[VCORE_ETH_AXI].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(eth0), 0,
                       qdev_get_gpio_in(DEVICE(&s->soc[0].harts[0]), IRQ_M_EXT));

    /* Get stream interfaces from Ethernet for DMA connection */
    ds = object_property_get_link(OBJECT(eth0),
                                  "axistream-connected-target", NULL);
    cs = object_property_get_link(OBJECT(eth0),
                                  "axistream-control-connected-target", NULL);
    
    /* Configure DMA device */
    qdev_prop_set_uint32(dma, "freqhz", 100000000);
    object_property_set_link(OBJECT(dma), "axistream-connected", ds, &error_abort);
    object_property_set_link(OBJECT(dma), "axistream-control-connected", cs, &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dma), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dma), 0, memmap[VCORE_ETH_AXI_DMA].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dma), 0,
                       qdev_get_gpio_in(DEVICE(&s->soc[0].harts[0]), IRQ_M_EXT));
    sysbus_connect_irq(SYS_BUS_DEVICE(dma), 1,
                       qdev_get_gpio_in(DEVICE(&s->soc[0].harts[0]), IRQ_M_EXT));

    /* Create placeholder for Synopsys DWMAC Ethernet (not yet implemented in QEMU) */
    create_unimplemented_device("dwmac", memmap[VCORE_ETH_DWMAC].base,
                                memmap[VCORE_ETH_DWMAC].size);

    /* Find and load firmware if specified */
    firmware_name = riscv_find_firmware(machine->firmware,
                        riscv_default_firmware_name(&s->soc[0]));

    if (firmware_name) {
        firmware_end_addr = riscv_load_firmware(firmware_name,
                                                &firmware_load_addr,
                                                NULL);
        g_free(firmware_name);
    }

    /* Generate device tree */
    create_fdt(s, memmap, riscv_is_32bit(&s->soc[0]));

    /* Prepare boot information for kernel loading */
    riscv_boot_info_init(&boot_info, &s->soc[0]);
    if (machine->kernel_filename) {
        kernel_start_addr = riscv_calc_kernel_start_addr(&boot_info, firmware_end_addr);

        /* Load kernel into memory */
        riscv_load_kernel(machine, &boot_info, kernel_start_addr, true, NULL);
        kernel_entry = boot_info.image_low_addr;
    } else {
        kernel_entry = 0;
    }

    /* Calculate FDT load address */
    fdt_load_addr = riscv_compute_fdt_addr(memmap[VCORE_DRAM].base,
                                           machine->ram_size,
                                           machine, &boot_info);
    riscv_load_fdt(fdt_load_addr, machine->fdt);

    /* Set up reset vector for ROM */
    riscv_setup_rom_reset_vec(machine, &s->soc[0], firmware_load_addr,
                              memmap[VCORE_MROM].base,
                              memmap[VCORE_MROM].size, kernel_entry,
                              fdt_load_addr);
}

/**
 * vcore_machine_instance_init() - Instance initialization callback
 * @obj: Pointer to Object instance
 *
 * This function is called when a new vcore machine instance is created.
 * Currently no instance-specific initialization is needed.
 */
static void vcore_machine_instance_init(Object *obj)
{
}

/**
 * vcore_machine_class_init() - Class initialization callback
 * @oc: Pointer to ObjectClass
 * @data: Pointer to class data
 *
 * This function initializes the vcore machine class with default values,
 * including the board description, initialization function, and default
 * CPU configuration.
 */
static void vcore_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V vcore board";       /* Board description */
    mc->init = vcore_board_init;           /* Board init function */
    mc->max_cpus = VCORE_CPUS_MAX;         /* Maximum CPUs supported */
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;  /* Default CPU type */
    mc->default_ram_id = "riscv.vcore.ram"; /* RAM region ID */
}

/**
 * vcore_machine_typeinfo - Type information for the vcore machine
 *
 * This structure registers the vcore machine type with QEMU's object system.
 */
static const TypeInfo vcore_machine_typeinfo = {
    .name = MACHINE_TYPE_NAME("vcore"),        /* Type name */
    .parent = TYPE_MACHINE,                     /* Parent type */
    .class_init = vcore_machine_class_init,     /* Class init callback */
    .instance_init = vcore_machine_instance_init, /* Instance init callback */
    .instance_size = sizeof(VCoreState),        /* Instance size */
};

/**
 * vcore_machine_init_register_types() - Register vcore machine type
 *
 * This function is called during QEMU initialization to register the
 * vcore machine type with QEMU's type system.
 */
static void vcore_machine_init_register_types(void)
{
    type_register_static(&vcore_machine_typeinfo);
}

/* Register the vcore machine type at initialization time */
type_init(vcore_machine_init_register_types)
