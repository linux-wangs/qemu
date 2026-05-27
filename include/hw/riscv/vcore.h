#ifndef HW_RISCV_VCORE_H
#define HW_RISCV_VCORE_H

#include "hw/core/boards.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/core/sysbus.h"

#define VCORE_CPUS_MAX 8
#define VCORE_SOCKETS_MAX 1

#define TYPE_VCORE_MACHINE MACHINE_TYPE_NAME("vcore")
typedef struct VCoreState VCoreState;
DECLARE_INSTANCE_CHECKER(VCoreState, VCORE_MACHINE,
                         TYPE_VCORE_MACHINE)

struct VCoreState {
    MachineState parent;

    RISCVHartArrayState soc[VCORE_SOCKETS_MAX];
};

enum {
    VCORE_MROM = 0,
    VCORE_CLINT,
    VCORE_UART_NS16550,
    VCORE_UART_UARTLITE,
    VCORE_ETH_AXI,
    VCORE_ETH_AXI_DMA,
    VCORE_ETH_DWMAC,
    VCORE_DRAM,
};

#endif
