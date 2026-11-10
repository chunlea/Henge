# Windows on Arm

Windows 11 for arm64 runs on the same engine as the virtual Mac: nothing is virtualized
by the processor, so it can run where apps have no hypervisor. What differs is the
machine around the CPU, because Windows does not start from a device tree or from
Apple's boot ROM: it starts from UEFI firmware and reads ACPI tables.

## The board

`devices/virt.c` builds a second board, in the same `HvVmapple` structure with the
Apple-only devices left out. Its memory map is that of QEMU's `virt` machine (with
`highmem` off), because that is the machine the firmware is built for:

| Address | What |
| --- | --- |
| `0x00000000` | the firmware, 64 MB of read-only flash |
| `0x04000000` | the firmware's variables: a flash chip (`devices/pflash.c`) kept in a file |
| `0x08000000` | GICv3 distributor; redistributors at `0x080a0000` |
| `0x09000000` | PL011 serial port; PL031 clock at `0x09010000` |
| `0x09020000` | fw_cfg (`devices/fw_cfg.c`) |
| `0x10000000` | PCI memory window; ECAM at `0x3f000000` |
| `0x40000000` | RAM |

The firmware is EDK2's ArmVirtQemu, unchanged: the `edk2-aarch64-code.fd` that QEMU
ships. It reads the devices from a device tree at the start of RAM
(`host/uefi_boot.c`), and is handed ACPI tables over fw_cfg the way QEMU hands over its
own (`host/acpi.c`): DSDT, FADT, MADT, GTDT, MCFG, SPCR and DBG2. With tables there, it
stops offering the device tree, and Windows (or Linux) sees ACPI only.

## Devices Windows has drivers for

Windows has no virtio drivers out of the box, so the board gives it what it does have:

- **Disk**: an NVMe controller (`devices/nvme.c`), which the firmware starts from too.
- **Keyboard and pointer**: USB devices on the xHCI controller (`devices/xhci.c`). The
  pointer is a tablet, which reports where it is on the screen: no mouse to capture.
- **Installation media**: a USB CD-ROM drive (mass storage, bulk-only, SCSI) holding the
  ISO. The firmware starts from it, and Setup reads its files from it.
- **Display**: QEMU's ramfb. The firmware's driver writes where its picture is to the
  fw_cfg file `etc/ramfb`, and Windows goes on drawing in the same memory through the
  framebuffer UEFI's Graphics Output Protocol hands it. 800x600 until there is a display
  driver.
- **Interrupts**: PCI interrupt pins, one SPI a slot. No MSI: the board has no ITS.

The xHCI controller has two sets of devices. The virtual Mac's keyboard and trackpad
work as they always have. The UEFI board's work a transfer descriptor at a time,
because Windows' driver takes its results from Event Data TRBs and spreads one buffer
over several chained TRBs.

## What Windows asked of the CPU

- `PMCCNTR_EL0` has to count. Windows' boot manager and loader time themselves with the
  PMU's cycle counter whatever `ID_AA64DFR0` says, and its programs read it at EL0.
- `LDAR` and `STLR` off their natural alignment, as the host's CPU allows (FEAT_LSE2):
  the clock interrupt stores the time with an `STLR` at offset 0x14 of a page.
- A TLBI by VA in a block mapping drops every entry made from the block, in the kernel
  half as well as the user half. The TLB holds 4 KB pieces of 2 MB blocks; Windows maps
  much of its kernel memory in blocks and takes single ones apart with one TLBI each.
  Left alone, the other pieces went on being written through, and Windows stopped with
  `KERNEL_MODE_HEAP_CORRUPTION` in about one boot in five.
- An xHCI event's cycle bit is written after the rest of the event. Windows reads the
  event ring on other CPUs while it is being written, and an event's parameter is a
  pointer of its own.

Windows switches both `TTBR0_EL1` and `TTBR1_EL1` for each process, with the address
space ID in `TTBR1` (`TCR_EL1.A1`). The engine empties its TLB on each such switch,
which is right and slow. It is the first thing to look at for speed.

## Running it

```
hengevm --uefi /opt/homebrew/share/qemu/edk2-aarch64-code.fd --uefi-vars vars.fd \
        --disk windows.img --cdrom Windows11_arm64.iso --ram 6144 --cpus 4
```

`windows.img` is a raw file (`truncate -s 64g windows.img`). `HV_FB_SHOT=shot.png`
writes the display to a PNG every five seconds, and `HV_INPUT_FILE=input.txt` takes
`click X Y`, `type TEXT`, `key CODE` and `hidkey USAGE` lines, which is how an install
can be driven without a window.

Setup refuses a machine without a TPM and Secure Boot. Shift+F10 at its first page opens
a command prompt, where these let it go on:

```
reg add HKLM\SYSTEM\Setup\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f
reg add HKLM\SYSTEM\Setup\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f
```

For debugging: `HV_VIRT_DISK=virtio` and `HV_VIRT_CD=virtio` put the disk and the media
on virtio instead (the firmware and Linux can use them, Windows cannot), `HV_NO_ACPI`
leaves the device tree as the only description, `HV_XHCI_LOG` logs the USB controller,
and `HV_BUGCHECK=0x13a` looks for Windows' record of a stop with that code in guest
memory when the machine stops.

Linux starts on this board as well, which is a quick way to check a device: from the
UEFI shell, `fs0:\path\vmlinuz initrd=\path\initramfs root=... console=ttyAMA0`.

## Where it stands

Windows 11 Pro 24H2 (build 26100) installs from Microsoft's ISO and runs to its desktop
with four CPUs: Setup, both of its restarts, the first-run pages (with
`oobe\bypassnro` from Shift+F10, there being no network) and a local account. The
first stage takes about seven minutes on an M-series Mac, the second about ten.

Two things the NVMe controller had to get right for that: reads and writes are a worker
thread's (a guest CPU that waits for the host's disk in a DPC is a
`DPC_WATCHDOG_VIOLATION`), and a completion waits when its queue is full. Windows makes
eight submission queues of 1024 entries that all complete into one queue of 1024, and
leaves it to the controller to hold completions back, as the specification says.

## Not there yet

- The iOS app does not offer Windows. Everything above is the Mac's command-line tool.
- No network: Windows has no driver for the virtio card.
- No sound, no display driver (so one resolution, drawn by the CPU), no clipboard or
  shared folder.
- A restart ends `hengevm`: it has to be started again (a shell loop does). The
  firmware then starts Windows Boot Manager from the disk, whose entry Windows wrote to
  the firmware's variables.
- Seen once and not explained: an interrupt storm on the NVMe line a third of the way
  into Setup's second stage, after the fix above. `hengevm` names the interrupt and
  prints the controller's state when it happens (`hv_nvme_dump`).
- Seen once under heavy host load: `hengevm` processes that did not exit on SIGTERM.
