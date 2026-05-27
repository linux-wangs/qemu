/*
 * QEMU model of the RISC-V vcore development board.
 *
 * This header file defines the vcore board-specific constants, data structures,
 * and memory map for the custom RISC-V vcore board.
 *
 * Memory Map Layout (from user requirements):
 * - 0x00001000-0x0000FFFF: MROM (mask ROM) - boot firmware
 * - 0x02000000-0x0200FFFF: CLINT (Core Local Interruptor)
 * - 0x30009000-0x30009FFF: NS16550 UART (serial port 0)
 * - 0x40600000-0x40600FFF: Xilinx UART Lite (serial port 1)
 * - 0x40c00000-0x40c0FFFF: Xilinx AXI Ethernet controller
 * - 0x41e00000-0x41e0FFFF: Xilinx AXI DMA (for Ethernet)
 * - 0x310a0000-0x310aFFFF: Synopsys DWMAC Ethernet controller
 * - 0x80000000+: DRAM (system memory)
 */

#ifndef HW_RISCV_VCORE_H
#define HW_RISCV_VCORE_H

#include "hw/core/boards.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/core/sysbus.h"

/* Maximum number of CPUs supported by the vcore board */
#define VCORE_CPUS_MAX 8

/* Maximum number of sockets (chiplets) supported */
#define VCORE_SOCKETS_MAX 1

/* QEMU type definition for the vcore machine */
#define TYPE_VCORE_MACHINE MACHINE_TYPE_NAME("vcore")
typedef struct VCoreState VCoreState;

/* Macro to check if an object is a VCoreState instance */
DECLARE_INSTANCE_CHECKER(VCoreState, VCORE_MACHINE, TYPE_VCORE_MACHINE)

/**
 * struct VCoreState - Main state structure for the vcore board
 * @parent: Parent MachineState object
 * @soc: Array of RISC-V HART array states (one per socket)
 */
struct VCoreState {
    MachineState parent;

    /* RISC-V HART array state for each socket */
    RISCVHartArrayState soc[VCORE_SOCKETS_MAX];
};

/**
 * enum - Memory region identifiers for the vcore board
 *
 * These indices are used to access the memory map array and identify
 * different memory regions in the device tree and board initialization.
 */
enum {
    VCORE_MROM = 0,          /* Mask ROM for boot firmware */
    VCORE_CLINT,             /* Core Local Interrupt Controller */
    VCORE_UART_NS16550,      /* NS16550A UART (primary serial port) */
    VCORE_UART_UARTLITE,     /* Xilinx UART Lite (secondary serial port) */
    VCORE_ETH_AXI,           /* Xilinx AXI Ethernet controller */
    VCORE_ETH_AXI_DMA,       /* Xilinx AXI DMA for Ethernet */
    VCORE_ETH_DWMAC,         /* Synopsys DWMAC Ethernet controller */
    VCORE_DRAM,              /* Main system DRAM */
};

#endif /* HW_RISCV_VCORE_H */
