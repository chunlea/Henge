#include "henge-vm.h"
#include "henge-ios-jit.h"
#include "install.h"
#include "jit_region.h"
#include "vm.h"

#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Real native bridge and device workers; only the debugger allocation and
   unused installer entry point are replaced. No Apple ROM or disk needed. */
static HvJitRegion jit;
static HengeDualMapping mapping;
static int failures;
static _Atomic int reader_done;
HengeDualMapping *henge_ios_jit_get_mapping(void) { return &mapping; }
void hv_prof_ios_start(void) {}
int hv_install_start(HvVirtioUsb *usb, const char *ipsw, const char *work, uint64_t ecid,
                     HvInstallProgress progress, void *user) {
    (void)usb; (void)ipsw; (void)work; (void)ecid; (void)progress; (void)user;
    return -1;
}
static void check(const char *what, int ok) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}
static int fds(void) {
    int n = 0;
    for (int i = 0; i < 2048; i++) if (fcntl(i, F_GETFD) != -1) n++;
    return n;
}
static unsigned threads(void) {
    thread_act_array_t list;
    mach_msg_type_number_t n;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return 0;
    for (unsigned i = 0; i < n; i++) mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof *list);
    return n;
}
static unsigned dead_ports(void) {
    mach_port_name_array_t names;
    mach_port_type_array_t types;
    mach_msg_type_number_t n, nt;
    if (mach_port_names(mach_task_self(), &names, &n, &types, &nt) != KERN_SUCCESS) exit(1);
    unsigned dead = 0;
    for (unsigned i = 0; i < nt; i++) if (types[i] & MACH_PORT_TYPE_DEAD_NAME) dead++;
    vm_deallocate(mach_task_self(), (vm_address_t)names, n * sizeof *names);
    vm_deallocate(mach_task_self(), (vm_address_t)types, nt * sizeof *types);
    return dead;
}
static void write_file(const char *path, const void *data, size_t bytes) {
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0 || (data && write(fd, data, bytes) != (ssize_t)bytes) || ftruncate(fd, bytes)) exit(1);
    close(fd);
}
static void *read_ui(void *unused) {
    (void)unused;
    while (!atomic_load(&reader_done)) {
        uint8_t image[16];
        int x, y, width, height, stride;
        const uint8_t *pixels;
        henge_vm_snapshot(image, 2, 2);
        henge_vm_pointer(&x, &y);
        henge_vm_cpus_on();
        henge_vm_gpu_frames();
        henge_vm_balloon(-1);
        henge_vm_set_display_layer(NULL);
        if (!henge_vm_boot_display(&pixels, &width, &height, &stride)) henge_vm_free_display(pixels);
        usleep(500);
    }
    return NULL;
}
int main(void) {
    alarm(60);
    char dir[] = "/tmp/henge-lifecycle-XXXXXX", rom[256], disk[256], aux[256], ram[256], error[256];
    if (!mkdtemp(dir) || hv_jit_region_open(&jit, 8 << 20)) return 1;
    snprintf(rom, sizeof rom, "%s/rom", dir);
    snprintf(disk, sizeof disk, "%s/disk", dir);
    snprintf(aux, sizeof aux, "%s/aux", dir);
    snprintf(ram, sizeof ram, "%s/ram", dir);
    setenv("HV_RAM_FILE", ram, 1);
    setenv("HV_RAM_NO_LEDGER", "1", 1);
    setenv("HV_BALLOON", "0", 1);
    write_file(disk, NULL, 1 << 20);
    write_file(aux, NULL, 1 << 20);
    mapping = (HengeDualMapping){.rx_addr = (uint8_t *)jit.base,
        .rw_addr = (uint8_t *)jit.base + jit.rw_diff, .size = jit.capacity * 4};
    pthread_t reader;
    if (pthread_create(&reader, NULL, read_ui, NULL)) return 1;
    /* Warm the process-wide pointer queue before counting worker threads. */
    hv_vm_mouse_move(0, 0);
    usleep(100000);
    int initial_fds = fds();
    unsigned initial_threads = threads();
    unsigned initial_dead_ports = dead_ports();
    /* Missing ROM fails after disks, RAM and device workers were created. */
    for (int i = 0; i < 3; i++) {
        check("failed boot reports an error", henge_vm_boot(rom, disk, aux, 1, 0, 64, 4, error, sizeof error) != 0);
        check("failed boot allows retry", henge_vm_state() == 0 && henge_vm_can_start());
        check("failed boot closes files", fds() == initial_fds);
        check("failed boot joins workers", threads() == initial_threads);
    }
    for (int i = 0; i < 12; i++) {
        int outcome = i % 3 == 2 ? HENGE_VM_OUTCOME_HOST_STOP : i % 3 == 1 ? HENGE_VM_OUTCOME_RESTART : HENGE_VM_OUTCOME_SHUTDOWN;
        uint32_t code[] = {outcome == HENGE_VM_OUTCOME_RESTART ? 0xd2800120 : 0xd2800100, 0xf2b08000, 0xd4000002};
        if (outcome == HENGE_VM_OUTCOME_HOST_STOP) code[0] = 0x14000000;
        write_file(rom, code, sizeof code);
        int rc = henge_vm_boot(rom, disk, aux, 1, 0, 64, 4, error, sizeof error);
        if (rc) fprintf(stderr, "boot %d: %s\n", i, error);
        check("boot succeeds without restarting app", rc == 0);
        if (rc) break;
        if (outcome == HENGE_VM_OUTCOME_HOST_STOP) {
            henge_vm_mouse_button(0, 1);
            henge_vm_mouse_button(1, 1);
            henge_vm_mouse_move(5, 7);
            henge_vm_mouse_button(1, 0);
            henge_vm_stop();
        }
        for (int n = 0; n < 3000 && henge_vm_state() != 2; n++) usleep(1000);
        check("terminal state is published after cleanup", henge_vm_state() == 2);
        if (henge_vm_state() != 2) exit(1);
        check("power outcome preserved", henge_vm_exit_outcome() == outcome);
        check("start is available again", henge_vm_can_start());
        check("new generation", henge_vm_generation() == (uint64_t)i + 1);
        check("RAM released", hv_vm_board()->ram == NULL);
        check("RAM file closed", hv_vm_board()->ram_fd == -1);
        check("balloon released", hv_vm_board()->balloon == NULL);
        check("translation metadata released", hv_vm_machine()->blocks == NULL && hv_vm_machine()->jit.base == NULL);
        check("borrowed JIT mapping retained", mapping.rx_addr == (uint8_t *)jit.base);
        check("no disk or network descriptor leak", fds() == initial_fds);
        /* The last CPU thread can still be returning from its callback. */
        for (int n = 0; n < 1000 && threads() > initial_threads; n++) usleep(1000);
        check("no CPU, disk or network worker leak", threads() == initial_threads);
        check("no leaked CPU thread ports", dead_ports() == initial_dead_ports);
    }
    hv_vm_destroy();
    atomic_store(&reader_done, 1);
    pthread_join(reader, NULL);
    hv_jit_region_close(&jit);
    unlink(rom); unlink(disk); unlink(aux); rmdir(dir);
    printf("VM lifecycle: 12 power cycles and 3 failed boots, %d failures\n", failures);
    return failures != 0;
}
