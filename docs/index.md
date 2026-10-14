---
layout: default
title: Zynq-7000 QEMU + XSim co-simulation
---

# Zynq-7000 PS/PL co-simulation with Xilinx QEMU and Vivado XSim

Run bare-metal Cortex-A9 firmware in Xilinx QEMU and have it read and write
**your own RTL** simulated in Vivado XSim, natively on Windows. No SystemC, no
PetaLinux, no board.

```
 ELF (ARM, bare metal)            QEMU arm-generic-fdt-7series
 Xil_Out32(0x43C00000, v)  --->   remote-port chardev (TCP)  --->  XSim
 Xil_In32 (0x43C00004)     <---                                     cosim_top.sv
                                                                    rp_dpi.c (DPI-C)
                                                                    my_math_ip.v
```

## Result

| Step | Status |
|---|---|
| Block design to XSA (Vivado 2022.1) | verified |
| Bare-metal ELF (Vitis 2022.1) | verified |
| QEMU <-> XSim on Windows | **verified: 7/7 vectors, 21 AXI4-Lite beats** |
| Linux / WSL2 script | written, not run |

## Quick start

```powershell
vivado -mode batch -source setup_bd.tcl
C:\Xilinx\Vitis\2022.1\bin\xsct.bat build_app.tcl
powershell -ExecutionPolicy Bypass -File run_cosim_win.ps1
```

## Why this is not just "use libsystemctlm-soc"

Xilinx's open-source SystemC library speaks the same remote-port protocol, but on
Windows with XSim it hit a chain of problems. The working route replaces the
SystemC side with a small DPI-C client:

| Problem | Fix |
|---|---|
| Co-sim DTB has no `chrdev-id`: `Missing chardesc prop` | `-chardev socket,id=pl-rp,...` and `-global remote-port.chardev=pl-rp` |
| Windows QEMU: `unix sockets are not available on windows` | TCP chardev with `wait=on` |
| `xsimk.exe` exits with 0xC0000139 when DPI objects import `ws2_32` | bind winsock at run time (`wsdyn.c`) |
| `xsc --gcc_link_options` is passed to `ar.exe` | no link options; `xelab -sv_root ... -sv_lib dpi` |
| SystemC module instantiated from Verilog is not found (stub generation) | plain DPI-C instead of SystemC |

## Protocol handling

`rp_dpi.c` implements the PL side of the remote-port protocol:

1. connect, send `hello` (capability `BUSACCESS_EXT_BASE`), process QEMU's hello;
2. answer `sync` packets with QEMU's own timestamp;
3. turn each read/write into 32-bit beats (`rp_next`), the testbench drives
   AXI4-Lite and calls `rp_done`, which sends the response;
4. consume interrupt, cfg and ATS packets (wire updates stay posted).

When the firmware prints `COSIM_DONE` the script stops QEMU, XSim sees the
socket close and ends with `$finish`, which flushes the waveform database.

## Limits

- One AXI4-Lite master; no PS interrupts, no AFI/ACP.
- Simulated time follows the sync responses, it is not cycle-accurate PS/PL timing.
- Tested only with Vivado/Vitis 2022.1 on Windows 11.

## Links

- [Troubleshooting](troubleshooting)
- Source and README: the repository root
- License: Apache-2.0 (`sim/winport/remote-port-sk.c` and `safeio.c` are MIT-derived, see `NOTICE`)
