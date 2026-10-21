#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

static void pname(const char *s, int w) {
    int c = 0;
    for (const char *p = s; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) c++;
    printf("%s%*s", s, w > c ? w - c : 0, "");
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#define MAXT 16
#define MAX_STRIDE_LONGS 32
static volatile long flat[MAXT * MAX_STRIDE_LONGS] __attribute__((aligned(256)));

typedef struct { int id; size_t stride; int local; long iters; } arg_t;

static void *worker(void *p) {
    arg_t *a = p;
    long n = a->iters;
    if (a->local) {
        long x = 0;
        for (long i = 0; i < n; i++) { x++; __asm__ volatile("" : "+r"(x)); }
        flat[(size_t)a->id * a->stride] = x;
    } else {
        volatile long *c = &flat[(size_t)a->id * a->stride];
        for (long i = 0; i < n; i++) (*c)++;
    }
    return NULL;
}

static double run(int T, size_t stride, int local, long iters) {
    memset((void *)flat, 0, sizeof flat);
    pthread_t th[MAXT];
    arg_t args[MAXT];
    double t0 = now_s();
    for (int i = 0; i < T; i++) {
        args[i] = (arg_t){ i, stride, local, iters };
        pthread_create(&th[i], NULL, worker, &args[i]);
    }
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    return now_s() - t0;
}

int main(int argc, char **argv) {
    double scale = argc > 1 ? atof(argv[1]) : 1.0;
    long iters = (long)(50000000 * scale);
    if (iters < 1000) iters = 1000;

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int tcounts[8], ntc = 0;
    for (int t = 1; t <= ncpu && t <= MAXT && ntc < 7; t *= 2) tcounts[ntc++] = t;
    if (ncpu > 1 && ncpu <= MAXT && tcounts[ntc - 1] != ncpu) tcounts[ntc++] = (int)ncpu;
    if (ntc == 1) tcounts[ntc++] = 2;

    struct { const char *name; size_t stride; int local; } variants[] = {
        { "інтервал 8 Б (в одному рядку)",     1,  0 },
        { "інтервал 64 Б",                     8,  0 },
        { "інтервал 128 Б (= рядок кешу)",     16, 0 },
        { "інтервал 256 Б",                    32, 0 },
        { "локальна змінна (запис наприкінці)", 16, 1 },
    };
    int nv = (int)(sizeof variants / sizeof variants[0]);

    printf("Логічних ядер: %ld\n", ncpu);
    printf("\nFalse sharing: вплив відстані між лічильниками (кожен потік %ld інкрементів)\n", iters);
    pname("Варіант", 38); printf(" %7s %10s %22s\n", "Потоків", "Час, мс", "Час / час при 1 потоці");

    run(1, 16, 1, iters / 10 + 1);

    for (int v = 0; v < nv; v++) {
        double t1 = 0;
        for (int k = 0; k < ntc; k++) {
            int T = tcounts[k];
            double best = 1e9;
            for (int rep = 0; rep < 3; rep++) {
                double t = run(T, variants[v].stride, variants[v].local, iters);
                if (t < best) best = t;
            }
            if (T == 1) t1 = best;
            pname(variants[v].name, 38);
            printf(" %7d %10.1f %21.2fx\n", T, best * 1e3, best / t1);
        }
    }
    return 0;
}