# Copyright 2026 The zynq-qemu-xsim-cosim authors
# SPDX-License-Identifier: Apache-2.0
# run_cosim_win.ps1 - QEMU (Cortex-A9 bare metal) <-> XSim (my_math_ip) co-simulation, native Windows.
# Needs Vivado 2022.1 (xvlog/xsc/xelab/xsim) and Vitis 2022.1 (Xilinx QEMU + co-sim DTB).
#   powershell -ExecutionPolicy Bypass -File run_cosim_win.ps1
#   powershell -ExecutionPolicy Bypass -File run_cosim_win.ps1 -Gui
param(
    [string]$XilinxRoot = 'C:\Xilinx',
    [string]$Ver        = '2022.1',
    [int]   $Port       = 9750,
    [int]   $QuantumNs  = 1000000,
    [int]   $TimeoutS   = 600,
    [switch]$Gui,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'

$Here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$Lib    = Join-Path $Here 'third_party\libsystemctlm-soc'
$Elf    = Join-Path $Here 'out\math_test.elf'
$Run    = Join-Path $Here 'build\cosim_win'
$Vivado = Join-Path $XilinxRoot "Vivado\$Ver"
$Emu    = Join-Path $XilinxRoot "Vitis\$Ver\data\emulation"
$Qemu   = Join-Path $Emu 'qemu_win\qemu\qemu-system-aarch64.exe'
$HwDtb  = Join-Path $Emu 'dtbs\zynq\zynq-arm-cosim.dtb'

function Die($m) { Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }
foreach ($p in @($Elf, $Qemu, $HwDtb, (Join-Path $Vivado 'settings64.bat'))) {
    if (-not (Test-Path $p)) { Die "missing $p" }
}
if (-not (Test-Path $Lib)) {
    git clone --depth 1 https://github.com/Xilinx/libsystemctlm-soc $Lib
}

New-Item -ItemType Directory -Force $Run | Out-Null
$uart = Join-Path $Run 'uart1.log'

# forward slashes: these go to gcc via xsc
$l  = $Lib  -replace '\\','/'

if (-not $SkipBuild) {
    # remote-port-proto.c is portable; sk/safeio are the winsock replacements in sim\winport
    $src = @("$Here\sim\winport\rp_dpi.c",
             "$Lib\libremote-port\remote-port-proto.c",
             "$Here\sim\winport\remote-port-sk.c",
             "$Here\sim\winport\safeio.c",
             "$Here\sim\winport\wsdyn.c") | ForEach-Object { '"' + $_ + '"' }
    $build = @"
@echo off
call "$Vivado\settings64.bat"
cd /d "$Run"
rmdir /s /q xsim.dir 2>nul
call xvlog "$Here\rtl\my_math_ip.v" || exit /b 1
call xvlog -sv "$Here\sim\cosim_top.sv" || exit /b 1
rem winsock is bound at run time (sim\winport\wsdyn.c): static ws2_32 imports stop xsimk.exe from starting
call xsc --gcc_compile_options "-I$l/libremote-port" $($src -join ' ') || exit /b 2
call xelab -debug typical -timescale 1ns/1ps -L work work.cosim_top -sv_root xsim.dir/work/xsc -sv_lib dpi -s cosim_snap || exit /b 3
"@
    Set-Content (Join-Path $Run 'build.bat') $build -Encoding ascii
    Write-Host 'Building XSim snapshot...'
    # redirect inside cmd: PowerShell 5.1 turns redirected native stderr into errors
    cmd /c "`"$Run\build.bat`" > `"$Run\build.log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Die "build failed (exit $LASTEXITCODE); see $Run\build.log" }
}

# ---- QEMU: PL remote-port over a TCP chardev. Windows QEMU has no unix sockets, and
# ---- the co-sim DTB has no chrdev-id, so the remote-port device is given the chardev
# ---- via -global. wait=on makes QEMU block until XSim connects.
Remove-Item $uart -ErrorAction SilentlyContinue
$qargs = @('-M','arm-generic-fdt-7series','-display','none',
           '-hw-dtb',$HwDtb,
           '-serial','null','-serial',"file:$uart",
           '-device',"loader,file=$Elf,cpu-num=0",
           '-chardev',"socket,id=pl-rp,host=127.0.0.1,port=$Port,server=on,wait=on",
           '-global','remote-port.chardev=pl-rp',
           '-sync-quantum',"$QuantumNs")
$q = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $Run 'qemu.out.log') -RedirectStandardError (Join-Path $Run 'qemu.err.log')
Start-Sleep -Seconds 2
if ($q.HasExited) { Get-Content (Join-Path $Run 'qemu.err.log'); Die 'QEMU exited early' }

# ---- XSim: runs until QEMU closes the socket (cosim_top then calls $finish) ----
$tcl = if ($Gui) { "log_wave -recursive /cosim_top`nadd_wave -recursive /cosim_top`nrun all" }
       else      { "log_wave -recursive /cosim_top`nrun all`nquit" }
Set-Content (Join-Path $Run 'wave.tcl') $tcl -Encoding ascii
$guiArg = if ($Gui) { '-gui' } else { '' }
$sim = @"
@echo off
call "$Vivado\settings64.bat"
cd /d "$Run"
set RP_SOCKET=tcp:127.0.0.1:$Port
xsim cosim_snap $guiArg -tclbatch wave.tcl -wdb cosim.wdb
"@
Set-Content (Join-Path $Run 'sim.bat') $sim -Encoding ascii
Write-Host 'Running co-simulation...'
$x = Start-Process cmd -ArgumentList '/c', "`"$Run\sim.bat`"" -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $Run 'xsim.out.log') -RedirectStandardError (Join-Path $Run 'xsim.err.log')

# Wait for the firmware end marker, then stop QEMU so XSim sees EOF and exits cleanly.
$deadline = (Get-Date).AddSeconds($TimeoutS)
while ((Get-Date) -lt $deadline -and -not $x.HasExited -and -not $q.HasExited) {
    if ((Test-Path $uart) -and (Select-String -Path $uart -Pattern 'COSIM_DONE' -Quiet)) { break }
    Start-Sleep -Milliseconds 500
}
Start-Sleep -Seconds 1
if (-not $q.HasExited) { Stop-Process -Id $q.Id -Force -ErrorAction SilentlyContinue }
if (-not $Gui) {
    $x.WaitForExit(60000) | Out-Null
    if (-not $x.HasExited) { Write-Warning 'xsim did not exit'; Stop-Process -Id $x.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host '---- UART1 ----'
if (Test-Path $uart) { Get-Content $uart } else { Write-Host '(no UART output)' }
Write-Host '---- XSim ----'
Get-Content (Join-Path $Run 'xsim.out.log') -ErrorAction SilentlyContinue | Select-String 'cosim_top' | ForEach-Object { $_.Line }
if ((Test-Path $uart) -and (Select-String -Path $uart -Pattern 'RESULT: ALL PASS' -Quiet)) {
    Write-Host "CO-SIM PASS (waveforms: $Run\cosim.wdb)"; exit 0
}
Write-Host "CO-SIM FAIL or timed out (see $Run\qemu.err.log, $Run\xsim.out.log)"; exit 1
