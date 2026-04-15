// memory_hog.c
//
// GIVEN BY PROFESSOR (test workload)
//
// What it does:
//   Slowly allocates more and more RAM (1 MiB every second).
//   Used to test the kernel module's soft and hard memory limits.
//
// Expected behavior:
//   - When it crosses soft_mib: dmesg shows a WARNING
//   - When it crosses hard_mib: dmesg shows KILLED, process dies
//
// How to use:
//   cp memory_hog ./rootfs-alpha/
//   sudo ./engine start memtest ./rootfs-alpha /memory_hog --soft-mib 32 --hard-mib 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define CHUNK_MB  1                     /* allocate 1 MiB per step */
#define CHUNK     (CHUNK_MB * 1024 * 1024)

static volatile int running = 1;
static void handle_sig(int s) { (void)s; running = 0; }

int main(void)
{
    signal(SIGTERM, handle_sig);
    signal(SIGINT,  handle_sig);

    printf("[memory_hog] starting — will allocate %d MiB every second\n",
           CHUNK_MB);
    fflush(stdout);

    long total_mb = 0;

    while (running) {
        char *p = malloc(CHUNK);
        if (!p) {
            printf("[memory_hog] malloc failed at %ld MiB\n", total_mb);
            break;
        }
        /* Touch every page so OS actually allocates physical RAM */
        memset(p, 0xAB, CHUNK);
        total_mb += CHUNK_MB;

        printf("[memory_hog] allocated %ld MiB total\n", total_mb);
        fflush(stdout);

        sleep(1);
    }

    printf("[memory_hog] exiting (total allocated: %ld MiB)\n", total_mb);
    return 0;
}
