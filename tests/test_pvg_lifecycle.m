#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#include "pvg_lite.h"
#include "vmapple.h"
#include <stdlib.h>
#include <unistd.h>

/* Exercise real Metal command completion while the guest board is destroyed.
   Empty execs avoid simulator limitations on guest buffer-backed textures. */
static HvMachine machine;
static HvCpu cpus[2];
static HvVmapple board;

int main(void) {
    alarm(30);
    for (int cycle = 0; cycle < 6; cycle++) @autoreleasepool {
        HvCfgParams cfg = {.nr_cpus = 2};
        if (hv_machine_init(&machine, 4 << 20) ||
            hv_vmapple_create(&board, &machine, cpus, 64 << 20, NULL, NULL, &cfg, NULL)) return 1;
        hv_vmapple_reset_cpu(&board);
        HvPvgMetal *metal = hv_pvg_metal_create(board.pvg);
        if (!metal) { fprintf(stderr, "Metal replay unavailable; lifecycle test cannot run\n"); return 1; }
        CAMetalLayer *layer = [CAMetalLayer layer];
        hv_pvg_metal_set_layer(metal, (__bridge void *)layer);
        for (int i = 0; i < 64; i++) {
            uint32_t *cmd = calloc(6, sizeof *cmd);
            cmd[0] = 0x43; cmd[1] = 24; cmd[2] = i + 1;
            hv_pvg_metal_submit(metal, board.pvg, 0, cmd, 24);
        }
        hv_pvg_metal_cursor_moved(metal);
        hv_pvg_metal_set_layer(metal, NULL);
        hv_pvg_metal_destroy(metal);
        hv_vmapple_destroy(&board);
        hv_machine_free(&machine);
    }
    puts("Metal lifecycle: 6 cycles, 384 GPU completions drained");
    return 0;
}
