#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/vcore.h"
#include "hw/riscv/boot.h"
#include "hw/char/serial.h"
#include "hw/char/serial-mm.h"
#include "hw/char/xilinx_uartlite.h"
#include "hw/intc/riscv_aclint.h"
#include "chardev/char.h"
#include "system/device_tree.h"
#include "system/system.h"
#include "net/net.h"
#include "hw/misc/unimp.h"

#include <libfdt.h>

static const MemMapEntry vcore_memmap[] = {
    [VCORE_MROM] =          {     0x1000,     0xf000 },
    [VCORE_CLINT] =         {  0x2000000,    0x10000 },
    [VCORE_UART_NS16550] =  {  0x30009000,   0x1000 },
    [VCORE_UART_UARTLITE] = {  0x40600000,   0x1000 },
    [VCORE_ETH_AXI] =       {  0x40c00000,  0x10000 },
    [VCORE_ETH_AXI_DMA] =   {  0x41e00000,  0x10000 },
    [VCORE_ETH_DWMAC] =     {  0x310a0000,  0x10000 },
    [VCORE_DRAM] =          { 0x80000000,        0x0 },
};

#define UART_NS16550_IRQ 1
#define UART_UARTLITE_IRQ 2
#define ETH_AXI_IRQ 3
#define ETH_AXI_DMA_IRQ0 4
#define ETH_AXI_DMA_IRQ1 5
#define ETH_DWMAC_IRQ 6

static void create_fdt(VCoreState *s, const MemMapEntry *memmap, bool is_32_bit)
{
    void *fdt;
    int fdt_size;
    uint64_t addr, size;
    unsigned long clint_addr;
    int cpu;
    MachineState *ms = MACHINE(s);
    uint32_t *clint_cells;
    uint32_t cpu_phandle, intc_phandle, phandle = 1;
    char *mem_name, *clint_name, *uart_name;
    char *cpu_name, *intc_name;
    static const char * const clint_compat[2] = {
        "sifive,clint0", "riscv,clint0"
    };

    fdt = ms->fdt = create_device_tree(&fdt_size);
    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(1);
    }

    qemu_fdt_setprop_string(fdt, "/", "model", "vcore,qemu");
    qemu_fdt_setprop_string(fdt, "/", "compatible", "vcore,vcore-dev");
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);

    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop(fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 0x2);

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "timebase-frequency",
        RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0x0);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 0x1);

    clint_cells = g_new0(uint32_t, s->soc[0].num_harts * 4);

    for (cpu = s->soc[0].num_harts - 1; cpu >= 0; cpu--) {
        cpu_phandle = phandle++;

        cpu_name = g_strdup_printf("/cpus/cpu@%d",
            s->soc[0].hartid_base + cpu);
        qemu_fdt_add_subnode(fdt, cpu_name);
        if (is_32_bit) {
            qemu_fdt_setprop_string(fdt, cpu_name, "mmu-type", "riscv,sv32");
        } else {
            qemu_fdt_setprop_string(fdt, cpu_name, "mmu-type", "riscv,sv48");
        }
        riscv_isa_write_fdt(&s->soc[0].harts[cpu], fdt, cpu_name);
        qemu_fdt_setprop_string(fdt, cpu_name, "compatible", "riscv");
        qemu_fdt_setprop_string(fdt, cpu_name, "status", "okay");
        qemu_fdt_setprop_cell(fdt, cpu_name, "reg",
            s->soc[0].hartid_base + cpu);
        qemu_fdt_setprop_string(fdt, cpu_name, "device_type", "cpu");
        qemu_fdt_setprop_cell(fdt, cpu_name, "phandle", cpu_phandle);

        intc_name = g_strdup_printf("%s/interrupt-controller", cpu_name);
        qemu_fdt_add_subnode(fdt, intc_name);
        intc_phandle = phandle++;
        qemu_fdt_setprop_cell(fdt, intc_name, "phandle", intc_phandle);
        qemu_fdt_setprop_string(fdt, intc_name, "compatible",
            "riscv,cpu-intc");
        qemu_fdt_setprop(fdt, intc_name, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, intc_name, "#interrupt-cells", 1);

        clint_cells[cpu * 4 + 0] = cpu_to_be32(intc_phandle);
        clint_cells[cpu * 4 + 1] = cpu_to_be32(IRQ_M_SOFT);
        clint_cells[cpu * 4 + 2] = cpu_to_be32(intc_phandle);
        clint_cells[cpu * 4 + 3] = cpu_to_be32(IRQ_M_TIMER);

        g_free(intc_name);
        g_free(cpu_name);
    }

    addr = memmap[VCORE_DRAM].base;
    size = ms->ram_size;
    mem_name = g_strdup_printf("/memory@%lx", (long)addr);
    qemu_fdt_add_subnode(fdt, mem_name);
    qemu_fdt_setprop_cells(fdt, mem_name, "reg",
        addr >> 32, addr, size >> 32, size);
    qemu_fdt_setprop_string(fdt, mem_name, "device_type", "memory");
    g_free(mem_name);

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

    uart_name = g_strdup_printf("/soc/uart@%lx", (long)memmap[VCORE_UART_NS16550].base);
    qemu_fdt_add_subnode(fdt, uart_name);
    qemu_fdt_setprop_string(fdt, uart_name, "compatible", "ns16550a");
    qemu_fdt_setprop_cells(fdt, uart_name, "reg",
        0x0, memmap[VCORE_UART_NS16550].base, 0x0, memmap[VCORE_UART_NS16550].size);
    qemu_fdt_setprop_cell(fdt, uart_name, "clock-frequency", 115200);
    qemu_fdt_setprop_string(fdt, uart_name, "status", "okay");
    g_free(uart_name);

    uart_name = g_strdup_printf("/soc/uartlite@%lx", (long)memmap[VCORE_UART_UARTLITE].base);
    qemu_fdt_add_subnode(fdt, uart_name);
    qemu_fdt_setprop_string(fdt, uart_name, "compatible", "xlnx,xps-uartlite-1.00.a");
    qemu_fdt_setprop_cells(fdt, uart_name, "reg",
        0x0, memmap[VCORE_UART_UARTLITE].base, 0x0, memmap[VCORE_UART_UARTLITE].size);
    qemu_fdt_setprop_cell(fdt, uart_name, "clock-frequency", 115200);
    qemu_fdt_setprop_string(fdt, uart_name, "status", "okay");
    g_free(uart_name);

    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", "/soc/uart@30009000");
}

static void vcore_board_init(MachineState *machine)
{
    const MemMapEntry *memmap = vcore_memmap;
    VCoreState *s = VCORE_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);
    hwaddr firmware_end_addr = memmap[VCORE_DRAM].base;
    hwaddr firmware_load_addr = memmap[VCORE_DRAM].base;
    vaddr kernel_start_addr;
    char *firmware_name;
    uint64_t fdt_load_addr;
    uint64_t kernel_entry;
    int i, base_hartid = 0, hart_count = 1;
    RISCVBootInfo boot_info;
    DeviceState *dev, *eth0, *dma;
    Object *ds, *cs;

    for (i = 0; i < VCORE_SOCKETS_MAX; i++) {
        hart_count = machine->smp.cpus;

        object_initialize_child(OBJECT(machine), "soc", &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_fatal);

        riscv_aclint_swi_create(
            memmap[VCORE_CLINT].base + i * memmap[VCORE_CLINT].size,
            base_hartid, hart_count, false);
        riscv_aclint_mtimer_create(
            memmap[VCORE_CLINT].base + i * memmap[VCORE_CLINT].size +
                RISCV_ACLINT_SWI_SIZE,
            RISCV_ACLINT_DEFAULT_MTIMER_SIZE, base_hartid, hart_count,
            RISCV_ACLINT_DEFAULT_MTIMECMP, RISCV_ACLINT_DEFAULT_MTIME,
            RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ, false);
    }

    memory_region_add_subregion(system_memory, memmap[VCORE_DRAM].base,
        machine->ram);

    memory_region_init_rom(mask_rom, NULL, "riscv.vcore.mrom",
                           memmap[VCORE_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[VCORE_MROM].base,
                                mask_rom);

    serial_mm_init(system_memory, memmap[VCORE_UART_NS16550].base, 0,
                   qdev_get_gpio_in(DEVICE(&s->soc[0].harts[0]),
                                    IRQ_M_EXT),
                   115200, serial_hd(0), DEVICE_LITTLE_ENDIAN);

    dev = qdev_new(TYPE_XILINX_UARTLITE);
    qdev_prop_set_enum(dev, "endianness", ENDIAN_MODE_LITTLE);
    qdev_prop_set_chr(dev, "chardev", serial_hd(1));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, memmap[VCORE_UART_UARTLITE].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in(DEVICE(&s->soc[0].harts[0]),
                                        IRQ_M_EXT));

    eth0 = qdev_new("xlnx.axi-ethernet");
    dma = qdev_new("xlnx.axi-dma");

    object_property_add_child(qdev_get_machine(), "xilinx-eth", OBJECT(eth0));
    object_property_add_child(qdev_get_machine(), "xilinx-dma", OBJECT(dma));

    ds = object_property_get_link(OBJECT(dma),
                                  "axistream-connected-target", NULL);
    cs = object_property_get_link(OBJECT(dma),
                                  "axistream-control-connected-target", NULL);
    qemu_configure_nic_device(eth0, true, NULL);
    qdev_prop_set_uint32(eth0, "rxmem", 0x1000);
    qdev_prop_set_uint32(eth0, "txmem", 0x1000);
    object_property_set_link(OBJECT(eth0), "axistream-connected", ds,
                             &error_abort);
    object_property_set_link(OBJECT(eth0), "axistream-control-connected", cs,
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(eth0), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(eth0), 0, memmap[VCORE_ETH_AXI].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(eth0), 0,
                       qdev_get_gpio_in(DEVICE(&s->soc[0].harts[0]),
                                        IRQ_M_EXT));

    ds = object_property_get_link(OBJECT(eth0),
                                  "axistream-connected-target", NULL);
    cs = object_property_get_link(OBJECT(eth0),
                                  "axistream-control-connected-target", NULL);
    qdev_prop_set_uint32(dma, "freqhz", 100000000);
    object_property_set_link(OBJECT(dma), "axistream-connected", ds,
                             &error_abort);
    object_property_set_link(OBJECT(dma), "axistream-control-connected", cs,
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dma), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dma), 0, memmap[VCORE_ETH_AXI_DMA].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(dma), 0,
                       qdev_get_gpio_in(DEVICE(&s->soc[0].harts[0]),
                                        IRQ_M_EXT));
    sysbus_connect_irq(SYS_BUS_DEVICE(dma), 1,
                       qdev_get_gpio_in(DEVICE(&s->soc[0].harts[0]),
                                        IRQ_M_EXT));

    create_unimplemented_device("dwmac", memmap[VCORE_ETH_DWMAC].base,
                                memmap[VCORE_ETH_DWMAC].size);

    firmware_name = riscv_find_firmware(machine->firmware,
                        riscv_default_firmware_name(&s->soc[0]));

    if (firmware_name) {
        firmware_end_addr = riscv_load_firmware(firmware_name,
                                                &firmware_load_addr,
                                                NULL);
        g_free(firmware_name);
    }

    create_fdt(s, memmap, riscv_is_32bit(&s->soc[0]));

    riscv_boot_info_init(&boot_info, &s->soc[0]);
    if (machine->kernel_filename) {
        kernel_start_addr = riscv_calc_kernel_start_addr(&boot_info,
                                                         firmware_end_addr);

        riscv_load_kernel(machine, &boot_info, kernel_start_addr,
                          true, NULL);
        kernel_entry = boot_info.image_low_addr;
    } else {
        kernel_entry = 0;
    }

    fdt_load_addr = riscv_compute_fdt_addr(memmap[VCORE_DRAM].base,
                                           machine->ram_size,
                                           machine, &boot_info);
    riscv_load_fdt(fdt_load_addr, machine->fdt);

    riscv_setup_rom_reset_vec(machine, &s->soc[0], firmware_load_addr,
                              memmap[VCORE_MROM].base,
                              memmap[VCORE_MROM].size, kernel_entry,
                              fdt_load_addr);
}

static void vcore_machine_instance_init(Object *obj)
{
}

static void vcore_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V vcore board";
    mc->init = vcore_board_init;
    mc->max_cpus = VCORE_CPUS_MAX;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->default_ram_id = "riscv.vcore.ram";
}

static const TypeInfo vcore_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("vcore"),
    .parent     = TYPE_MACHINE,
    .class_init = vcore_machine_class_init,
    .instance_init = vcore_machine_instance_init,
    .instance_size = sizeof(VCoreState),
};

static void vcore_machine_init_register_types(void)
{
    type_register_static(&vcore_machine_typeinfo);
}

type_init(vcore_machine_init_register_types)