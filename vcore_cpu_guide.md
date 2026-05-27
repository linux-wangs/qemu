# vcore开发板 - CPU部分代码详解
=========================================

## 1. 头文件定义部分
---

### vcore.h 中的CPU相关定义

```c
#include "hw/riscv/vcore.h"
```

---

### 代码解释：

**头文件中包含了CPU相关的关键数据结构：

| 字段/宏定义
```c
#define VCORE_CPUS_MAX 8           // 定义最大支持8个CPU核心（HART）
#define VCORE_SOCKETS_MAX 1       // 定义最大支持1个CPU Socket
#define TYPE_VCORE_MACHINE MACHINE_TYPE_NAME("vcore")  // 定义机器类型

/* 声明VCoreState类型检查宏
```

---

## 核心数据结构VCoreState
```c
struct VCoreState {
    MachineState parent;            // 继承自QEMU通用机器状态
    RISCVHartArrayState soc[VCORE_SOCKETS_MAX];  // RISC-V HART数组，每个HART代表一个CPU核心
};
```

---

### vcore.c中的内存映射（包含CLINT）
```c
// 内存映射枚举
static const MemMapEntry vcore_memmap[] = {
    [VCORE_MROM] = {     0x1000,     0xf000 }, // Mask ROM（启动固件）
    [VCORE_CLINT] = {  0x2000000,    0x10000 }, // CLINT（CPU本地中断控制器
    // ... 其他外设 ...
    [VCORE_DRAM] = { 0x80000000,        0x0 }, // 主内存
};
```

---

## 2. CPU初始化核心代码（来自vcore.c
---

### 完整的vcore_board_init()函数中CPU初始化部分

```c
static void vcore_board_init(MachineState *machine)
{
    const MemMapEntry *memmap = vcore_memmap;  // 内存映射指针
    VCoreState *s = VCORE_MACHINE(machine);    // 转换为vcore状态结构体
    MemoryRegion *system_memory = get_system_memory(); // 获取系统内存区域
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);  // MROM内存区域
    hwaddr firmware_end_addr = memmap[VCORE_DRAM].base; // 固件结束地址
    hwaddr firmware_load_addr = memmap[VCORE_DRAM].base; // 固件加载地址
    vaddr kernel_start_addr;   // 内核起始地址
    char *firmware_name;       // 固件文件名
    uint64_t fdt_load_addr;    // FDT在内存中的加载地址
    uint64_t kernel_entry;     // 内核入口点
    int i, base_hartid = 0, hart_count = 1;  // CPU配置参数
    RISCVBootInfo boot_info;   // 启动信息结构体
    DeviceState *dev, *eth0, *dma;  // 设备句柄
    Object *ds, *cs;           // 流接口对象
```

---

### CPU Socket循环遍历并创建HART数组（CPU核心）
```c
    /* 初始化CPU Socket（目前只支持单个Socket） */
    for (i = 0; i < VCORE_SOCKETS_MAX; i++) {
        hart_count = machine->smp.cpus;  // 从机器配置获取CPU数量

        /* 创建该Socket的RISC-V HART数组 */
        object_initialize_child(OBJECT(machine), "soc", &s->soc[i],
                                TYPE_RISCV_HART_ARRAY);  // 初始化子对象
```

---

### 配置HART属性
```c
        // 设置CPU类型属性
        object_property_set_str(OBJECT(&s->soc[i]), "cpu-type",
                                machine->cpu_type, &error_abort);
        
        // 设置第一个HART的ID起始值
        object_property_set_int(OBJECT(&s->soc[i]), "hartid-base",
                                base_hartid, &error_abort);
        
        // 设置HART（CPU数量
        object_property_set_int(OBJECT(&s->soc[i]), "num-harts",
                                hart_count, &error_abort);
        
        // 使能HART数组（系统总线设备
        sysbus_realize(SYS_BUS_DEVICE(&s->soc[i]), &error_fatal);
```

---

### 创建CLINT（Core Local Interruptor（核心本地中断控制器）
```c
        /* 创建ACINT SWI（软件中断）控制器 */
        riscv_aclint_swi_create(
            memmap[VCORE_CLINT].base + i * memmap[VCORE_CLINT].size,  // SWI基地址
            base_hartid,  // 起始HART ID
            hart_count,  // HART数量
            false);  // 是否有ACLINT MSWI
        
        /* 创建ACINT MTIMER（定时器）控制器 */
        riscv_aclint_mtimer_create(
            // MTIMER基地址（SWI后面）
            memmap[VCORE_CLINT].base + i * memmap[VCORE_CLINT].size + RISCV_ACLINT_SWI_SIZE,
            // MTIMER大小
            RISCV_ACLINT_DEFAULT_MTIMER_SIZE, 
            // 起始HART ID
            base_hartid, 
            // HART数量
            hart_count,
            // MTIMECMP寄存器
            RISCV_ACLINT_DEFAULT_MTIMECMP,
            // MTIME寄存器
            RISCV_ACLINT_DEFAULT_MTIME,
            // 定时器频率
            RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ,
            // 是否有ACLINT MTimer
            false);
```

---

## 3. CPU相关的核心函数

### CLINT（Core Local Interruptor（核心本地中断控制器）

CLINT（Core Local Interruptor是RISC-V架构中的重要组件：

|功能|说明|
|------|------|
|MTIME|64位计时器|
|MTIMECMP|比较寄存器，每个HART一个|
|MSIP|机器模式软件中断寄存器|

---

### CLINT地址映射（来自vcore_memmap[]
```c
[VCORE_CLINT] = {  0x2000000,    0x10000 } // 64KB大小
```

---

## 4. 设备树中的CPU节点创建（来自create_fdt()函数）
```c
    /* CPUs节点 - 描述RISC-V HART */
    qemu_fdt_add_subnode(fdt, "/cpus");
    // 设置时基频率
    qemu_fdt_setprop_cell(fdt, "/cpus", "timebase-frequency",
        RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0x0);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 0x1);
```

---

### 每个CPU的设备树节点
```c
    /* 添加每个CPU及其中断控制器
    for (cpu = s->soc[0].num_harts - 1; cpu >= 0; cpu--) {
        cpu_phandle = phandle++;
        
        /* 创建CPU节点
        cpu_name = g_strdup_printf("/cpus/cpu@%d", s->soc[0].hartid_base + cpu);
        qemu_fdt_add_subnode(fdt, cpu_name);
        
        // 根据32位/64位设置MMU类型
        if (is_32_bit) {
            qemu_fdt_setprop_string(fdt, cpu_name, "mmu-type", "riscv,sv32");
        } else {
            qemu_fdt_setprop_string(fdt, cpu_name, "mmu-type", "riscv,sv48");
        }
        
        // 写入ISA字符串到DT
        riscv_isa_write_fdt(&s->soc[0].harts[cpu], fdt, cpu_name);
        
        // 设置CPU属性
        qemu_fdt_setprop_string(fdt, cpu_name, "compatible", "riscv");
        qemu_fdt_setprop_string(fdt, cpu_name, "status", "okay");
        qemu_fdt_setprop_cell(fdt, cpu_name, "reg", s->soc[0].hartid_base + cpu);
        qemu_fdt_setprop_string(fdt, cpu_name, "device_type", "cpu");
        qemu_fdt_setprop_cell(fdt, cpu_name, "phandle", cpu_phandle);
```

---

### CPU中断控制器节点
```c
        /* 创建CPU中断控制器节点
        intc_name = g_strdup_printf("%s/interrupt-controller", cpu_name);
        qemu_fdt_add_subnode(fdt, intc_name);
        intc_phandle = phandle++;
        qemu_fdt_setprop_cell(fdt, intc_name, "phandle", intc_phandle);
        qemu_fdt_setprop_string(fdt, intc_name, "compatible",
            "riscv,cpu-intc");
        qemu_fdt_setprop(fdt, intc_name, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, intc_name, "#interrupt-cells", 1);
```

---

### CLINT中断连接
```c
        /* CLINT中断单元格填充
        clint_cells[cpu * 4 + 0] = cpu_to_be32(intc_phandle);
        clint_cells[cpu * 4 + 1] = cpu_to_be32(IRQ_M_SOFT);  // 机器模式软件中断
        clint_cells[cpu * 4 + 2] = cpu_to_be32(intc_phandle);
        clint_cells[cpu * 4 + 3] = cpu_to_be32(IRQ_M_TIMER); // 机器模式定时器中断
```

---

## 5. 核心数据结构详解

### RISCVHartArrayState
```c
/* 每个字段：

- soc[VCORE_SOCKETS_MAX]; // RISC-V HART数组，每个HART是一个CPU核心
```

---

### RISCVBootInfo
```c
// riscv_boot_info_init(&boot_info, &s->soc[0]);
```

---

## 6. 启动流程与CPU相关的部分
```c
// 准备启动信息用于内核加载
riscv_boot_info_init(&boot_info, &s->soc[0]);
if (machine->kernel_filename) {
    kernel_start_addr = riscv_calc_kernel_start_addr(&boot_info, firmware_end_addr);
    
    // 加载内核到内存
    riscv_load_kernel(machine, &boot_info, kernel_start_addr, true, NULL);
    kernel_entry = boot_info.image_low_addr;
} else {
    kernel_entry = 0;
}

// 计算FDT加载地址
fdt_load_addr = riscv_compute_fdt_addr(memmap[VCORE_DRAM].base,
                                       machine->ram_size,
                                       machine, &boot_info);
riscv_load_fdt(fdt_load_addr, machine->fdt);

// 设置ROM的复位向量
riscv_setup_rom_reset_vec(machine, &s->soc[0], firmware_load_addr,
                          memmap[VCORE_MROM].base,
                          memmap[VCORE_MROM].size, kernel_entry,
                          fdt_load_addr);
```

---

## 总结：

## 7. 相关文件位置

|文件|路径|说明|
|------|------|------|
|CPU状态|/workspace/hw/riscv/vcore.c|vcore板实现|
|头文件|/workspace/include/hw/riscv/vcore.h|vcore板头文件|
|RISC-V HART|target/riscv/|RISC-V CPU实现|
|CLINT|hw/intc/riscv_aclint.c|CLINT实现|
