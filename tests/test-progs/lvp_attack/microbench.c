#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define LINE_SIZE 64
#define FLUSH_BUF_SIZE (64 * 1024)
#define TRIALS 100

static uint8_t flush_buf[FLUSH_BUF_SIZE];
static uint8_t victim_buf[8192];

static inline uint64_t rdcycle(void) {
    uint64_t c;
    asm volatile("csrrs %0, cycle, x0" : "=r"(c) :: "memory");
    return c;
}

static void flush_all_l1(void) {
    volatile uint8_t *fb = flush_buf;
    for (int i = 0; i < FLUSH_BUF_SIZE; i += LINE_SIZE)
        (void)fb[i];
}

static inline uint64_t measure_one(volatile uint8_t *p) {
    uint64_t c1, c2;
    uint8_t v;
    c1 = rdcycle();
    v = *p;
    asm volatile("" : "+r"(v) : : "memory");
    c2 = rdcycle();
    return c2 - c1;
}

static uint64_t tmin(uint64_t *a, int n) {
    uint64_t m = ~0ULL;
    for (int i = 0; i < n; i++)
        if (a[i] < m) m = a[i];
    return m;
}

static uint64_t tmax(uint64_t *a, int n) {
    uint64_t m = 0;
    for (int i = 0; i < n; i++)
        if (a[i] > m) m = a[i];
    return m;
}

static double tavg(uint64_t *a, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += (double)a[i];
    return s / n;
}

int main(void) {
    volatile uint8_t *hot = victim_buf;
    volatile uint8_t *cold = victim_buf + 4096;
    uint64_t hit[TRIALS], miss[TRIALS];

    printf("cache microbench start\n");
    uint64_t a = rdcycle();
    uint64_t b = rdcycle();
    printf("rdcycle delta ~%llu cycles\n", (unsigned long long)(b - a));

    for (int i = 0; i < TRIALS; i++) {
        flush_all_l1();
        miss[i] = measure_one(hot);
        hit[i] = measure_one(hot);
        {
            uint64_t c = measure_one(cold);
            (void)c;
        }
    }

    printf("hot(hit)      avg=%.1f  min=%llu  max=%llu\n",
           tavg(hit, TRIALS), (unsigned long long)tmin(hit, TRIALS),
           (unsigned long long)tmax(hit, TRIALS));
    printf("hot(miss)     avg=%.1f  min=%llu  max=%llu\n",
           tavg(miss, TRIALS), (unsigned long long)tmin(miss, TRIALS),
           (unsigned long long)tmax(miss, TRIALS));
    printf("cache microbench done\n");
    return 0;
}