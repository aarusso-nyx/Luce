slice: S0
title: Inventory and extraction map
timestamp: 20260222_210632
commands:
  - for e in luce_stage0 luce_stage1 luce_stage2 luce_stage3 luce_stage4 luce_stage5 luce_stage6 luce_stage7 luce_stage8 luce_stage9 luce_stage10; do
      python3 -m platformio run -e $e;
    done
  - python3 -m platformio test -e luce_test_native
evidence:
  - split/s0_plan_capture.md
  - 20_build/luce_stage0.txt
  - 20_build/luce_stage1.txt
  - 20_build/luce_stage2.txt
  - 20_build/luce_stage3.txt
  - 20_build/luce_stage4.txt
  - 20_build/luce_stage5.txt
  - 20_build/luce_stage6.txt
  - 20_build/luce_stage7.txt
  - 20_build/luce_stage8.txt
  - 20_build/luce_stage9.txt
  - 20_build/luce_stage10.txt
  - 30_unit/native_test.txt
  - 00_index.md
  - 90_summary.md
result:
  - all_stage_builds=PASS
  - native_unit=PASS
