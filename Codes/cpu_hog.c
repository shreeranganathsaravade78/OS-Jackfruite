// cpu_hog.c
//
// GIVEN BY PROFESSOR (test workload)
//
// What it does:
//   Burns 100% CPU in an infinite loop.
//   Used for scheduler experiments — two instances compete for CPU.
//   Run inside a container to see how Linux shares CPU.
//
// How to use:
//   cp cpu_hog ./rootfs-alpha/
//   sudo ./engine start cpu1 ./rootfs-alpha /cpu_hog
//   sudo ./engine start cpu2 ./rootfs-beta  /cpu_hog
//   # Then compare how much CPU each gets

#include <stdio.h>
#include <time.h>
#include <signal.h>

static volatile int running = 1;

static void handle_sig(int s) { (void)s; running = 0; }

int main(void)
{
    signal(SIGTERM, handle_sig);
    signal(SIGINT,  handle_sig);

    printf("[cpu_hog] starting CPU burn\n");
    fflush(stdout);

    long long count = 0;
    time_t start = time(NULL);

    while (running) {
        /* Busy spin — uses 100% CPU */
        for (volatile long i = 0; i < 1000000L; i++) {}
        count++;

        /* Print progress every 5 seconds */
        long elapsed = (long)(time(NULL) - start);
        if (count % 500 == 0) {
            printf("[cpu_hog] elapsed=%lds iterations=%lld\n", elapsed, count);
            fflush(stdout);
        }
    }

    printf("[cpu_hog] exiting after %lld iterations\n", count);
    return 0;
}
