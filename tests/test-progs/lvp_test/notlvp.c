/*
 * LVP Verification Test
 * 
 * Phase 1: Train the LVP by loading the same constant value 300 times
 *          from the same address (same PC in the loop).
 * Phase 2: Change the value and load again — should trigger a
 *          misprediction squash since the LVP predicts the old value.
 */
#include <stdio.h>

volatile int target = 42;

int main() {
    int sum = 0;

    /* Phase 1: Training — 300 loads of constant value 42 */
    printf("Phase 1: Training LVP with 300 constant loads...\n");
    for (int i = 0; i < 300; i++) {
        sum += target;  /* Same PC, same value → LCT counter climbs to 255 */
    }

    printf("Training done. sum=%d (expect 12600)\n", sum);

    /* Phase 2: Change value → next load should be mispredicted */
    target = 99;
    printf("Phase 2: Value changed to 99. Loading...\n");

    int val = target;  /* LVP predicts 42, actual is 99 → SQUASH */
    printf("Loaded: %d (expect 99)\n", val);

    /* Phase 3: Retrain with new value */
    printf("Phase 3: Retraining with 300 loads of value 99...\n");
    sum = 0;
    for (int i = 0; i < 300; i++) {
        sum += target;
    }
    printf("Retrain done. sum=%d (expect 29700)\n", sum);

    return 0;
}
