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

## The display's size

Windows has no driver for a display here: it draws in the framebuffer the firmware's Graphics Output
Protocol gave its boot manager, and offers in its Settings that framebuffer's size and smaller ones
(which it stretches). The firmware's driver, QemuRamfbDxe, has a table of three sizes, 640x480 to
1024x768, and the boot manager keeps the one the firmware was using, 800x600.

So the table is rewritten (`ramfb_sizes` in `devices/virt.c`): `--screen 1440x1000`, or the machine's
screen in the iOS app, becomes every entry of it. The driver looks for `etc/ramfb` in fw_cfg's
directory as it starts, before it reads its table to size the framebuffer, and that is when the
table is found in the guest's memory (the driver's image, loaded near the top of RAM) and changed.
The firmware's file is as QEMU ships it. A Windows installed at one size keeps it until the new one
is chosen in Settings, System, Display; the pointer is only where the finger says when Windows
fills the framebuffer.

## The GPU

`hengevm --gpu DIR` (the folder of GPU frameworks, `ios/deps/virgl/macos`; the iOS app's Windows machines
have it unless switched off) adds a virtio-gpu device for Triton, UTM's Direct3D 11 driver for Windows on
Arm (`viogpu3d`, github.com/osy/kvm-guest-drivers-windows; signed by Microsoft's hardware program, so
Windows loads it as it is). The driver forwards Direct3D calls over virtio in UTM's Neptune protocol;
virglrenderer (UTM's fork, the one Linux guests' 3D already uses) carries them out with dxmt-native,
Direct3D 11 on Metal, in its render server: a process of its own on a Mac (UTM's `QEMURenderServer`, signed
again without its sandbox by `ios/scripts/fetch_virgl.sh`), a thread on iOS. `dwm.exe` then composes
the desktop on the host's GPU, and Direct3D 11 programs draw with it.

Touch is a USB touch screen beside the tablet (`HV_USB_SCREEN` in `devices/xhci.c`): a HID digitizer of five
fingers (tip switch, contact number, X and Y across the display, contact count, scan time, and the feature
report for the most contacts), which Windows takes with what it comes with. The iOS app lays a surface over
the machine's picture (`TouchScreen`, in Trackpad.swift) and reports each finger where it is; a machine's
settings can make the picture a trackpad's surface instead. `finger ID 1|0 X Y` in hengevm's input file (X, Y
in thousandths of the display) is a finger for testing. `HV_NO_TOUCHSCREEN` leaves the device out.

Sound is an Intel HD Audio controller (`devices/hda.c`: ICH6's IDs as QEMU's intel-hda, one codec with a
stereo output, 16 bits at 44.1 or 48 kHz), which Windows drives with what it comes with (hdaudbus and the HD
Audio class driver). Windows makes the stream's buffer 20 ms and keeps it full by its own clock, so the
stream moves as a sound card's does: at its rate by the clock exactly, a millisecond at a time (the board
ticks every millisecond while one runs), its sound taken a millisecond ahead of the position into a ring that
holds 60 ms for the host's output. Taken in the output's lumps, or behind the position, or at a rate bent to
suit the output, it was last lap's sound or the next one's (a robot's voice); the output's own clock drift
is absorbed in the ring. Windows reads positions from the registers' second copy at 0x2000, which has to be
there. `HV_HDA_DUMP=file` keeps what the output was given, which is how this was measured (a 440 Hz
`[console]::beep`, its periods and jumps counted).
`HV_HDA_LOG=1` says when sound starts and stops and how loud it was; 2 and 3 add the registers' traffic.
`HV_NO_SOUND` leaves the card out.

The display's size with the GPU. Windows starts in the size it last had on the monitor, or 1024 x 768 where
that one is gone; UTM's driver takes its preferred mode from the firmware's linear framebuffer, which EDK2's
picture on a virtio GPU does not have. The size wanted (`--screen`, the iOS machine's setting, a resized
window) is applied by the driver's helper in the user's session (`viogpuap.exe`) when a display event comes,
so the device repeats the event from when the driver first draws until the picture has that size
(`ask_size`, `devices/virtio_gpu.c`). For the firmware to draw on the device at all (the picture before
Windows' driver starts, at the size wanted: `firmware_sizes`), the device is laid out as QEMU's: subsystem ID
0x1100, registers in BAR 2 and the shared memory in BAR 4. EDK2 numbers BARs in the order found, a 64-bit one
counting once, and found no registers the other way round.

On iOS dxmt is held to feature level 11_0 (`DXMT_CONFIG=d3d11.maxFeatureLevel=11_0`, set by the device). Its
iOS build has no logic operations in the output merger, which 11_1 requires: offered an 11_1 device without
them, `dwm.exe` made its device, asked (`CheckFeatureSupport`), and threw it away, again and again, and the
screen flashed. `HV_NPT_DUMP=DIR` is how that was found: it keeps each context's command ring and replies,
which decode against virglrenderer's `src/neptune/neptune-protocol` headers.

What the device (`devices/virtio_gpu_virgl.c`, `hv_virtio_gpu_create_d3d`) has for that, beside what Linux's
OpenGL uses: blob resources, contexts made for a protocol, fences on a context's own timelines, an EDID, and
a host-visible shared memory region (a 64-bit BAR in a second PCI window above RAM, `VIRT_PCIE_MMIO64`)
that the driver has host blobs mapped into. The driver places a blob at any multiple of the guest's 4 KB
pages and the host's pages are 16 KB, so the region is a paged RAM region of the engine (`core/machine.h`):
each 4 KB page has its own place in this process, inside virglrenderer's mapping of the blob. Taking a blob
out has every CPU empty its TLB before the mapping goes. The picture is a Metal texture virglrenderer hands
over for the scanout. Where the texture is on an IOSurface the surface itself is what the host shows
(`hv_gpu_virgl_surface`, the layer Linux guests' frames go to in the iOS app), with nothing copied; otherwise
it is read into the device's display buffer at each flush. Windows flips between a few blobs and sets the
scanout at every flip, so a blob keeps its texture from one frame to the next.

The host can ask for another size (`hv_vm_display_wanted`; `resize W H` in hengevm's input file; the iOS
session asks for the machine's width in the shape of the room it has): the device raises its display event,
the driver asks again for the display's information and adds that size to what Windows offers
(Settings, System, Display), where choosing it changes the picture at once. The driver's helper in Windows
(vgpusrv, viogpuap) is meant to apply it by itself and runs, but did not here: not looked into. The driver
does not take the device's EDID feature, so 1024x768 is what Windows calls recommended.

Installing the driver, once, in Windows: UTM's guest tools disc as the CD-ROM
(`--cdrom utm-guest-tools-0.1.275.iso`; the iOS app downloads it in the machine's settings), then, in a
prompt run as administrator, `pnputil /add-driver D:\Drivers\viogpu3d\w11\ARM64\viogpu3d.inf /install`.
The screen changes over when the driver starts. From then on the machine is started with `--gpu-only 1`
(the app remembers): with the firmware's framebuffer still there Windows has two monitors, the
framebuffer one nobody sees, and its taskbar and Start menu come out wrong. Without it nothing shows
until the driver starts, about forty seconds into a start.

Two things the driver needed of virtio that nothing else here did: the config interrupt's vector reads back
what was written (it writes one with no MSI-X, and gives the device up otherwise), and a reset of the
device lets go of every resource and context.

Seen with driver v0.5 on a Mac: the desktop, Settings, Windows Terminal and the Start menu with its blur
are drawn; the Start menu's app names are not. Not yet tried on an iPad.

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
- Without the GPU's driver installed (below) Windows draws with the CPU, and the screen's size is fixed
  for a run.
- A restart ends `hengevm`: it has to be started again (a shell loop does). The
  firmware then starts Windows Boot Manager from the disk, whose entry Windows wrote to
  the firmware's variables.
- Rarely (two starts in about sixty, with ten or twelve machines starting at once, both on the engine
  merged with the Linux branch) Windows stops with a bug check soon after its desktop comes up, the
  kernel following a pointer that is not one, and restarts. Not explained.
- The network card's port is suspended by Windows when idle, and nothing wakes it from the host's
  side: what the guest starts gets through, a connection in to an idle guest would not.
- Seen once under heavy host load: `hengevm` processes that did not exit on SIGTERM.
