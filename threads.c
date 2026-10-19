#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
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

// race condition
enum { M_PLAIN, M_VOLATILE, M_ATOMIC, M_MUTEX };
static const char *mode_names[] = {
    "звичайний int (data race)",
    "volatile int (все одно race)",
    "_Atomic int (fetch_add)",
    "mutex + int"
};

static int          plain_cnt;
static volatile int vol_cnt;
static atomic_int   atom_cnt;
static int          mtx_cnt;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

// noinline: інакше компілятор при -O2 покладе лічильник у регістр на весь цикл і прибере гонку, яку ми хочемо продемонструвати
__attribute__((noinline)) static void inc_plain(int *p) { (*p)++; }

typedef struct { int mode; long iters; } warg_t;

static void *worker4(void *p) {
    warg_t *w = p;
    switch (w->mode) {
    case M_PLAIN:
        for (long i = 0; i < w->iters; i++) inc_plain(&plain_cnt);
        break;
    case M_VOLATILE:
        for (long i = 0; i < w->iters; i++) vol_cnt++; // read-modify-write,  атомарно
        break;
    case M_ATOMIC:
        for (long i = 0; i < w->iters; i++) atomic_fetch_add(&atom_cnt, 1);
        break;
    case M_MUTEX:
        for (long i = 0; i < w->iters; i++) {
            pthread_mutex_lock(&mtx);
            mtx_cnt++;
            pthread_mutex_unlock(&mtx);
        }
        break;
    }
    return NULL;
}

static double run4(int mode, int T, long iters, long *result) {
    plain_cnt = 0; vol_cnt = 0; atomic_store(&atom_cnt, 0); mtx_cnt = 0;
    pthread_t th[64];
    warg_t arg = { mode, iters }; // result - з останнього повтору, час у test_race - мінімум з 3
    double t0 = now_s();
    for (int i = 0; i < T; i++) pthread_create(&th[i], NULL, worker4, &arg);
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    double t1 = now_s();
    switch (mode) {
    case M_PLAIN:    *result = plain_cnt; break;
    case M_VOLATILE: *result = vol_cnt; break;
    case M_ATOMIC:   *result = atomic_load(&atom_cnt); break;
    default:         *result = mtx_cnt; break;
    }
    return t1 - t0;
}

static void test_race(const int *tcounts, int ntc, double scale) {
    long iters = (long)(2000000 * scale);
    if (iters < 1000) iters = 1000;
    printf("\n=== Експеримент 4: спільний лічильник (кожен потік робить %ld інкрементів) ===\n", iters);
    pname("Варіант", 30); printf(" %7s %10s %14s %14s %9s\n", "Потоків", "Час, мс", "Результат", "Очікувано", "Втрачено");
    for (int m = 0; m < 4; m++) {
        for (int k = 0; k < ntc; k++) {
            int T = tcounts[k];
            long res;
            double best = 1e9;
            for (int rep = 0; rep < 3; rep++) { // мінімум часу з 3 прогонів
                double t = run4(m, T, iters, &res);
                if (t < best) best = t;
            }
            long expected = (long)T * iters;
            pname(mode_names[m], 30); printf(" %7d %10.1f %14ld %14ld %8.1f%%\n", T, best * 1e3,
                   res, expected, 100.0 * (double)(expected - res) / (double)expected);
        }
    }
}

// false sharing
typedef struct { volatile long v; } shared_t; // 8 Б: 8 лічильників у рядку 64 Б, 16 - у рядку 128 Б
#define CACHE_LINE 128 // sysctl hw.cachelinesize на Apple Silicon; на x86 зазвичай 64
typedef struct { _Alignas(CACHE_LINE) volatile long v; char pad[CACHE_LINE - sizeof(long)]; } padded_t; // 1 лічильник = 1 рядок кешу

static shared_t fs_shared[64];
static padded_t fs_padded[64];
enum { F_ADJACENT, F_PADDED, F_LOCAL };
static const char *fs_names[] = {
    "сусідні лічильники (false sharing)",
    "з padding 128 Б (окремі рядки кешу)",
    "локальна змінна, запис наприкінці"
};

typedef struct { int id; int mode; long iters; } farg_t;

static void *worker5(void *p) {
    farg_t *a = p;
    long n = a->iters;
    switch (a->mode) {
    case F_ADJACENT:
        for (long i = 0; i < n; i++) fs_shared[a->id].v++;
        break;
    case F_PADDED:
        for (long i = 0; i < n; i++) fs_padded[a->id].v++;
        break;
    case F_LOCAL: {
        long local = 0;
        for (long i = 0; i < n; i++) { local++; __asm__ volatile("" : "+r"(local)); }
        fs_padded[a->id].v = local;
        break;
    }
    }
    return NULL;
}

static double run5(int mode, int T, long iters) {
    memset((void *)fs_shared, 0, sizeof fs_shared);
    memset((void *)fs_padded, 0, sizeof fs_padded);
    pthread_t th[64];
    farg_t args[64];
    double t0 = now_s();
    for (int i = 0; i < T; i++) {
        args[i] = (farg_t){ i, mode, iters };
        pthread_create(&th[i], NULL, worker5, &args[i]);
    }
    for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
    return now_s() - t0;
}

static void test_false_sharing(const int *tcounts, int ntc, double scale) {
    long iters = (long)(50000000 * scale);
    if (iters < 1000) iters = 1000;
    printf("\n=== Експеримент 5: false sharing (кожен потік інкрементує СВІЙ лічильник %ld разів) ===\n", iters);
    printf("sizeof(shared_t)=%zu, sizeof(padded_t)=%zu\n", sizeof(shared_t), sizeof(padded_t));
    pname("Варіант", 38); printf(" %7s %10s %12s\n", "Потоків", "Час, мс", "Прискорення");

    run5(F_LOCAL, 1, iters / 10 + 1); // розігрів: CPU виходить зі стану low-power

    for (int k = 0; k < ntc; k++) {
        int T = tcounts[k];
        double adj = 0; // час варіанта сусідні лічильники
        for (int m = 0; m < 3; m++) {
            double best = 1e9;
            for (int rep = 0; rep < 3; rep++) {
                double t = run5(m, T, iters);
                if (t < best) best = t;
            }
            if (m == F_ADJACENT) adj = best;
            pname(fs_names[m], 38); printf(" %7d %10.1f", T, best * 1e3);
            if (m == F_ADJACENT) printf(" %12s\n", "(базовий)");
            else printf(" %11.2fx\n", adj / best);
        }
    }
}

int main(int argc, char **argv) {
    const char *which = argc > 1 ? argv[1] : "all";
    double scale = argc > 2 ? atof(argv[2]) : 1.0;
    int all = strcmp(which, "all") == 0;

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    printf("Логічних ядер: %ld\n", ncpu);
    int tcounts[8], ntc = 0;
    for (int t = 1; t <= ncpu && t <= 16 && ntc < 7; t *= 2) tcounts[ntc++] = t;
    if (ncpu > 1 && ncpu <= 16 && tcounts[ntc - 1] != ncpu) tcounts[ntc++] = (int)ncpu;
    if (ntc == 1) tcounts[ntc++] = 2; // навіть на 1 ядрі покажемо 2 потоки

    if (all || !strcmp(which, "1")) test_race(tcounts, ntc, scale);
    if (all || !strcmp(which, "2")) test_false_sharing(tcounts, ntc, scale);
    return 0;
}