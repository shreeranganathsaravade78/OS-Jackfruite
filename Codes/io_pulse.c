// io_pulse.c
//
// GIVEN BY PROFESSOR (test workload)
//
// What it does:
//   Repeatedly writes and reads a temp file.
//   This is an I/O-bound process — it spends most time waiting for disk.
//   Used to compare with cpu_hog in scheduler experiments.
//
// Key OS concept shown:
//   Linux scheduler gives more CPU to I/O-bound processes when they
//   "wake up" from waiting — they get a priority boost.
//   CPU-bound processes get lower priority over time.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

static volatile int running = 1;
static void handle_sig(int s) { (void)s; running = 0; }

int main(void)
{
    signal(SIGTERM, handle_sig);
    signal(SIGINT,  handle_sig);

    printf("[io_pulse] starting I/O workload\n");
    fflush(stdout);

    char  buf[4096];
    memset(buf, 'X', sizeof(buf));
    long  cycles = 0;
    time_t start = time(NULL);

    while (running) {
        /* Write to temp file */
        FILE *f = fopen("/tmp/io_test.dat", "w");
        if (f) {
            for (int i = 0; i < 256; i++) fwrite(buf, 1, sizeof(buf), f);
            fclose(f);
        }

        /* Read it back */
        f = fopen("/tmp/io_test.dat", "r");
        if (f) {
            while (fread(buf, 1, sizeof(buf), f) > 0) {}
            fclose(f);
        }

        cycles++;
        long elapsed = (long)(time(NULL) - start);
        if (cycles % 100 == 0) {
            printf("[io_pulse] elapsed=%lds cycles=%ld\n", elapsed, cycles);
            fflush(stdout);
        }

        usleep(10000);   /* 10ms pause to simulate realistic I/O wait */
    }

    printf("[io_pulse] done, %ld cycles\n", cycles);
    unlink("/tmp/io_test.dat");
    return 0;
}
