// SPDX-License-Identifier: GPL-2.0-or-later
/* hvlinux-unpack OUT.img PART... : host/linux_image.c on the Mac, for testing what the app does with Omarchy's parts. */
#include <stdio.h>

#include "linux_image.h"

static void progress(uint64_t done, uint64_t total, void *user) {
    (void)user;
    static int last = -1;
    int pct = (int)(done * 100 / (total ? total : 1));
    if (pct != last) fprintf(stderr, "\r%d%%", pct), last = pct;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: hvlinux-unpack OUT.img PART...\n");
        return 2;
    }
    char err[256];
    if (hv_linux_image_unpack((const char *const *)argv + 2, argc - 2, argv[1], 0, progress, NULL, err, sizeof err) != 0) {
        fprintf(stderr, "\nhvlinux-unpack: %s\n", err);
        return 1;
    }
    fprintf(stderr, "\n");
    return 0;
}
