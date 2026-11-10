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
    b->uart = hv_pl011_create(m, b->gic, VIRT_UART, 32 + VIRT_IRQ_UART);
    b->rtc = hv_pl031_create(m, b->gic, VIRT_RTC, 32 + VIRT_IRQ_RTC);
    b->fwcfg = hv_fw_cfg_create(m, VIRT_FW_CFG, b->ncpus);
    b->pcie = hv_pcie_create(m, b->gic, VIRT_PCIE_ECAM, VIRT_PCIE_ECAM_SIZE, VIRT_PCIE_MMIO, VIRT_PCIE_MMIO_SIZE,
                             32 + VIRT_IRQ_PCIE);
    hv_machine_add_mmio(m, VIRT_PCIE_PIO, 0x10000, b, pio_read, pio_write, "pcie-pio");
    if (p->disk && !(b->root_blk = hv_virtio_blk_create(m, b->pcie, p->disk, HV_VBLK_PLAIN))) return -1;
    if (p->mac) b->net = hv_virtio_net_create(m, b->pcie, p->mac);
    b->keyboard = hv_virtio_input_create(m, b->pcie, HV_VINPUT_KEYBOARD);
    b->mouse = hv_virtio_input_create(m, b->pcie, HV_VINPUT_MOUSE);

    standard_ids(b->root_blk ? &b->root_blk->vio : NULL);
    standard_ids(b->net ? &b->net->vio : NULL);
    standard_ids(&b->keyboard->vio);
    standard_ids(&b->mouse->vio);

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
