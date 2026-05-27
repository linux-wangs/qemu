/*
 * vcore开发板 - CPU相关代码片段（中文详细注释版）
 */

#include "hw/riscv/vcore.h"

/*
 * ==========================================================================
 * 第一部分：头文件中的CPU相关定义
 * ==========================================================================
 */

// vcore.h中的定义

/* 最大支持的CPU数量（HART）：8个 */
#define VCORE_CPUS_MAX 8

/* 最大支持的Socket数量：1个 */
#define VCORE_SOCKETS_MAX 1

/* vcore机器类型定义 */
#define TYPE_VCORE_MACHINE MACHINE_TYPE_NAME("vcore")

/* vcore板主状态结构 */
struct VCoreState {
    /* 继承自QEMU通用的MachineState */
    MachineState parent;
    
    /* RISC-V HART数组（每个HART相当于一个CPU核心） */
    RISCVHartArrayState soc[VCORE_SOCKETS_MAX];
};

/* 内存映射枚举（包含CPU相关区域） */
enum {
    VCORE_MROM = 0,           /* Mask ROM（启动固件） */
    VCORE_CLINT,              /* CLINT（CPU本地中断控制器） */
    VCORE_UART_NS16550,       /* NS16550 UART */
    VCORE_UART_UARTLITE,      /* Xilinx UART Lite */
    VCORE_ETH_AXI,            /* AXI Ethernet */
    VCORE_ETH_AXI_DMA,        /* AXI DMA */
    VCORE_ETH_DWMAC,          /* DWMAC Ethernet */
    VCORE_DRAM,               /* 主内存 */
};

/* 内存映射表 */
static const MemMapEntry vcore_memmap[] = {
    [VCORE_MROM] = {     0x1000,     0xf000 }, /* MROM：0x1000-0xFFFF，60KB */
    [VCORE_CLINT] = {  0x2000000,    0x10000 }, /* CLINT：0x2000000-0x200FFFF，64KB */
    [VCORE_UART_NS16550] = {  0x30009000,   0x1000 }, /* UART */
    [VCORE_UART_UARTLITE] = {  0x40600000,   0x1000 },
    [VCORE_ETH_AXI] = {  0x40c00000,  0x10000 },
    [VCORE_ETH_AXI_DMA] = {  0x41e00000,  0x10000 },
    [VCORE_ETH_DWMAC] = {  0x310a0000,  0x10000 },
    [VCORE_DRAM] = { 0x80000000,        0x0 }, /* DRAM：0x80000000+，大小可变 */
};

/*
 * ==========================================================================
 * 第二部分：vcore板初始化函数中的CPU相关部分
 * ==========================================================================
 */

static void vcore_board_init(MachineState *machine)
{
    const MemMapEntry *memmap = vcore_memmap;  /* 获取内存映射表 */
    VCoreState *s = VCORE_MACHINE(machine);    /* 将通用机器状态转换为vcore状态 */
    MemoryRegion *system_memory = get_system_memory(); /* 获取系统内存区域 */
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1); /* 创建MROM内存区域 */
    hwaddr firmware_end_addr = memmap[VCORE_DRAM].base; /* 固件结束地址 */
    hwaddr firmware_load_addr = memmap[VCORE_DRAM].base; /* 固件加载地址 */
    vaddr kernel_start_addr;   /* 内核启动地址 */
    char *firmware_name;       /* 固件文件名 */
    uint64_t fdt_load_addr;    /* 设备树FDT加载地址 */
    uint64_t kernel_entry;     /* 内核入口点 */
    
    /* 
     * CPU配置参数
     * i：socket索引
     * base_hartid：第一个HART的ID
     * hart_count：HART数量（CPU数量）
     */
    int i, base_hartid = 0, hart_count = 1;
    RISCVBootInfo boot_info;   /* RISC-V启动信息结构 */
    DeviceState *dev, *eth0, *dma;  /* 设备指针 */
    Object *ds, *cs;           /* AXI流接口对象 */
    
    /*
     * 遍历所有CPU Socket并初始化（目前只支持1个Socket）
     */
    for (i = 0; i < VCORE_SOCKETS_MAX; i++) {
        
        /* 从命令行参数或配置获取CPU核心数量 */
        hart_count = machine->smp.cpus;
        
        /*
         * 初始化RISC-V HART数组对象
         * 参数说明：
         * OBJECT(machine)：父对象
         * "soc"：对象名称
         * &s->soc[i]：对象存储位置
         * TYPE_RISCV_HART_ARRAY：对象类型（RISC-V HART数组）
         */
        object_initialize_child(OBJECT(machine), "soc", &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);
        
        /*
         * 设置HART数组的属性：CPU类型
         * 例如："rv64gc"、"rv64imafdc"等
         */
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        
        /*
         * 设置第一个HART的ID起始值
         * 通常从0开始，多核系统中会递增
         */
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        
        /*
         * 设置HART的数量（CPU核心数量）
         */
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        
        /*
         * 实现（Realize）HART数组
         * 这会创建所有的CPU对象并配置它们
         */
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_fatal);
        
        /*
         * =================================================================
         * 创建CLINT（Core Local Interruptor - 核心本地中断控制器）
         * =================================================================
         *
         * CLINT包含两个主要组件：
         * 1. MSWI：机器模式软件中断（用于核间通信）
         * 2. MTIMER：机器模式定时器（用于计时和定时器中断）
         */
        
        /*
         * 创建ACLINT SWI（软件中断）控制器
         * 
         * 参数：
         * 1. memmap[VCORE_CLINT].base + ...：SWI寄存器基地址
         * 2. base_hartid：第一个HART的ID
         * 3. hart_count：HART数量
         * 4. false：是否有ACLINT MSWI（高级MSWI）
         */
        riscv_aclint_swi_create(
            memmap[VCORE_CLINT].base + i * memmap[VCORE_CLINT].size,
            base_hartid,
            hart_count,
            false);
        
        /*
         * 创建ACLINT MTIMER（定时器）控制器
         *
         * 参数：
         * 1. 基地址（SWI之后）
         * 2. MTIMER区域大小
         * 3. 第一个HART的ID
         * 4. HART数量
         * 5. MTIMECMP寄存器地址（默认）
         * 6. MTIME寄存器地址（默认）
         * 7. 定时器频率（10MHz）
         * 8. false：是否有ACLINT MTimer
         */
        riscv_aclint_mtimer_create(
            memmap[VCORE_CLINT].base + i * memmap[VCORE_CLINT].size 
                + RISCV_ACLINT_SWI_SIZE,
            RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
            base_hartid,
            hart_count,
            RISCV_ACLINT_DEFAULT_MTIMECMP,
            RISCV_ACLINT_DEFAULT_MTIME,
            RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ,
            false);
    }
    
    /* ====================================================================
     * 下面是内存和设备初始化（与CPU间接相关）
     * ==================================================================== */
    
    /* 映射主RAM到系统内存 */
    memory_region_add_subregion(system_memory, memmap[VCORE_DRAM].base,
        machine->ram);
    
    /* 创建Mask ROM（启动固件区域） */
    memory_region_init_rom(mask_rom, NULL, "riscv.vcore.mrom",
                           memmap[VCORE_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[VCORE_MROM].base,
                                mask_rom);
    
    /* ====================================================================
     * 串口和设备初始化（略，见完整代码）
     * ==================================================================== */
}

/*
 * ==========================================================================
 * 第三部分：设备树中CPU节点的创建
 * ==========================================================================
 */

static void create_fdt(VCoreState *s, const MemMapEntry *memmap, bool is_32_bit)
{
    void *fdt;                           /* 设备树缓冲区 */
    int fdt_size;                        /* 设备树大小 */
    uint64_t addr, size;                 /* 地址和大小 */
    unsigned long clint_addr;            /* CLINT地址 */
    int cpu;                             /* CPU循环变量 */
    MachineState *ms = MACHINE(s);       /* 机器状态 */
    uint32_t *clint_cells;               /* CLINT中断单元格 */
    uint32_t cpu_phandle, intc_phandle;  /* 设备树句柄 */
    uint32_t phandle = 1;                /* 句柄计数器 */
    char *mem_name, *clint_name, *uart_name; /* 设备节点名 */
    char *cpu_name, *intc_name;          /* CPU和中断控制器名 */
    
    /* CLINT兼容字符串 */
    static const char * const clint_compat[2] = {
        "sifive,clint0", "riscv,clint0"
    };
    
    /* 创建空的设备树 */
    fdt = ms->fdt = create_device_tree(&fdt_size);
    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(1);
    }
    
    /* 设置根节点属性 */
    qemu_fdt_setprop_string(fdt, "/", "model", "vcore,qemu");
    qemu_fdt_setprop_string(fdt, "/", "compatible", "vcore,vcore-dev");
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);
    
    /* 创建SOC节点 */
    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop(fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 0x2);
    
    /*
     * 创建CPUs节点
     */
    qemu_fdt_add_subnode(fdt, "/cpus");
    
    /* 设置时基频率（CLINT定时器频率） */
    qemu_fdt_setprop_cell(fdt, "/cpus", "timebase-frequency",
        RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ);
    
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0x0);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 0x1);
    
    /* 分配CLINT中断单元格数组 */
    clint_cells = g_new0(uint32_t, s->soc[0].num_harts * 4);
    
    /*
     * 循环添加每个CPU节点
     * 注意：倒序添加（从最后一个CPU到第一个）
     */
    for (cpu = s->soc[0].num_harts - 1; cpu >= 0; cpu--) {
        
        /* 分配一个新的phandle给CPU */
        cpu_phandle = phandle++;
        
        /* 创建CPU节点名称，格式：/cpus/cpu@<hartid> */
        cpu_name = g_strdup_printf("/cpus/cpu@%d",
            s->soc[0].hartid_base + cpu);
        
        /* 添加CPU节点到设备树 */
        qemu_fdt_add_subnode(fdt, cpu_name);
        
        /*
         * 根据系统是32位还是64位，设置MMU类型
         * - rv32使用sv32
         * - rv64使用sv48
         */
        if (is_32_bit) {
            qemu_fdt_setprop_string(fdt, cpu_name, "mmu-type", "riscv,sv32");
        } else {
            qemu_fdt_setprop_string(fdt, cpu_name, "mmu-type", "riscv,sv48");
        }
        
        /*
         * 写入RISC-V ISA字符串到设备树
         * 例如："rv64imafdc"
         */
        riscv_isa_write_fdt(&s->soc[0].harts[cpu], fdt, cpu_name);
        
        /* 设置CPU兼容性字符串 */
        qemu_fdt_setprop_string(fdt, cpu_name, "compatible", "riscv");
        
        /* 设置CPU状态为"okay"（启用） */
        qemu_fdt_setprop_string(fdt, cpu_name, "status", "okay");
        
        /* 设置CPU寄存器（HART ID） */
        qemu_fdt_setprop_cell(fdt, cpu_name, "reg",
            s->soc[0].hartid_base + cpu);
        
        /* 设置设备类型为"cpu" */
        qemu_fdt_setprop_string(fdt, cpu_name, "device_type", "cpu");
        
        /* 设置CPU的phandle */
        qemu_fdt_setprop_cell(fdt, cpu_name, "phandle", cpu_phandle);
        
        /*
         * 创建CPU中断控制器子节点
         * 每个RISC-V CPU都有自己的中断控制器
         */
        intc_name = g_strdup_printf("%s/interrupt-controller", cpu_name);
        qemu_fdt_add_subnode(fdt, intc_name);
        
        /* 分配phandle给中断控制器 */
        intc_phandle = phandle++;
        qemu_fdt_setprop_cell(fdt, intc_name, "phandle", intc_phandle);
        
        /* 设置中断控制器兼容性字符串 */
        qemu_fdt_setprop_string(fdt, intc_name, "compatible",
            "riscv,cpu-intc");
        
        /* 标记这是一个中断控制器 */
        qemu_fdt_setprop(fdt, intc_name, "interrupt-controller", NULL, 0);
        
        /* 设置中断单元格大小为1 */
        qemu_fdt_setprop_cell(fdt, intc_name, "#interrupt-cells", 1);
        
        /*
         * 填充CLINT中断单元格
         * 每个HART有4个条目：
         * [0]：intc_phandle（中断控制器句柄）
         * [1]：IRQ_M_SOFT（机器模式软件中断）
         * [2]：intc_phandle
         * [3]：IRQ_M_TIMER（机器模式定时器中断）
         */
        clint_cells[cpu * 4 + 0] = cpu_to_be32(intc_phandle);
        clint_cells[cpu * 4 + 1] = cpu_to_be32(IRQ_M_SOFT);
        clint_cells[cpu * 4 + 2] = cpu_to_be32(intc_phandle);
        clint_cells[cpu * 4 + 3] = cpu_to_be32(IRQ_M_TIMER);
        
        /* 释放临时字符串 */
        g_free(intc_name);
        g_free(cpu_name);
    }
    
    /*
     * 添加内存节点到设备树
     */
    addr = memmap[VCORE_DRAM].base;
    size = ms->ram_size;
    mem_name = g_strdup_printf("/memory@%lx", (long)addr);
    qemu_fdt_add_subnode(fdt, mem_name);
    qemu_fdt_setprop_cells(fdt, mem_name, "reg",
        addr >> 32, addr, size >> 32, size);
    qemu_fdt_setprop_string(fdt, mem_name, "device_type", "memory");
    g_free(mem_name);
    
    /*
     * 添加CLINT节点到设备树
     */
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
    
    /*
     * 添加串口和其他外设节点（略）
     */
}

/*
 * ==========================================================================
 * 第四部分：启动相关的CPU设置
 * ==========================================================================
 */

static void setup_boot(MachineState *machine, VCoreState *s,
                       const MemMapEntry *memmap)
{
    hwaddr firmware_end_addr = memmap[VCORE_DRAM].base;
    hwaddr firmware_load_addr = memmap[VCORE_DRAM].base;
    vaddr kernel_start_addr;
    char *firmware_name;
    uint64_t fdt_load_addr;
    uint64_t kernel_entry;
    RISCVBootInfo boot_info;
    
    /* 查找固件 */
    firmware_name = riscv_find_firmware(machine->firmware,
                        riscv_default_firmware_name(&s->soc[0]));
    
    /* 加载固件 */
    if (firmware_name) {
        firmware_end_addr = riscv_load_firmware(firmware_name,
                                                &firmware_load_addr,
                                                NULL);
        g_free(firmware_name);
    }
    
    /* 初始化启动信息 */
    riscv_boot_info_init(&boot_info, &s->soc[0]);
    
    /* 加载内核 */
    if (machine->kernel_filename) {
        kernel_start_addr = riscv_calc_kernel_start_addr(&boot_info,
                                                         firmware_end_addr);
        riscv_load_kernel(machine, &boot_info, kernel_start_addr,
                          true, NULL);
        kernel_entry = boot_info.image_low_addr;
    } else {
        kernel_entry = 0;
    }
    
    /* 计算并加载FDT（设备树） */
    fdt_load_addr = riscv_compute_fdt_addr(memmap[VCORE_DRAM].base,
                                           machine->ram_size,
                                           machine, &boot_info);
    riscv_load_fdt(fdt_load_addr, machine->fdt);
    
    /*
     * 设置ROM复位向量
     * 配置CPU的复位地址和启动参数
     */
    riscv_setup_rom_reset_vec(machine, &s->soc[0], firmware_load_addr,
                              memmap[VCORE_MROM].base,
                              memmap[VCORE_MROM].size, kernel_entry,
                              fdt_load_addr);
}

/*
 * ==========================================================================
 * 第五部分：vcore机器类型注册
 * ==========================================================================
 */

/* vcore机器实例初始化回调 */
static void vcore_machine_instance_init(Object *obj)
{
    /* 目前不需要特定的实例初始化 */
}

/* vcore机器类初始化回调 */
static void vcore_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    
    /* 设置机器描述 */
    mc->desc = "RISC-V vcore board";
    
    /* 设置板初始化函数 */
    mc->init = vcore_board_init;
    
    /* 设置最大CPU数量 */
    mc->max_cpus = VCORE_CPUS_MAX;
    
    /* 设置默认CPU类型 */
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    
    /* 设置默认RAM ID */
    mc->default_ram_id = "riscv.vcore.ram";
}

/* vcore机器类型信息 */
static const TypeInfo vcore_machine_typeinfo = {
    .name = MACHINE_TYPE_NAME("vcore"),
    .parent = TYPE_MACHINE,
    .class_init = vcore_machine_class_init,
    .instance_init = vcore_machine_instance_init,
    .instance_size = sizeof(VCoreState),
};

/* 注册vcore机器类型 */
static void vcore_machine_init_register_types(void)
{
    type_register_static(&vcore_machine_typeinfo);
}

/* 在QEMU初始化时注册类型 */
type_init(vcore_machine_init_register_types)
