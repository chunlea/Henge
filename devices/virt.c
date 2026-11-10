#include "virt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static uint64_t pio_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque, (void)off, (void)size;
    return ~0ull;
}

static void pio_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)opaque, (void)off, (void)v, (void)size;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* etc/ramfb was written: {u64 address, u32 fourcc, u32 flags, u32 width, u32 height, u32 stride}, big-endian.
   Called with the device lock held, which is what a reader of the fb_ fields takes (host/vm.c). */
static void ramfb_written(void *opaque) {
    HvVmapple *b = opaque;
    const uint8_t *c = hv_fw_cfg_find(b->fwcfg, "etc/ramfb")->data;
    uint64_t addr = (uint64_t)be32(c) << 32 | be32(c + 4);
    uint32_t fourcc = be32(c + 8), width = be32(c + 16), height = be32(c + 20), stride = be32(c + 24);
    if (!stride) stride = width * 4;
    uint64_t size = (uint64_t)stride * height;
    b->fb_size = 0;
    /* XRGB8888 ("XR24") is the only format the firmware asks for. */
    if (fourcc != 0x34325258 || !width || !height || width > 8192 || height > 8192 || stride < width * 4 ||
        addr < b->mem_base || addr - b->mem_base + size > b->ram_size) {
        if (b->m->log) fprintf(stderr, "hengevm: ramfb: %ux%u at %llx (format %08x) cannot be shown\n", width, height,
                               (unsigned long long)addr, fourcc);
        return;
    }
    b->fb_base = addr, b->fb_width = width, b->fb_height = height, b->fb_stride = stride, b->fb_size = size;
    if (b->m->log) fprintf(stderr, "hengevm: ramfb: %ux%u at %llx\n", width, height, (unsigned long long)addr);
}

/* The display's size. The firmware's display driver (EDK2's QemuRamfbDxe) offers the sizes in a table of its
   own, 640 x 480 to 1024 x 768, and Windows, with no display driver for this machine, draws for good in the
   one its boot manager is handed. There is no asking the driver for another size, so its table is made to
   say the one size wanted: the driver looks for etc/ramfb in fw_cfg's directory as it starts, before it has
   read the table (it sizes its framebuffer from it), and that is when the table, in the driver's image in
   the guest's memory, is found and rewritten. Each entry is an EFI_GRAPHICS_OUTPUT_MODE_INFORMATION: 36
   bytes, a version of 0, then the width and the height. */
static void ramfb_sizes(void *opaque) {
    HvVmapple *b = opaque;
    if (b->screen_set || !b->screen_width || !b->screen_height) return;
    enum { ENTRY = 36 };
    static const uint32_t first[3] = {0, 640, 480}, second[3] = {0, 800, 600};
    /* The firmware loads its drivers at the top of memory: the last gigabyte is where to look. */
    uint64_t span = b->ram_size < (1ull << 30) ? b->ram_size : 1ull << 30;
    uint8_t *from = b->ram + (b->ram_size - span), *end = b->ram + b->ram_size - 64 * ENTRY;
    for (uint8_t *p = from; p < end && (p = memmem(p, (size_t)(end - p), first, sizeof first)); p += 4) {
        if (memcmp(p + ENTRY, second, sizeof second)) continue;
        int n = 0;
        for (; n < 64; n++) {
            uint32_t e[3];
            memcpy(e, p + n * ENTRY, sizeof e);
            if (e[0] || e[1] < 320 || e[1] > 8192 || e[2] < 200 || e[2] > 8192) break;
            memcpy(p + n * ENTRY + 4, &b->screen_width, 4);
            memcpy(p + n * ENTRY + 8, &b->screen_height, 4);
        }
        b->screen_set = 1;
        if (b->m->log) fprintf(stderr, "hengevm: ramfb: the firmware's %d display sizes are all %ux%u now\n", n, b->screen_width, b->screen_height);
        return;
    }
}

/* An interrupt is being taken over and over (devices/gicv3.c): what its device looks like. */
static void storm(void *opaque, int intid) {
    HvVmapple *b = opaque;
    static int said;
    if (said++ >= 3) return;
    if (b->nvme && intid == 32 + VIRT_IRQ_PCIE + b->nvme->pci.slot) hv_nvme_dump(b->nvme);
}

/* EDK2's driver for virtio 1.0 devices takes only those whose PCI subsystem id is 0x40 or more, as the
   specification says of devices that are not transitional. QEMU's have 0x1100; the virtual Mac's keep the
   device type there, which is what macOS has always been shown. */
static void standard_ids(HvVirtio *v) {
    if (v) v->pci.config[0x2e] = 0x00, v->pci.config[0x2f] = 0x11;
}

int hv_virt_create(HvVmapple *b, HvMachine *m, HvCpu *cpus, const HvVirtParams *p) {
    memset(b, 0, sizeof *b);
    b->ram_fd = -1;
    b->m = m;
    b->cpu = cpus;
    b->ncpus = p->ncpus < 1 ? 1 : p->ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : p->ncpus;
    b->ram_size = p->ram_size;
    b->mem_base = VIRT_MEM;
    if (hv_vmapple_alloc_ram(b) != 0) return -1;
    b->fw_code = mmap(NULL, VIRT_FLASH_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b->fw_code == MAP_FAILED) return -1;
    hv_machine_add_ram(m, VIRT_FLASH_CODE, VIRT_FLASH_SIZE, b->fw_code, 1);
    hv_machine_add_ram(m, VIRT_MEM, b->ram_size, b->ram, 0);
    if (!(b->vars = hv_pflash_create(m, VIRT_FLASH_VARS, VIRT_FLASH_SIZE, p->vars_path))) return -1;

    b->gic = hv_gicv3_create(m, VIRT_GIC_DIST, VIRT_GIC_REDIST);
    b->gic->storm = storm, b->gic->storm_opaque = b;
    b->uart = hv_pl011_create(m, b->gic, VIRT_UART, 32 + VIRT_IRQ_UART);
    b->rtc = hv_pl031_create(m, b->gic, VIRT_RTC, 32 + VIRT_IRQ_RTC);
    b->fwcfg = hv_fw_cfg_create(m, VIRT_FW_CFG, b->ncpus);
    hv_fw_cfg_add_file(b->fwcfg, "etc/ramfb", NULL, 28, ramfb_written, b);
    b->screen_width = p->screen_width, b->screen_height = p->screen_height;
    b->fwcfg->dir_selected = ramfb_sizes, b->fwcfg->dir_opaque = b;
    b->pcie = hv_pcie_create(m, b->gic, VIRT_PCIE_ECAM, VIRT_PCIE_ECAM_SIZE, VIRT_PCIE_MMIO, VIRT_PCIE_MMIO_SIZE,
                             32 + VIRT_IRQ_PCIE);
    hv_pcie_add_window(b->pcie, VIRT_PCIE_MMIO64, VIRT_PCIE_MMIO64_SIZE);
    hv_machine_add_mmio(m, VIRT_PCIE_PIO, 0x10000, b, pio_read, pio_write, "pcie-pio");
    /* HV_VIRT_DISK=virtio: the disk as a virtio one instead (debugging). */
    const char *kind = getenv("HV_VIRT_DISK");
    if (p->disk && kind && !strcmp(kind, "virtio")) {
        if (!(b->root_blk = hv_virtio_blk_create(m, b->pcie, p->disk, HV_VBLK_PLAIN))) return -1;
    } else if (p->disk && !(b->nvme = hv_nvme_create(m, b->pcie, p->disk))) return -1;
    /* Keyboard, pointer and installation media are USB devices, which Windows has drivers for.
       HV_VIRT_CD=virtio: the media as a virtio disk instead (the firmware can start from it; Windows cannot read it). */
    const char *cd = getenv("HV_VIRT_CD");
    int virtio_cd = cd && !strcmp(cd, "virtio");
    if (p->cdrom && virtio_cd && !(b->aux_blk = hv_virtio_blk_create(m, b->pcie, p->cdrom, HV_VBLK_PLAIN))) return -1;
    /* The network card is a USB one too (CDC NCM). HV_VIRT_NET=virtio: a virtio card instead. */
    const char *nk = getenv("HV_VIRT_NET");
    int virtio_net = getenv("HV_NO_XHCI") || (nk && !strcmp(nk, "virtio"));
    if (!getenv("HV_NO_XHCI"))
        b->usb = hv_xhci_create_pc(m, b->pcie, virtio_cd ? NULL : p->cdrom, virtio_net ? NULL : p->mac);
    if (p->mac && virtio_net) b->net = hv_virtio_net_create(m, b->pcie, p->mac);
    b->usb_net = p->mac && !virtio_net && b->usb;
    if (!b->usb) { /* HV_NO_XHCI: virtio input, which the firmware and Linux can use */
        b->keyboard = hv_virtio_input_create(m, b->pcie, HV_VINPUT_KEYBOARD);
        b->mouse = hv_virtio_input_create(m, b->pcie, HV_VINPUT_MOUSE);
    }

    /* A GPU beside the firmware's framebuffer: Windows draws in the framebuffer until its driver for this one
       (viogpu3d) is installed and takes the display. */
    if (p->gpu_dir) {
        b->gpu = hv_virtio_gpu_create_d3d(m, b->pcie, p->screen_width ? p->screen_width : 1280, p->screen_height ? p->screen_height : 800, p->gpu_dir);
        if (b->gpu) standard_ids(&b->gpu->vio);
    }
    standard_ids(b->root_blk ? &b->root_blk->vio : NULL);
    standard_ids(b->aux_blk ? &b->aux_blk->vio : NULL);
    standard_ids(b->net ? &b->net->vio : NULL);
    standard_ids(b->keyboard ? &b->keyboard->vio : NULL);
    standard_ids(b->mouse ? &b->mouse->vio : NULL);

    m->tick = hv_vmapple_tick;
    m->tick_opaque = b;
    cpus->m = m;
    return 0;
}

int hv_virt_load_firmware(HvVmapple *b, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(b->fw_code, 1, VIRT_FLASH_SIZE, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    return n == 0 || more ? -1 : 0;
}

void hv_virt_reset_cpu(HvVmapple *b) {
    for (int i = 0; i < b->ncpus; i++) {
        HvCpu *c = &b->cpu[i];
        hv_cpu_reset_index(c, b->m, (uint32_t)i);
        c->mpidr = 0x80000000u | (uint32_t)i;
        c->pc = VIRT_FLASH_CODE;
        c->off = i != 0;
    }
}
