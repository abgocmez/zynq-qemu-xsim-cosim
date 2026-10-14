# Zynq-7000 PS/PL co-simulation: Xilinx QEMU + Vivado XSim

Run bare-metal Cortex-A9 firmware in Xilinx QEMU and have it talk to your **own
RTL** simulated in Vivado XSim, natively on **Windows** (and, untested, Linux).
No SystemC, no PetaLinux, no real board.

The demo target is a Digilent Zybo Z7-20 (`xc7z020clg400-1`) with a custom
AXI4-Lite IP on `M_AXI_GP0` at `0x43C00000`:

```
 ELF (ARM, bare metal)            QEMU arm-generic-fdt-7series
 Xil_Out32(0x43C00000, v)  --->   remote-port chardev (TCP)  --->  XSim
 Xil_In32 (0x43C00004)     <---                                     cosim_top.sv
                                                                    rp_dpi.c (DPI-C)
                                                                    my_math_ip.v
```

The PL side is a ~250-line DPI-C client of Xilinx's *remote-port* protocol
([`sim/winport/rp_dpi.c`](sim/winport/rp_dpi.c)). It turns each PS access into
AXI4-Lite beats driven by a small SystemVerilog master.

## Status

| Part | Verified |
|---|---|
| `setup_bd.tcl` -> XSA (Vivado 2022.1, Windows 11) | yes |
| `build_app.tcl` -> ELF (Vitis/xsct 2022.1) | yes |
| `run_cosim_win.ps1` end to end (Windows, QEMU from Vitis 2022.1) | **yes: 7/7 vectors PASS, 21 AXI beats** |
| `run_cosim.sh` (Linux / WSL2) | **no**: same design, never run |

## Requirements

- Vivado and Vitis **2022.1**. QEMU and the Zynq co-sim device tree are taken from
  the Vitis install: `data/emulation/qemu_win/qemu/qemu-system-aarch64.exe` and
  `data/emulation/dtbs/zynq/zynq-arm-cosim.dtb`.
- Git: the scripts clone [`Xilinx/libsystemctlm-soc`](https://github.com/Xilinx/libsystemctlm-soc)
  into `third_party/`, only for `libremote-port/remote-port-proto.c` (packet encode/decode).

## Run (Windows)

```powershell
vivado -mode batch -source setup_bd.tcl                  # -> out\system_wrapper.xsa
C:\Xilinx\Vitis\2022.1\bin\xsct.bat build_app.tcl        # -> out\math_test.elf
powershell -ExecutionPolicy Bypass -File run_cosim_win.ps1          # batch
powershell -ExecutionPolicy Bypass -File run_cosim_win.ps1 -Gui     # waveform viewer
```

Expected output ends with `RESULT: ALL PASS` and `CO-SIM PASS`. Waveforms are in
`build\cosim_win\cosim.wdb`. On Linux/WSL2 try `./run_cosim.sh` (see its header).

## Use it with your own IP

1. Replace `rtl/my_math_ip.v` (keep AXI4-Lite `S_AXI_*` ports) and adapt the DUT
   instance and address width in `sim/cosim_top.sv`.
2. Replace `sw/main.c`; print `COSIM_DONE` when finished (the scripts stop on it).
3. Re-run the three commands above.

## What was hard (and is solved here)

| Problem | Fix |
|---|---|
| The Xilinx co-sim DTB has no `chrdev-id`, so QEMU says `Missing chardesc prop` | `-chardev socket,id=pl-rp,...` plus `-global remote-port.chardev=pl-rp` |
| Windows QEMU has no unix sockets (`-machine-path` fails) | TCP chardev, `wait=on` so QEMU blocks until XSim connects |
| `xsimk.exe` refuses to start (0xC0000139) when DPI objects import `ws2_32` statically | winsock bound at run time ([`wsdyn.c`](sim/winport/wsdyn.c)) |
| `xsc` forwards `--gcc_link_options` to `ar.exe` on Windows | pass none; use plain `xsc`, then `xelab -sv_lib dpi` |
| Instantiating SystemC from Verilog in XSim (stub generation) did not resolve | dropped SystemC; plain DPI-C |

Details and more failure modes: [TROUBLESHOOTING.md](docs/troubleshooting.md).

## Layout

| Path | Purpose |
|---|---|
| `rtl/my_math_ip.v` | AXI4-Lite slave: reg0 RW, reg1 = reg0 * 2 |
| `setup_bd.tcl` | Block design (`processing_system7_0` + IP at `0x43C00000`), exports XSA |
| `sw/main.c`, `build_app.tcl` | Bare-metal test and its xsct build |
| `sim/cosim_top.sv` | Clock/reset, DUT, AXI-Lite master driven through DPI-C |
| `sim/winport/rp_dpi.c` | Remote-port client: hello, sync, bus read/write |
| `sim/winport/wsdyn.[ch]`, `remote-port-sk.c`, `safeio.c` | Windows (winsock) layer |
| `run_cosim_win.ps1`, `run_cosim.sh` | Build, launch QEMU and XSim, check result |

## Limits

- Single AXI4-Lite master; no interrupts back to the PS, no AFI/ACP ports.
- Simulated time is not locked to QEMU time (sync responses echo QEMU's timestamp).
  This checks function, not cycle-accurate PS/PL timing.
- Accesses wider than 32 bits are split into 32-bit beats.

## License

Apache-2.0, see [LICENSE](LICENSE). `sim/winport/remote-port-sk.c` and
`sim/winport/safeio.c` are derived from libsystemctlm-soc (MIT); see [NOTICE](NOTICE).
QEMU, the co-sim device tree and the Xilinx tools are **not** included and remain
under their own licenses.
