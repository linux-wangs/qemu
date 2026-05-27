/*
 * vcore开发板 - 内存相关代码片段（中文详细注释版）
 */

#include "hw/riscv/vcore.h"
#include "hw/core/loader.h"
#include "system/system.h"
#include "system/device_tree.h"
#include "hw/core/sysbus.h"

/*
 * ==========================================================================
 * 第一部分：内存映射定义（来自vcore.h和vcore.c）
 * ==========================================================================
 */

/*
 * 内存区域枚举定义
 * 每个枚举值对应一个内存区域
 */
enum {
    VCORE_MROM = 0,           /* Mask ROM（启动固件区域） */
    VCORE_CLINT,              /* CLINT（CPU本地中断控制器） */
    VCORE_UART_NS16550,       /* NS16550 UART串口 */
    VCORE_UART_UARTLITE,      /* Xilinx UART Lite串口 */
    VCORE_ETH_AXI,            /* Xilinx AXI以太网控制器 */
    VCORE_ETH_AXI_DMA,        /* Xilinx AXI DMA控制器 */
    VCORE_ETH_DWMAC,          /* Synopsys DWMAC以太网控制器 */
    VCORE_DRAM,               /* 主系统内存（DRAM） */
};

/*
 * 内存映射表（MemMapEntry数组）
 * 
 * 每个条目包含：
 * - base: 物理基地址
 * - size: 区域大小（0表示大小可变，如DRAM）
 * 
 * 地址空间布局：
 * ┌─────────────────────────────────────────────────────────────────┐
 * │ 地址范围        │ 大小    │ 区域名称       │ 用途                 │
 * ├─────────────────────────────────────────────────────────────────┤
 * │ 0x00001000     │ 60KB    │ MROM           │ 启动固件（Boot ROM） │
 * │ 0x02000000     │ 64KB    │ CLINT          │ CPU本地中断控制器     │
 * │ 0x30009000     │ 4KB     │ NS16550 UART   │ 串口1                │
 * │ 0x310a0000     │ 64KB    │ DWMAC          │ 以太网控制器2        │
 * │ 0x40600000     │ 4KB     │ UART Lite      │ 串口2                │
 * │ 0x40c00000     │ 64KB    │ AXI Ethernet   │ 以太网控制器1        │
 * │ 0x41e00000     │ 64KB    │ AXI DMA        │ DMA控制器            │
 * │ 0x80000000+    │ 可变    │ DRAM           │ 主内存               │
 * └─────────────────────────────────────────────────────────────────┘
 */
static const MemMapEntry vcore_memmap[] = {
    /* MROM: 0x1000 - 0xFFFF，60KB */
    [VCORE_MROM] =          {     0x1000,     0xf000 },
    
    /* CLINT: 0x2000000 - 0x200FFFF，64KB */
    [VCORE_CLINT] =         {  0x2000000,    0x10000 },
    
    /* NS16550 UART: 0x30009000 - 0x30009FFF，4KB */
    [VCORE_UART_NS16550] =  {  0x30009000,   0x1000 },
    
    /* Xilinx UART Lite: 0x40600000 - 0x40600FFF，4KB */
    [VCORE_UART_UARTLITE] = {  0x40600000,   0x1000 },
    
    /* Xilinx AXI Ethernet: 0x40c00000 - 0x40c0FFFF，64KB */
    [VCORE_ETH_AXI] =       {  0x40c00000,  0x10000 },
    
    /* Xilinx AXI DMA: 0x41e00000 - 0x41e0FFFF，64KB */
    [VCORE_ETH_AXI_DMA] =   {  0x41e00000,  0x10000 },
    
    /* Synopsys DWMAC: 0x310a0000 - 0x310aFFFF，64KB */
    [VCORE_ETH_DWMAC] =     {  0x310a0000,  0x10000 },
    
    /* DRAM: 0x80000000+，大小由用户通过-m参数指定 */
    [VCORE_DRAM] =          { 0x80000000,        0x0 },
};

/*
 * ==========================================================================
 * 第二部分：板初始化中的内存配置（来自vcore_board_init()）
 * ==========================================================================
 */

static void vcore_board_init(MachineState *machine)
{
    /* 获取内存映射表 */
    const MemMapEntry *memmap = vcore_memmap;
    
    /* 获取vcore板状态 */
    VCoreState *s = VCORE_MACHINE(machine);
    
    /* 获取系统内存区域对象 */
    MemoryRegion *system_memory = get_system_memory();
    
    /* 创建Mask ROM内存区域对象 */
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);
    
    /* 固件加载地址和结束地址 */
    hwaddr firmware_end_addr = memmap[VCORE_DRAM].base;
    hwaddr firmware_load_addr = memmap[VCORE_DRAM].base;
    
    /* 内核启动地址 */
    vaddr kernel_start_addr;
    
    /* 固件文件名 */
    char *firmware_name;
    
    /* FDT（设备树）加载地址 */
    uint64_t fdt_load_addr;
    
    /* 内核入口点地址 */
    uint64_t kernel_entry;
    
    /* RISC-V启动信息结构 */
    RISCVBootInfo boot_info;
    
    /* ====================================================================
     * 步骤1：映射主RAM到系统内存
     * ====================================================================
     * 
     * 将用户指定大小的RAM（通过-m参数）映射到物理地址0x80000000
     * machine->ram是QEMU在启动时根据用户指定的内存大小创建的内存区域
     */
    memory_region_add_subregion(
        system_memory,           /* 父内存区域（系统内存） */
        memmap[VCORE_DRAM].base, /* 子区域的物理地址（DRAM基址） */
        machine->ram             /* 要映射的内存区域 */
    );
    
    /* ====================================================================
     * 步骤2：创建并映射Mask ROM（只读内存）
     * ====================================================================
     * 
     * Mask ROM用于存储启动固件（如OpenSBI），CPU复位后从这里开始执行
     * 
     * QEMU的MemoryRegion类型：
     * - memory_region_init_rom: 只读内存（模拟ROM）
     * - memory_region_init_ram: 可读写内存（模拟RAM）
     * - memory_region_init_io: I/O区域（用于外设）
     */
    memory_region_init_rom(
        mask_rom,                /* 内存区域对象指针 */
        NULL,                    /* 所有者对象（无） */
        "riscv.vcore.mrom",      /* 区域名称（用于调试） */
        memmap[VCORE_MROM].size, /* 区域大小 */
        &error_fatal             /* 错误处理回调 */
    );
    
    /* 将MROM映射到物理地址0x1000 */
    memory_region_add_subregion(
        system_memory,
        memmap[VCORE_MROM].base,
        mask_rom
    );
    
    /* ====================================================================
     * 步骤3：加载固件到内存
     * ====================================================================
     * 
     * 固件（firmware）通常是OpenSBI或类似的启动程序，负责：
     * 1. 初始化硬件
     * 2. 设置页表
     * 3. 启动内核
     */
    
    /* 查找固件文件（优先使用用户指定的，否则使用默认固件） */
    firmware_name = riscv_find_firmware(
        machine->firmware,                           /* 用户指定的固件 */
        riscv_default_firmware_name(&s->soc[0])     /* 默认固件名称 */
    );
    
    /* 如果找到了固件文件 */
    if (firmware_name) {
        /* 加载固件到内存，并获取加载地址和结束地址 */
        firmware_end_addr = riscv_load_firmware(
            firmware_name,         /* 固件文件名 */
            &firmware_load_addr,   /* 输出：固件加载地址 */
            NULL                   /* 输出：固件入口点（可选） */
        );
        
        /* 释放固件名字符串 */
        g_free(firmware_name);
    }
    
    /* ====================================================================
     * 步骤4：加载内核和设备树
     * ====================================================================
     */
    
    /* 初始化启动信息结构 */
    riscv_boot_info_init(&boot_info, &s->soc[0]);
    
    /* 如果用户指定了内核文件 */
    if (machine->kernel_filename) {
        /* 计算内核加载地址（在固件之后） */
        kernel_start_addr = riscv_calc_kernel_start_addr(
            &boot_info,        /* 启动信息 */
            firmware_end_addr  /* 固件结束地址 */
        );
        
        /* 加载内核到内存 */
        riscv_load_kernel(
            machine,            /* 机器状态 */
            &boot_info,         /* 启动信息 */
            kernel_start_addr,   /* 内核加载地址 */
            true,               /* 是否是大端序 */
            NULL                /* 内核入口点（可选，自动检测） */
        );
        
        /* 获取内核入口点 */
        kernel_entry = boot_info.image_low_addr;
    } else {
        /* 没有指定内核，入口点设为0 */
        kernel_entry = 0;
    }
    
    /* ====================================================================
     * 步骤5：计算并加载设备树（FDT）
     * ====================================================================
     * 
     * 设备树（Flattened Device Tree）是一种描述硬件的标准格式，
     * 告诉内核系统中有哪些设备以及它们的地址等信息。
     */
    
    /* 计算FDT在内存中的加载地址 */
    fdt_load_addr = riscv_compute_fdt_addr(
        memmap[VCORE_DRAM].base,  /* DRAM基地址 */
        machine->ram_size,        /* RAM大小 */
        machine,                  /* 机器状态 */
        &boot_info                /* 启动信息 */
    );
    
    /* 将FDT写入内存 */
    riscv_load_fdt(fdt_load_addr, machine->fdt);
    
    /* ====================================================================
     * 步骤6：设置ROM复位向量
     * ====================================================================
     * 
     * 配置CPU复位后执行的第一条指令地址（复位向量），
     * 以及传递给固件/内核的启动参数（如FDT地址）。
     */
    riscv_setup_rom_reset_vec(
        machine,               /* 机器状态 */
        &s->soc[0],            /* HART数组状态 */
        firmware_load_addr,     /* 固件加载地址 */
        memmap[VCORE_MROM].base, /* ROM基地址 */
        memmap[VCORE_MROM].size,  /* ROM大小 */
        kernel_entry,           /* 内核入口点 */
        fdt_load_addr           /* FDT加载地址 */
    );
}

/*
 * ==========================================================================
 * 第三部分：设备树中的内存描述（来自create_fdt()）
 * ==========================================================================
 */

static void create_fdt(VCoreState *s, const MemMapEntry *memmap, bool is_32_bit)
{
    void *fdt;                           /* 设备树缓冲区 */
    int fdt_size;                        /* 设备树大小 */
    uint64_t addr, size;                 /* 地址和大小 */
    MachineState *ms = MACHINE(s);       /* 机器状态 */
    char *mem_name;                      /* 内存节点名称 */
    
    /* 创建空的设备树 */
    fdt = ms->fdt = create_device_tree(&fdt_size);
    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(1);
    }
    
    /* ====================================================================
     * 创建内存节点
     * ====================================================================
     * 
     * 在设备树中描述系统内存区域，供内核识别和使用。
     * 
     * 设备树节点格式：
     * /memory@80000000 {
     *     device_type = "memory";
     *     reg = <0x0 0x80000000 0x0 0x20000000>;  // 512MB
     * };
     */
    
    /* 获取DRAM基地址和大小 */
    addr = memmap[VCORE_DRAM].base;
    size = ms->ram_size;
    
    /* 创建内存节点名称，格式：/memory@<地址> */
    mem_name = g_strdup_printf("/memory@%lx", (long)addr);
    
    /* 添加内存节点到设备树 */
    qemu_fdt_add_subnode(fdt, mem_name);
    
    /* 设置reg属性（地址和大小） */
    qemu_fdt_setprop_cells(
        fdt,                              /* 设备树缓冲区 */
        mem_name,                         /* 节点路径 */
        "reg",                            /* 属性名 */
        addr >> 32,                       /* 高32位地址 */
        addr,                             /* 低32位地址 */
        size >> 32,                       /* 高32位大小 */
        size                              /* 低32位大小 */
    );
    
    /* 设置device_type属性为"memory" */
    qemu_fdt_setprop_string(fdt, mem_name, "device_type", "memory");
    
    /* 释放临时字符串 */
    g_free(mem_name);
    
    /* ====================================================================
     * 创建其他节点（略）
     * ====================================================================
     */
}

/*
 * ==========================================================================
 * 第四部分：QEMU内存区域API说明
 * ==========================================================================
 */

/*
 * QEMU内存区域类型：
 * 
 * 1. MemoryRegion - 内存区域抽象基类
 *    - 可以是RAM、ROM、I/O等类型
 *    - 支持层次结构（父区域包含子区域）
 * 
 * 2. memory_region_init_ram() - 初始化RAM区域
 *    - 可读写内存
 *    - 用于模拟物理RAM
 * 
 * 3. memory_region_init_rom() - 初始化ROM区域
 *    - 只读内存
 *    - 用于模拟固件、BIOS等
 * 
 * 4. memory_region_init_io() - 初始化I/O区域
 *    - 用于外设寄存器
 *    - 需要提供读写回调函数
 * 
 * 5. memory_region_add_subregion() - 添加子区域
 *    - 将子区域映射到父区域的指定地址
 *    - 建立地址映射关系
 * 
 * 6. get_system_memory() - 获取系统内存根区域
 *    - 返回整个物理地址空间的根MemoryRegion
 */

/*
 * ==========================================================================
 * 第五部分：内存布局设计说明
 * ==========================================================================
 */

/*
 * RISC-V内存布局设计考虑：
 * 
 * 1. MROM（0x1000-0xFFFF）
 *    - CPU复位向量地址是0x1000（RISC-V标准）
 *    - 存储OpenSBI等启动固件
 *    - 大小60KB，足够存储标准OpenSBI
 * 
 * 2. CLINT（0x2000000-0x200FFFF）
 *    - Core Local Interruptor
 *    - 包含软件中断寄存器和定时器
 *    - 每个HART有独立的定时器比较寄存器
 * 
 * 3. 外设区域（0x30000000-0x4FFFFFFF）
 *    - UART、Ethernet、DMA等外设
 *    - 按功能分组，地址空间预留扩展空间
 * 
 * 4. DRAM（0x80000000+）
 *    - 主系统内存
 *    - 通常从0x80000000开始（RISC-V常见约定）
 *    - 大小由用户通过-m参数指定
 * 
 * 5. 内存映射原则：
 *    - 低地址（< 0x80000000）用于I/O和特殊区域
 *    - 高地址（>= 0x80000000）用于DRAM
 *    - 外设地址按功能模块分组，便于管理和扩展
 */

/*
 * ==========================================================================
 * 第六部分：启动流程中的内存操作
 * ==========================================================================
 */

/*
 * 启动流程内存操作时序：
 * 
 * 1. QEMU初始化阶段：
 *    - 根据-m参数创建RAM区域（machine->ram）
 *    - 创建系统内存根区域
 * 
 * 2. 板初始化阶段（vcore_board_init）：
 *    - 映射RAM到0x80000000
 *    - 创建MROM区域
 * 
 * 3. 固件加载阶段：
 *    - 读取OpenSBI等固件文件
 *    - 将固件写入MROM或DRAM
 * 
 * 4. 设备树生成阶段（create_fdt）：
 *    - 创建内存节点描述
 *    - 创建外设节点描述
 * 
 * 5. 内核加载阶段：
 *    - 读取内核文件
 *    - 将内核写入DRAM
 *    - 将设备树写入DRAM
 * 
 * 6. CPU启动阶段：
 *    - CPU复位到0x1000（MROM起始地址）
 *    - 执行固件（OpenSBI）
 *    - 固件设置页表
 *    - 固件跳转到内核入口
 * 
 * 7. 内核启动阶段：
 *    - 内核读取设备树
 *    - 内核初始化内存管理单元（MMU）
 *    - 内核建立页表映射
 *    - 内核启用虚拟内存
 */

/*
 * ==========================================================================
 * 第七部分：内存相关工具函数说明
 * ==========================================================================
 */

/*
 * riscv_find_firmware() - 查找固件文件
 * 参数：
 *   - firmware: 用户指定的固件路径（可为NULL）
 *   - default_firmware: 默认固件名称
 * 返回：
 *   - 固件文件的完整路径（成功）
 *   - NULL（失败）
 * 
 * riscv_load_firmware() - 加载固件到内存
 * 参数：
 *   - filename: 固件文件名
 *   - load_addr: 输出参数，固件加载地址
 *   - entry: 输出参数，固件入口点（可选）
 * 返回：
 *   - 固件结束地址
 * 
 * riscv_load_kernel() - 加载内核到内存
 * 参数：
 *   - machine: 机器状态
 *   - boot_info: 启动信息结构
 *   - addr: 内核加载地址
 *   - big_endian: 是否大端序
 *   - entry: 输出参数，内核入口点（可选）
 * 
 * riscv_compute_fdt_addr() - 计算设备树加载地址
 * 参数：
 *   - dram_base: DRAM基地址
 *   - dram_size: DRAM大小
 *   - machine: 机器状态
 *   - boot_info: 启动信息结构
 * 返回：
 *   - FDT加载地址（通常在DRAM末尾附近）
 * 
 * riscv_load_fdt() - 将设备树写入内存
 * 参数：
 *   - addr: 写入地址
 *   - fdt: 设备树缓冲区
 * 
 * riscv_setup_rom_reset_vec() - 设置复位向量
 * 参数：
 *   - machine: 机器状态
 *   - soc: HART数组状态
 *   - firmware_load_addr: 固件加载地址
 *   - rom_base: ROM基地址
 *   - rom_size: ROM大小
 *   - kernel_entry: 内核入口点
 *   - fdt_load_addr: FDT加载地址
 */

/*
 * ==========================================================================
 * 第八部分：内存区域大小计算示例
 * ==========================================================================
 */

/*
 * 假设用户指定内存大小为512MB（-m 512M）：
 * 
 * DRAM区域：
 *   - 基地址：0x80000000
 *   - 大小：0x20000000（512MB）
 *   - 结束地址：0x9FFFFFFF
 * 
 * MROM区域：
 *   - 基地址：0x1000
 *   - 大小：0xF000（60KB）
 *   - 结束地址：0xFFFF
 * 
 * CLINT区域：
 *   - 基地址：0x2000000
 *   - 大小：0x10000（64KB）
 *   - 结束地址：0x200FFFF
 * 
 * FDT加载地址：
 *   - 通常在DRAM末尾减去FDT大小
 *   - 例如：0x9FFFFFFF - FDT_SIZE
 */

/*
 * ==========================================================================
 * 第九部分：常见内存相关问题排查
 * ==========================================================================
 */

/*
 * 1. 固件无法加载：
 *    - 检查固件文件路径是否正确
 *    - 检查MROM大小是否足够
 *    - 检查firmware参数是否正确设置
 * 
 * 2. 内核无法启动：
 *    - 检查内核格式是否正确（ELF格式）
 *    - 检查内核加载地址是否正确
 *    - 检查设备树是否正确生成
 * 
 * 3. 内存访问错误：
 *    - 检查地址映射是否正确
 *    - 检查内存区域类型是否正确（ROM/RAM/IO）
 *    - 检查设备树内存描述是否正确
 * 
 * 4. 性能问题：
 *    - 检查内存区域是否使用了正确的优化标志
 *    - 检查是否启用了KVM加速
 *    - 检查内存大小是否超过物理内存限制
 */
