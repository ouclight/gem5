#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gem5/m5ops.h>

#define PAGE_SIZE 4096
#define ALIAS0_ADDR ((void *)0x600000000000ULL)
#define ALIAS1_ADDR ((void *)0x600000001000ULL)

static int
reserve_page(void *address)
{
    void *mapped = mmap(address, PAGE_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mapped == MAP_FAILED) {
        fprintf(stderr, "mmap(%p) failed: %s\n", address, strerror(errno));
        return 1;
    }
    if (mapped != address) {
        fprintf(stderr, "mmap returned %p instead of %p\n", mapped, address);
        return 1;
    }
    return 0;
}

int
main(void)
{
    if (reserve_page(ALIAS0_ADDR) || reserve_page(ALIAS1_ADDR)) {
        return 1;
    }

    /*
     * The gem5 config handles this event by mapping both untouched VMAs to
     * one physical page, then resumes the program.
     */
    m5_work_begin(0, 0);

    volatile uint8_t *alias0 = (volatile uint8_t *)ALIAS0_ADDR;
    volatile uint8_t *alias1 = (volatile uint8_t *)ALIAS1_ADDR;

    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        alias0[i] = (uint8_t)((i * 37U + 11U) & 0xffU);
    }

    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        uint8_t expected = (uint8_t)((i * 37U + 11U) & 0xffU);
        if (alias1[i] != expected) {
            fprintf(stderr,
                    "aliases do not share data before munmap at byte %zu: "
                    "expected %#x got %#x\n",
                    i, expected, alias1[i]);
            return 1;
        }
    }

    if (munmap(ALIAS0_ADDR, PAGE_SIZE) != 0) {
        fprintf(stderr, "munmap(%p) failed: %s\n",
                ALIAS0_ADDR, strerror(errno));
        return 1;
    }

    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        uint8_t expected = (uint8_t)((i * 37U + 11U) & 0xffU);
        if (alias1[i] != expected) {
            fprintf(stderr,
                    "surviving alias changed after munmap at byte %zu: "
                    "expected %#x got %#x\n",
                    i, expected, alias1[i]);
            return 1;
        }
    }

    puts("SE page alias preservation passed");
    return 0;
}
