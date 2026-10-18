/*
 * Лабораторна: використання кешу
 * Частина 2: множення матриць - вплив порядку циклів і блочності (tiling)
 *
 * Збірка:  gcc -O2 -std=c11 matrix.c -o matrix -lm
 * Запуск:  ./matrix [N]        (за замовчуванням N = 512; спробуйте 1024)
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* вирівнювання колонок з UTF-8 (printf рахує байти, а не символи) */
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

/* i-j-k: B читається по стовпчику (крок N*8 байт) -> багато промахів */
static void mm_ijk(const double *A, const double *B, double *C, int n) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            double s = 0;
            for (int k = 0; k < n; k++) s += A[i * n + k] * B[k * n + j];
            C[i * n + j] = s;
        }
}

/* j-k-i: найгірший порядок, A і C читаються по стовпчиках */
static void mm_jki(const double *A, const double *B, double *C, int n) {
    memset(C, 0, (size_t)n * n * sizeof(double));
    for (int j = 0; j < n; j++)
        for (int k = 0; k < n; k++) {
            double b = B[k * n + j];
            for (int i = 0; i < n; i++) C[i * n + j] += A[i * n + k] * b;
        }
}

/* i-k-j: усі три матриці читаються по рядках (послідовно) */
static void mm_ikj(const double *A, const double *B, double *C, int n) {
    memset(C, 0, (size_t)n * n * sizeof(double));
    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++) {
            double a = A[i * n + k];
            for (int j = 0; j < n; j++) C[i * n + j] += a * B[k * n + j];
        }
}

/* i-j-k з попереднім транспонуванням B (час транспонування входить у вимір) */
static void mm_ijk_T(const double *A, const double *B, double *C, int n) {
    double *Bt = malloc((size_t)n * n * sizeof(double));
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) Bt[j * n + i] = B[i * n + j];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            double s = 0;
            for (int k = 0; k < n; k++) s += A[i * n + k] * Bt[j * n + k];
            C[i * n + j] = s;
        }
    free(Bt);
}

/* блочне множення (tiling): блок BS x BS трьох матриць вміщується в кеш */
static void mm_blocked(const double *A, const double *B, double *C, int n, int BS) {
    memset(C, 0, (size_t)n * n * sizeof(double));
    for (int ii = 0; ii < n; ii += BS)
        for (int kk = 0; kk < n; kk += BS)
            for (int jj = 0; jj < n; jj += BS) {
                int imax = ii + BS < n ? ii + BS : n;
                int kmax = kk + BS < n ? kk + BS : n;
                int jmax = jj + BS < n ? jj + BS : n;
                for (int i = ii; i < imax; i++)
                    for (int k = kk; k < kmax; k++) {
                        double a = A[i * n + k];
                        for (int j = jj; j < jmax; j++) C[i * n + j] += a * B[k * n + j];
                    }
            }
}

static double checksum(const double *C, int n) {
    double s = 0;
    for (size_t i = 0; i < (size_t)n * n; i++) s += C[i];
    return s;
}

typedef struct { const char *name; int bs; void (*f)(const double *, const double *, double *, int); } variant_t;

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 512;
    size_t sz = (size_t)n * n;
    double *A = malloc(sz * sizeof(double));
    double *B = malloc(sz * sizeof(double));
    double *C = malloc(sz * sizeof(double));
    if (!A || !B || !C) { fprintf(stderr, "malloc failed\n"); return 1; }
    srand(42);
    for (size_t i = 0; i < sz; i++) { A[i] = (double)rand() / RAND_MAX; B[i] = (double)rand() / RAND_MAX; }

    variant_t vs[] = {
        {"i-j-k (класичний)",         0,   mm_ijk},
        {"j-k-i (найгірший)",         0,   mm_jki},
        {"i-k-j (по рядках)",         0,   mm_ikj},
        {"i-j-k + транспонування B",  0,   mm_ijk_T},
        {"блочний i-k-j, блок 16",    16,  NULL},
        {"блочний i-k-j, блок 32",    32,  NULL},
        {"блочний i-k-j, блок 64",    64,  NULL},
        {"блочний i-k-j, блок 128",   128, NULL},
    };
    int cnt = (int)(sizeof vs / sizeof vs[0]);

    printf("=== Множення матриць %d x %d (double, %.1f МБ на матрицю) ===\n", n, n, sz * 8.0 / (1 << 20));
    pname("Варіант", 28); printf(" %10s %10s %14s %s\n", "Час, с", "GFLOPS", "Відносно ikj", "Перевірка");

    double ref = 0, t_ikj = 0, times[16];
    double sums[16];
    for (int v = 0; v < cnt; v++) {
        double best = 1e9;
        for (int rep = 0; rep < 2; rep++) {
            double t0 = now_s();
            if (vs[v].f) vs[v].f(A, B, C, n); else mm_blocked(A, B, C, n, vs[v].bs);
            double t1 = now_s();
            if (t1 - t0 < best) best = t1 - t0;
        }
        times[v] = best; sums[v] = checksum(C, n);
        if (v == 0) ref = sums[0];
        if (v == 2) t_ikj = best;
    }
    for (int v = 0; v < cnt; v++) {
        double gf = 2.0 * n * n * n / times[v] / 1e9;
        int ok = fabs(sums[v] - ref) / fabs(ref) < 1e-9;
        pname(vs[v].name, 28); printf(" %10.3f %10.2f %13.2fx %s\n", times[v], gf,
               times[v] / t_ikj, ok ? "OK" : "РІЗНИЦЯ!");
    }
    free(A); free(B); free(C);
    return 0;
}