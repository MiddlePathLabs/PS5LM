/* SPDX-License-Identifier: GPL-3.0-or-later
 * The pinned SDK supplies mkstemps, but not the later mkstemp adapter.
 * Reuse its real O_EXCL implementation, not libScePosixForWebKit's title-inaccessible
 * import. The probe's open guard still rejects any unexpected temp-file write.
 */
#include <ps5platform/libc.h>

int ps5lm_mkstemp(char *path_template)
{
    return ps5_mkstemps(path_template, 0);
}

/* Nor posix_madvise, which llama.cpp's mmap loader calls for paging hints.
 * The model build loads without mmap, so nothing calls this; a hint that is
 * not applied reports ENOSYS instead of claiming success. */
#include <errno.h>
#include <stddef.h>

int ps5lm_posix_madvise(void *address, size_t length, int advice)
{
    (void)address;
    (void)length;
    (void)advice;
    return ENOSYS;
}
