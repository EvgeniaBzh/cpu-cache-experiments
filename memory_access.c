/*
 * Лабораторна: використання кешу
 * Частина 1: шаблони доступу до пам'яті
 *   Експеримент 1 - послідовний vs випадковий доступ
 *   Експеримент 2 - залежність від розміру робочого набору (pointer chasing)
 *   Експеримент 3 - крок доступу (stride), визначення розміру cache line
 *
 * Збірка:  gcc -O2 -std=c11 memory_access.c -o memory_access
 * Запуск:  ./memory_access [1|2|3|all] [розмір_МБ_для_експ.1]
 *          наприклад: ./memory_access all 128
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
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

/* швидкий генератор xorshift64 */
static uint64_t rng_state = 88172645463325252ULL;
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* sink не дає компілятору викинути обчислення, результат яких не використовується */
static volatile long long sink;

/* ---------------------------------------------------------------- */
/* Експеримент 1: послідовний vs випадковий доступ                  */
/* ---------------------------------------------------------------- */
static long long sum_direct(const int *a, size_t n) {
    long long s = 0;
    for (size_t i = 0; i < n; i++) s += a[i];
    return s;
}

static long long sum_indirect(const int *a, const uint32_t *idx, size_t n) {
    long long s = 0;
    for (size_t i = 0; i < n; i++) s += a[idx[i]];
    return s;
}

static void test_seq_vs_rand(size_t mb) {
    size_t n = mb * 1024 * 1024 / sizeof(int);
    int *a = malloc(n * sizeof(int));
    uint32_t *idx_seq = malloc(n * sizeof(uint32_t));
    uint32_t *idx_rnd = malloc(n * sizeof(uint32_t));
    if (!a || !idx_seq || !idx_rnd) { fprintf(stderr, "malloc failed\n"); exit(1); }

    for (size_t i = 0; i < n; i++) {
        a[i] = (int)(i & 0xFF);
        idx_seq[i] = (uint32_t)i;
        idx_rnd[i] = (uint32_t)i;
    }
    for (size_t i = n - 1; i > 0; i--) {          /* Фішер-Єйтс */
        size_t j = rnd() % (i + 1);
        uint32_t t = idx_rnd[i]; idx_rnd[i] = idx_rnd[j]; idx_rnd[j] = t;
    }

    const char *names[3] = {
        "a[i]           (послідовно, прямий доступ)",
        "a[idx_seq[i]]  (послідовно через індекси)",
        "a[idx_rnd[i]]  (випадковий порядок)"
    };
    double best[3] = {1e9, 1e9, 1e9};
    long long res[3] = {0, 0, 0};

    for (int r = 0; r < 3; r++) {                 /* беремо мінімум з 3 прогонів */
        double t0 = now_s();
        res[0] = sum_direct(a, n);
        double t1 = now_s();
        res[1] = sum_indirect(a, idx_seq, n);
        double t2 = now_s();
        res[2] = sum_indirect(a, idx_rnd, n);
        double t3 = now_s();
        if (t1 - t0 < best[0]) best[0] = t1 - t0;
        if (t2 - t1 < best[1]) best[1] = t2 - t1;
        if (t3 - t2 < best[2]) best[2] = t3 - t2;
    }
    sink = res[0] + res[1] + res[2];

    printf("\n=== Експеримент 1: послідовний vs випадковий доступ (масив %zu МБ, %zu елементів) ===\n", mb, n);
    pname("Варіант", 46); printf(" %10s %12s %10s %s\n", "Час, мс", "нс/елемент", "Відносно", "Сума");
    for (int i = 0; i < 3; i++) {
        pname(names[i], 46); printf(" %10.1f %12.2f %9.1fx %lld\n", best[i] * 1e3,
               best[i] / (double)n * 1e9, best[i] / best[0], res[i]);
    }

    free(a); free(idx_seq); free(idx_rnd);
}

/* ---------------------------------------------------------------- */
/* Експеримент 2: розмір робочого набору, pointer chasing           */
/* Кожен вузол = рівно 1 рядок кешу (64 Б), наступний вузол         */
/* вибирається випадково -> prefetcher безсилий.                    */
/* ---------------------------------------------------------------- */
typedef struct { size_t next; char pad[64 - sizeof(size_t)]; } node_t;

static void test_working_set(void) {
    const size_t steps = 1u << 24;                /* 16.7 млн доступів на кожен розмір */
    printf("\n=== Експеримент 2: затримка доступу залежно від розміру робочого набору ===\n");
    printf("%12s %14s\n", "Розмір", "нс/доступ");

    for (size_t bytes = 4096; bytes <= (256u << 20); bytes *= 2) {
        size_t n = bytes / sizeof(node_t);
        node_t *nodes = aligned_alloc(64, n * sizeof(node_t));
        if (!nodes) { fprintf(stderr, "alloc failed\n"); exit(1); }
        for (size_t i = 0; i < n; i++) { nodes[i].next = i; memset(nodes[i].pad, 0, sizeof nodes[i].pad); }
        /* алгоритм Саттоло: перестановка з одним єдиним циклом */
        for (size_t i = n - 1; i > 0; i--) {
            size_t j = rnd() % i;
            size_t t = nodes[i].next; nodes[i].next = nodes[j].next; nodes[j].next = t;
        }
        size_t p = 0;
        size_t warm = n < steps ? n : steps;      /* прогрів кешу */
        for (size_t s = 0; s < warm; s++) p = nodes[p].next;

        double t0 = now_s();
        for (size_t s = 0; s < steps; s++) p = nodes[p].next;   /* залежний ланцюг */
        double t1 = now_s();
        sink = (long long)p;

        double ns = (t1 - t0) / (double)steps * 1e9;
        if (bytes < (1u << 20)) printf("%9zu КБ %14.2f\n", bytes >> 10, ns);
        else                    printf("%9zu МБ %14.2f\n", bytes >> 20, ns);
        free(nodes);
    }
}

/* ---------------------------------------------------------------- */
/* Експеримент 3: крок доступу (stride)                             */
/* Обсяг роботи однаковий (N інкрементів), міняється лише порядок.  */
/* ---------------------------------------------------------------- */
static void test_stride(void) {
    const size_t n = 16u << 20;                   /* 16M int = 64 МБ */
    int *a = calloc(n, sizeof(int));
    if (!a) { fprintf(stderr, "alloc failed\n"); exit(1); }
    printf("\n=== Експеримент 3: крок доступу (масив 64 МБ, всього %zu інкрементів) ===\n", n);
    printf("%8s %10s %8s %12s %10s\n", "Крок", "Крок, Б", "Час, мс", "нс/елемент", "Відносно");

    double base = 0;
    for (size_t stride = 1; stride <= 1024; stride *= 2) {
        double best = 1e9;
        for (int rep = 0; rep < 3; rep++) {
            double t0 = now_s();
            for (size_t r = 0; r < stride; r++)
                for (size_t i = r; i < n; i += stride) a[i]++;
            double t1 = now_s();
            if (t1 - t0 < best) best = t1 - t0;
        }
        if (stride == 1) base = best;
        printf("%8zu %10zu %8.1f %12.2f %9.1fx\n", stride, stride * sizeof(int),
               best * 1e3, best / (double)n * 1e9, best / base);
    }
    sink = a[12345];
    free(a);
}

int main(int argc, char **argv) {
    const char *which = argc > 1 ? argv[1] : "all";
    size_t mb = argc > 2 ? (size_t)atol(argv[2]) : 128;
    int all = strcmp(which, "all") == 0;
    if (all || !strcmp(which, "1")) test_seq_vs_rand(mb);
    if (all || !strcmp(which, "2")) test_working_set();
    if (all || !strcmp(which, "3")) test_stride();
    return 0;
}