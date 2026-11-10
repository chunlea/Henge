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
- **Network**: a USB network card, a CDC NCM function on the same xHCI controller. Windows 11 has a
  driver for that class (UsbNcm), and Linux has one (cdc_ncm). Behind it is the user-mode network the
  other guests have: an address by DHCP (10.0.2.15), the host's DNS, connections out.
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

- A level interrupt that is taken again and again with its line never dropping waits a moment before
  each next time (`devices/gicv3.c`). Windows' NVMe driver takes a completion off its queue and writes
  the queue's head back a few instructions later; interrupted in between, on the CPU that takes the
  controller's interrupt, it has a handler that finds nothing new and an interrupt that stays up until
  those few instructions run. A real distributor takes long enough to signal again that they do. Here
  the interrupt came back the instant its handler ended, and the machine hung: at about one start in
  six, with the firmware's picture still on the screen, or later with an "interrupt storm" in the log.

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
`HV_VIRT_NET=virtio` makes the network card a virtio one, `HV_NET_LOG` logs each frame, `HV_GIC_LOG` logs an
interrupt Windows routes to another CPU (it does, as it runs), and `HV_BUGCHECK=0x13a` looks for Windows' record of a stop with that code in guest
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

## In the iOS app

The Windows tab makes a machine from an ISO chosen in Files (a blank sparse disk, with the ISO in the
CD-ROM drive until it is removed) or from a disk brought in: a raw image, or one packed with
`build/hvsparse pack` (the holes of a 64 GB image left out), which is how an image installed on a Mac
gets there. The firmware comes with the app, gzipped: `ios/scripts/fetch_uefi.sh` takes it from
Homebrew's QEMU before a build.

Windows has no driver for the Magic Trackpad the app's trackpad surface is to the other guests, so
for a UEFI guest the engine keeps the pointer itself (`host/vm.c`): one finger moves it, two scroll,
a mouse moves it, and the USB tablet reports where it is. A session with the disc in its drive
presses a key while the firmware starts, for the disc's "Press any key to boot from CD or DVD", and
its Details type what Setup needs: the LabConfig settings above, and `oobe\bypassnro`.

## Not there yet

- No sound, no clipboard or shared folder.
- No display driver, so Windows draws with the CPU in the framebuffer its boot manager chose and
  its Settings cannot change the size. The firmware offers 640x480, 800x600 and 1024x768, and the
  boot manager takes 800x600 unless told `bcdedit /set {globalsettings} highestmode on` (from an
  administrator's prompt; the iOS session's Details type it). More than that needs a display
  driver: virtio-win's viogpudo, on a virtio-gpu device, is the one to try.
- A restart ends `hengevm`: it has to be started again (a shell loop does). The
  firmware then starts Windows Boot Manager from the disk, whose entry Windows wrote to
  the firmware's variables.
- Rarely (two starts in about sixty, with ten or twelve machines starting at once, both on the engine
  merged with the Linux branch) Windows stops with a bug check soon after its desktop comes up, the
  kernel following a pointer that is not one, and restarts. Not explained.
- The network card's port is suspended by Windows when idle, and nothing wakes it from the host's
  side: what the guest starts gets through, a connection in to an idle guest would not.
- Seen once under heavy host load: `hengevm` processes that did not exit on SIGTERM.
