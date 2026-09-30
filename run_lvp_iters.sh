#!/bin/bash

# Configuration
START=${1:-10}
END=${2:-500}
STEP=${3:-10}

GEM5_DIR="/gem5"
TEST_PROG_DIR="tests/test-progs/lvp_test"
C_SRC="${TEST_PROG_DIR}/lvp_test.c"
EXEC="${TEST_PROG_DIR}/lvp_test"
OUT_FILE="${GEM5_DIR}/m5out/lvp_sweep_results_off.csv"

cd "$GEM5_DIR"

echo "Sweeping ITERS from $START to $END with step $STEP"
echo "ITER,simTicks" > "$OUT_FILE"

for (( i=$START; i<=$END; i+=$STEP )); do
    echo "Running with ITERS = $i ..."
    
    # 1. Compile the test program with the specific ITERS value
    riscv64-linux-gnu-gcc -static -DITERS=$i "$C_SRC" -o "$EXEC"
    
    # 2. Run the gem5 simulator
    build/RISCV/gem5.opt configs/deprecated/example/se.py \
        --cpu-type=O3CPU \
        --caches \
        --cmd="$EXEC" \
        --param 'system.cpu[0].loadValuePredictor.enabled=False' > m5out/sweep_run.log 2>&1
        
    # 3. Extract the tick value
    # The output format in stats.txt is: simTicks        1234567   # ...
    TICKS=$(grep -E "^simTicks" m5out/stats.txt | awk '{print $2}')
    
    # 4. Write to the results file
    echo "$i,$TICKS" >> "$OUT_FILE"
    echo "  -> Completed: $TICKS ticks"
done

echo ""
echo "Done! Results saved to $OUT_FILE"
