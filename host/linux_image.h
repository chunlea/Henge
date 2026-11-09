// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef HENGEVM_LINUX_IMAGE_H
#define HENGEVM_LINUX_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/* A disk image published as a Zstandard stream cut into parts (how Omarchy's
   is: files .001, .002, … one after another make one .img.zst), unpacked into
   a sparse file: runs of zero blocks are skipped, not written, so a 20 GB disk
   takes the space of its data. With remove_parts, each part is deleted once it
   has been read, so the parts and the disk never need room at the same time.

   progress(done, total, user): compressed bytes read so far, of all of them.
   Returns 0, or -1 with a reason in err. */
int hv_linux_image_unpack(const char *const *parts, int nparts, const char *out_path, int remove_parts,
                          void (*progress)(uint64_t done, uint64_t total, void *user), void *user,
                          char *err, size_t errlen);

#endif
