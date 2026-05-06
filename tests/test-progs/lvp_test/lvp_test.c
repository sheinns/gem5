#include <stdio.h>
#include <stdlib.h>

#ifndef ITERS
#define ITERS 100000
#endif

int arr[100];

int main(int argc, char **argv) {
    printf("Initializing array for pointer-chasing...\n");
    // Array where index 42 points back to 42
    arr[42] = 42;
    
    // We start chasing at index 42
    int val = 42;
    // prevent compiler optimization
    if (argc > 1) val = atoi(argv[1]);

    printf("Starting 100,000 dependent loads...\n");
    
    // Unroll to minimize loop overhead and focus on load latency
    for (int i = 0; i < ITERS; i++) {
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
        val = arr[val];
    }
    
    printf("Done. val=%d (expect 42)\n", val);
    return 0;
}
