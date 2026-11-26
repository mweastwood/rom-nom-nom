# GCC 2.7.2 Per-Pass Line & Branch Coverage Baseline (Stages 0–6)

- **Shared Corpus Size**: 4,382 cases (626 base C functions × 7 compiler/assembler flag profiles)
- **Parallel Shards / Workers**: 4
- **Reproduction Command**: `python3 tools/gcc_pass_coverage.py --markdown-out=docs/gcc_pass_coverage_stages_0_6.md`
- **Methodology**: Isolated `gcov --coverage -fprofile-abs-path` archive (`toolchain/libgcc272_harness_cov.a`, 14 MIPS-preprocessed GCC 2.7.2 pass files). `forward` executes only live `TraceForwardCompilation(tc)` across all corpus cases; `replay` loads cached traces and executes only live Stages 0–6 right-inverse replay (with fail-fast exact match assertions on every single check).

## 1. Per-File MIPS-Reachable Line & Branch Coverage

| Stage | GCC 2.7.2 File | Total Lines | Forward Lines | Replay Lines | Total Branches | Forward Branches | Replay Branches |
|---|---|---:|---:|---:|---:|---:|---:|
| Stage 0 (Final) | `final.c` | 727 | 279 (38.4%) | 271 (37.3%) | 646 | 183 (28.3%) | 178 (27.6%) |
| Stage 0 (MIPS Backend) | `config/mips/mips.c` | 1,784 | 1,019 (57.1%) | 833 (46.7%) | 1,855 | 857 (46.2%) | 692 (37.3%) |
| Stage 1 (DBR Schedule) | `reorg.c` | 1,472 | 1,183 (80.4%) | 1,158 (78.7%) | 1,715 | 1,200 (70.0%) | 1,193 (69.6%) |
| Stage 2 (Jump2) | `jump.c` | 1,396 | 1,092 (78.2%) | 621 (44.5%) | 1,642 | 1,135 (69.1%) | 666 (40.6%) |
| Stages 3 & 6 (Sched) | `sched.c` | 1,683 | 1,182 (70.2%) | 1,266 (75.2%) | 1,696 | 1,021 (60.2%) | 1,073 (63.3%) |
| Stage 4 (Global Alloc) | `global.c` | 513 | 412 (80.3%) | 417 (81.3%) | 652 | 454 (69.6%) | 459 (70.4%) |
| Stage 4 (Reload) | `reload.c` | 1,965 | 946 (48.1%) | 896 (45.6%) | 3,196 | 1,228 (38.4%) | 1,170 (36.6%) |
| Stage 4 (Reload1) | `reload1.c` | 2,045 | 1,327 (64.9%) | 1,329 (65.0%) | 2,628 | 1,435 (54.6%) | 1,437 (54.7%) |
| Stage 4 (Caller-Save) | `caller-save.c` | 222 | 208 (93.7%) | 208 (93.7%) | 268 | 215 (80.2%) | 216 (80.6%) |
| Stage 5 (Local Alloc) | `local-alloc.c` | 738 | 615 (83.3%) | 667 (90.4%) | 771 | 598 (77.6%) | 614 (79.6%) |
| Stage 7 (Combine — baseline) | `combine.c` | 3,929 | 2,840 (72.3%) | 6 (0.2%) | 5,380 | 3,177 (59.1%) | 1 (0.0%) |
| Stage 8 (Flow — baseline) | `flow.c` | 795 | 650 (81.8%) | 626 (78.7%) | 848 | 598 (70.5%) | 579 (68.3%) |
| Stages 9 & 11 (CSE — baseline) | `cse.c` | 3,024 | 2,238 (74.0%) | 14 (0.5%) | 4,514 | 2,695 (59.7%) | 33 (0.7%) |
| Stage 10 (Loop — baseline) | `loop.c` | 2,070 | 1,438 (69.5%) | 8 (0.4%) | 2,330 | 1,343 (57.6%) | 0 (0.0%) |
| **Stages 0–6 Subtotal (10 files)** | — | **12,545** | **8,263 (65.9%)** | **7,666 (61.1%)** | **15,069** | **8,326 (55.3%)** | **7,698 (51.1%)** |
| **All 14 Files Total** | — | **22,363** | **15,429 (69.0%)** | **8,320 (37.2%)** | **28,141** | **16,139 (57.4%)** | **8,311 (29.5%)** |

## 2. Forward ($F_s$) vs. Right-Inverse Replay ($F_s(G_s(x))$) Diff

| Stage | GCC 2.7.2 File | Both Lines | Forward-Only Lines | Replay-Only Lines | Replay / Forward Overlap | Forward-Only Branches | Replay-Only Branches |
|---|---|---:|---:|---:|---:|---:|---:|
| Stage 0 (Final) | `final.c` | 271 | 8 | 0 | 97.1% | 5 | 0 |
| Stage 0 (MIPS Backend) | `config/mips/mips.c` | 833 | 186 | 0 | 81.7% | 166 | 1 |
| Stage 1 (DBR Schedule) | `reorg.c` | 1,157 | 26 | 1 | 97.8% | 18 | 11 |
| Stage 2 (Jump2) | `jump.c` | 621 | 471 | 0 | 56.9% | 472 | 3 |
| Stages 3 & 6 (Sched) | `sched.c` | 1,181 | 1 | 85 | 99.9% | 2 | 54 |
| Stage 4 (Global Alloc) | `global.c` | 412 | 0 | 5 | 100.0% | 0 | 5 |
| Stage 4 (Reload) | `reload.c` | 896 | 50 | 0 | 94.7% | 60 | 2 |
| Stage 4 (Reload1) | `reload1.c` | 1,327 | 0 | 2 | 100.0% | 0 | 2 |
| Stage 4 (Caller-Save) | `caller-save.c` | 208 | 0 | 0 | 100.0% | 0 | 1 |
| Stage 5 (Local Alloc) | `local-alloc.c` | 612 | 3 | 55 | 99.5% | 8 | 24 |
| Stage 7 (Combine — baseline) | `combine.c` | 6 | 2,834 | 0 | 0.2% | 3,176 | 0 |
| Stage 8 (Flow — baseline) | `flow.c` | 623 | 27 | 3 | 95.8% | 24 | 5 |
| Stages 9 & 11 (CSE — baseline) | `cse.c` | 14 | 2,224 | 0 | 0.6% | 2,662 | 0 |
| Stage 10 (Loop — baseline) | `loop.c` | 8 | 1,430 | 0 | 0.6% | 1,343 | 0 |

## 3. Stages 0–6 Per-Function Gap Analysis (`file:line` Citations)

### `final.c` — Stage 0 (Final)
- Lines: **279/727 (38.4%)** forward, **271/727 (37.3%)** replay (overlap **97.1%**, **8** forward-only, **0** replay-only).
- Branches: **183/646 (28.3%)** forward, **178/646 (27.6%)** replay (**5** forward-only, **0** replay-only).

**Top Forward-Only Functions** (`forward > 0 && replay == 0`):
- `split_double` (`final.c:2869-2986`): 5 lines (2932, 2951, 2957, 2959-2960)
- `app_disable` (`final.c:520-527`): 3 lines (520, 522, 527)

**Top Corpus-Uncovered Functions** (`forward == 0`):
- `final_scan_insn` (`final.c:1259-2028`): 109/199 lines uncovered (e.g. 1272, 1302-1303, 1316, 1328-1329, 1333, ... (+58 more))
- `end_final` (`final.c:307-501`): 71/71 lines uncovered (e.g. 307, 312, 315-316, 325, 342, ... (+39 more))
- `asm_fprintf` (`final.c:2719-2861`): 37/37 lines uncovered (e.g. 2719, 2730, 2737, 2739-2740, 2766-2767, ... (+14 more))
- `output_addr_const` (`final.c:2571-2706`): 32/53 lines uncovered (e.g. 2580-2582, 2584-2585, 2596-2599, 2617-2618, 2621-2622, ... (+13 more))
- `shorten_branches` (`final.c:642-824`): 21/69 lines uncovered (e.g. 696, 716-717, 726, 731, 768, ... (+11 more))

### `config/mips/mips.c` — Stage 0 (MIPS Backend)
- Lines: **1,019/1,784 (57.1%)** forward, **833/1,784 (46.7%)** replay (overlap **81.7%**, **186** forward-only, **0** replay-only).
- Branches: **857/1,855 (46.2%)** forward, **692/1,855 (37.3%)** replay (**166** forward-only, **1** replay-only).

**Top Forward-Only Functions** (`forward > 0 && replay == 0`):
- `gen_int_relational` (`config/mips/mips.c:1734-1895`): 42 lines (1734, 1776-1777, 1780-1781, 1783-1784, 1789, 1791, 1794, 1798, ... (+23 more))
- `gen_conditional_branch` (`config/mips/mips.c:1903-2030`): 27 lines (1903, 1959-1963, 1965-1966, 1968, 1975-1976, 1979, 1984, 1986-1987, ... (+10 more))
- `mips_address_cost` (`config/mips/mips.c:1600-1666`): 18 lines (1600, 1603, 1615, 1617-1619, 1622, 1625, 1631, 1633, ... (+6 more))
- `expand_block_move` (`config/mips/mips.c:2331-2424`): 14 lines (2331, 2334-2339, 2343, 2346, 2350-2351, 2353, 2361-2362)
- `map_test_to_internal_test` (`config/mips/mips.c:1688-1709`): 13 lines (1688, 1693, 1696-1705, 1708)

**Top Corpus-Uncovered Functions** (`forward == 0`):
- `output_block_move` (`config/mips/mips.c:2444-2739`): 68/152 lines uncovered (e.g. 2487-2491, 2501-2502, 2504-2505, 2507-2510, 2512, ... (+17 more))
- `print_operand` (`config/mips/mips.c:3563-3794`): 64/135 lines uncovered (e.g. 3574-3576, 3583-3585, 3587-3589, 3595-3597, 3611-3613, ... (+23 more))
- `override_options` (`config/mips/mips.c:3146-3466`): 53/154 lines uncovered (e.g. 3174-3175, 3181-3182, 3205-3216, 3233-3235, 3248-3253, ... (+14 more))
- `save_restore_insns` (`config/mips/mips.c:4587-4822`): 51/100 lines uncovered (e.g. 4603, 4626, 4635-4637, 4639, 4641, ... (+30 more))
- `function_epilogue` (`config/mips/mips.c:5072-5329`): 49/112 lines uncovered (e.g. 5103-5104, 5110-5112, 5117-5119, 5124-5126, 5131-5133, ... (+21 more))

### `reorg.c` — Stage 1 (DBR Schedule)
- Lines: **1,183/1,472 (80.4%)** forward, **1,158/1,472 (78.7%)** replay (overlap **97.8%**, **26** forward-only, **1** replay-only).
- Branches: **1,200/1,715 (70.0%)** forward, **1,193/1,715 (69.6%)** replay (**18** forward-only, **11** replay-only).

**Top Forward-Only Functions** (`forward > 0 && replay == 0`):
- `relax_delay_slots` (`reorg.c:3749-4050`): 13 lines (3791-3793, 3802-3804, 3806, 3808, 3812-3813, 3815-3816, 3818)
- `fill_simple_delay_slots` (`reorg.c:2800-3231`): 8 lines (3061, 3065-3067, 3069-3070, 3077-3078)
- `steal_delay_list_from_fallthrough` (`reorg.c:1700-1765`): 2 lines (1742-1743)
- `try_merge_delay_insns` (`reorg.c:1777-1931`): 1 lines (1840)
- `redundant_insn` (`reorg.c:1953-2141`): 1 lines (2053)

**Top Replay-Only Functions** (`forward == 0 && replay > 0`):
- `condition_dominates_p` (`reorg.c:1480-1502`): 1 lines (1501)

**Top Corpus-Uncovered Functions** (`forward == 0`):
- `dbr_schedule` (`reorg.c:4179-4397`): 81/81 lines uncovered (e.g. 4179, 4199, 4204, 4206-4209, 4213-4215, ... (+37 more))
- `make_return_insns` (`reorg.c:4059-4173`): 44/44 lines uncovered (e.g. 4059, 4063, 4069-4070, 4072-4073, 4078, ... (+19 more))
- `fill_simple_delay_slots` (`reorg.c:2800-3231`): 32/174 lines uncovered (e.g. 2821, 2832, 2859, 2863, 2871-2873, ... (+13 more))
- `relax_delay_slots` (`reorg.c:3749-4050`): 17/135 lines uncovered (e.g. 3867-3869, 3895, 3897, 3909, 3926-3928, ... (+6 more))
- `mark_referenced_resources` (`reorg.c:275-480`): 16/83 lines uncovered (e.g. 298, 326-328, 330, 333-334, 336-337, ... (+6 more))

### `jump.c` — Stage 2 (Jump2)
- Lines: **1,092/1,396 (78.2%)** forward, **621/1,396 (44.5%)** replay (overlap **56.9%**, **471** forward-only, **0** replay-only).
- Branches: **1,135/1,642 (69.1%)** forward, **666/1,642 (40.6%)** replay (**472** forward-only, **3** replay-only).

**Top Forward-Only Functions** (`forward > 0 && replay == 0`):
- `jump_optimize` (`jump.c:138-2117`): 208 lines (189, 244, 262, 264, 267-268, 270, 272-274, 278, ... (+88 more))
- `thread_jumps` (`jump.c:4161-4340`): 74 lines (4161, 4184-4188, 4190, 4194, 4197-4200, 4202-4203, 4205, 4207, ... (+28 more))
- `duplicate_loop_exit_test` (`jump.c:2131-2311`): 65 lines (2131, 2137, 2139, 2153, 2155-2157, 2159, 2180, 2182-2184, ... (+27 more))
- `rtx_equal_for_thread_p` (`jump.c:4349-4513`): 42 lines (4349, 4358, 4360, 4366, 4371-4374, 4376-4380, 4383, 4385-4386, ... (+18 more))
- `squeeze_notes` (`jump.c:2320-2353`): 17 lines (2320, 2326, 2328-2333, 2337, 2341-2347, 2352)

**Top Corpus-Uncovered Functions** (`forward == 0`):
- `jump_optimize` (`jump.c:138-2117`): 84/540 lines uncovered (e.g. 166-167, 225-226, 235, 279-281, 417-418, ... (+41 more))
- `rtx_equal_for_thread_p` (`jump.c:4349-4513`): 21/63 lines uncovered (e.g. 4375, 4410, 4414, 4417, 4420, ... (+10 more))
- `delete_computation` (`jump.c:3284-3384`): 20/29 lines uncovered (e.g. 3322, 3326, 3330, 3333-3334, 3339, ... (+8 more))
- `duplicate_loop_exit_test` (`jump.c:2131-2311`): 17/82 lines uncovered (e.g. 2164, 2174-2177, 2185, 2202-2203, 2234, ... (+4 more))
- `delete_insn` (`jump.c:3393-3535`): 15/64 lines uncovered (e.g. 3402, 3411, 3413-3416, 3484-3485, 3487-3491, ... (+2 more))

### `sched.c` — Stages 3 & 6 (Sched)
- Lines: **1,182/1,683 (70.2%)** forward, **1,266/1,683 (75.2%)** replay (overlap **99.9%**, **1** forward-only, **85** replay-only).
- Branches: **1,021/1,696 (60.2%)** forward, **1,073/1,696 (63.3%)** replay (**2** forward-only, **54** replay-only).

**Top Forward-Only Functions** (`forward > 0 && replay == 0`):
- `schedule_block` (`sched.c:3144-4048`): 1 lines (3578)

**Top Replay-Only Functions** (`forward == 0 && replay > 0`):
- `schedule_insns` (`sched.c:4666-4967`): 79 lines (4666, 4669, 4675, 4680, 4689, 4691-4692, 4700-4706, 4708, ... (+31 more))
- `update_n_sets` (`sched.c:4239-4264`): 3 lines (4256, 4258-4259)
- `attach_deaths` (`sched.c:2779-2965`): 2 lines (2901-2902)
- `memrefs_conflict_p` (`sched.c:614-783`): 1 lines (768)

**Top Corpus-Uncovered Functions** (`forward == 0`):
- `schedule_insns` (`sched.c:4666-4967`): 127/127 lines uncovered (e.g. 4666, 4669, 4675, 4680, 4689, ... (+52 more))
- `update_flow_info` (`sched.c:4271-4660`): 111/157 lines uncovered (e.g. 4299, 4301-4302, 4307-4312, 4315-4316, 4323-4325, ... (+52 more))
- `schedule_block` (`sched.c:3144-4048`): 72/353 lines uncovered (e.g. 3173, 3196, 3224, 3270-3271, 3359, ... (+32 more))
- `new_insn_dead_notes` (`sched.c:4138-4232`): 25/43 lines uncovered (e.g. 4149, 4165, 4171-4172, 4174-4178, 4198, ... (+7 more))
- `split_hard_reg_notes` (`sched.c:4089-4132`): 19/19 lines uncovered (e.g. 4089, 4097-4098, 4100, 4102, 4104, ... (+8 more))

### `global.c` — Stage 4 (Global Alloc)
- Lines: **412/513 (80.3%)** forward, **417/513 (81.3%)** replay (overlap **100.0%**, **0** forward-only, **5** replay-only).
- Branches: **454/652 (69.6%)** forward, **459/652 (70.4%)** replay (**0** forward-only, **5** replay-only).

**Top Replay-Only Functions** (`forward == 0 && replay > 0`):
- `mark_reg_clobber` (`global.c:1381-1432`): 3 lines (1415, 1417-1418)
- `find_reg` (`global.c:904-1193`): 1 lines (1071)
- `mark_reg_store` (`global.c:1317-1376`): 1 lines (1332)

**Top Corpus-Uncovered Functions** (`forward == 0`):
- `find_reg` (`global.c:904-1193`): 24/114 lines uncovered (e.g. 1011, 1013-1014, 1023, 1025-1026, 1035, ... (+9 more))
- `dump_conflicts` (`global.c:1647-1692`): 23/23 lines uncovered (e.g. 1647, 1652-1653, 1656-1659, 1661, 1666, ... (+4 more))
- `mark_reg_conflicts` (`global.c:1438-1471`): 15/15 lines uncovered (e.g. 1438, 1443-1444, 1446, 1449, 1451-1452, ... (+5 more))
- `global_alloc` (`global.c:277-581`): 12/115 lines uncovered (e.g. 314, 376-379, 381, 392, 395, ... (+4 more))
- `dump_global_regs` (`global.c:1695-1714`): 8/8 lines uncovered (e.g. 1695, 1701-1702, 1704-1705, 1710-1711, 1714)

### `reload.c` — Stage 4 (Reload)
- Lines: **946/1,965 (48.1%)** forward, **896/1,965 (45.6%)** replay (overlap **94.7%**, **50** forward-only, **0** replay-only).
- Branches: **1,228/3,196 (38.4%)** forward, **1,170/3,196 (36.6%)** replay (**60** forward-only, **2** replay-only).

**Top Forward-Only Functions** (`forward > 0 && replay == 0`):
- `push_reload` (`reload.c:725-1390`): 40 lines (822, 824-827, 840-841, 845-846, 852, 856, 894-898, 926-930, ... (+7 more))
- `refers_to_regno_for_reload_p` (`reload.c:5128-5238`): 10 lines (5168, 5171-5172, 5174-5175, 5177, 5179, 5190-5192)

**Top Corpus-Uncovered Functions** (`forward == 0`):
- `find_reloads` (`reload.c:2189-3872`): 205/575 lines uncovered (e.g. 2310, 2313-2314, 2318, 2320, 2322, ... (+113 more))
- `find_reloads_address_1` (`reload.c:4587-4934`): 119/119 lines uncovered (e.g. 4587, 4595, 4597, 4599, 4601-4604, ... (+72 more))
- `find_reloads_address` (`reload.c:4112-4405`): 88/107 lines uncovered (e.g. 4133, 4135-4136, 4141-4145, 4147, 4156-4160, ... (+39 more))
- `push_reload` (`reload.c:725-1390`): 87/247 lines uncovered (e.g. 748, 750, 781-786, 828-829, 848-850, ... (+40 more))
- `push_secondary_reload` (`reload.c:332-603`): 81/120 lines uncovered (e.g. 363-364, 367, 397, 401-402, 418, ... (+30 more))

### `reload1.c` — Stage 4 (Reload1)
- Lines: **1,327/2,045 (64.9%)** forward, **1,329/2,045 (65.0%)** replay (overlap **100.0%**, **0** forward-only, **2** replay-only).
- Branches: **1,435/2,628 (54.6%)** forward, **1,437/2,628 (54.7%)** replay (**0** forward-only, **2** replay-only).

**Top Replay-Only Functions** (`forward == 0 && replay > 0`):
- `reload_as_needed` (`reload1.c:3733-4001`): 2 lines (3840-3841)

**Top Corpus-Uncovered Functions** (`forward == 0`):
- `reload` (`reload1.c:446-2027`): 118/464 lines uncovered (e.g. 556, 566, 571-572, 575, 825-826, ... (+61 more))
- `emit_reload_insns` (`reload1.c:5641-6679`): 102/281 lines uncovered (e.g. 5721, 5730-5731, 5733, 5775, 5790, ... (+60 more))
- `eliminate_regs` (`reload1.c:2641-3155`): 77/185 lines uncovered (e.g. 2700, 2702, 2704-2705, 2737, 2773-2776, ... (+41 more))
- `choose_reload_regs` (`reload1.c:4903-5547`): 47/197 lines uncovered (e.g. 5174-5175, 5197, 5212, 5228-5229, 5238, ... (+32 more))
- `gen_reload` (`reload1.c:6688-6879`): 45/56 lines uncovered (e.g. 6700-6703, 6705-6708, 6737-6739, 6742, 6759-6760, ... (+24 more))

### `caller-save.c` — Stage 4 (Caller-Save)
- Lines: **208/222 (93.7%)** forward, **208/222 (93.7%)** replay (overlap **100.0%**, **0** forward-only, **0** replay-only).
- Branches: **215/268 (80.2%)** forward, **216/268 (80.6%)** replay (**0** forward-only, **1** replay-only).

**Top Corpus-Uncovered Functions** (`forward == 0`):
- `insert_save_restore` (`caller-save.c:646-770`): 5/37 lines uncovered (e.g. 665, 699, 703, 738, 742)
- `save_call_clobbered_regs` (`caller-save.c:349-509`): 3/55 lines uncovered (e.g. 499-501)
- `set_reg_live` (`caller-save.c:517-542`): 2/12 lines uncovered (e.g. 526-527)
- `restore_referenced_regs` (`caller-save.c:573-629`): 2/25 lines uncovered (e.g. 597-598)
- `init_caller_save` (`caller-save.c:98-209`): 1/47 lines uncovered (e.g. 144)

### `local-alloc.c` — Stage 5 (Local Alloc)
- Lines: **615/738 (83.3%)** forward, **667/738 (90.4%)** replay (overlap **99.5%**, **3** forward-only, **55** replay-only).
- Branches: **598/771 (77.6%)** forward, **614/771 (79.6%)** replay (**8** forward-only, **24** replay-only).

**Top Forward-Only Functions** (`forward > 0 && replay == 0`):
- `optimize_reg_copy_1` (`local-alloc.c:700-857`): 3 lines (767, 774, 825)

**Top Replay-Only Functions** (`forward == 0 && replay > 0`):
- `local_alloc` (`local-alloc.c:394-524`): 54 lines (394, 408, 413, 424-429, 431, 433-441, 443-444, 446, ... (+15 more))
- `reg_is_born` (`local-alloc.c:1976-2005`): 1 lines (1983)

**Top Corpus-Uncovered Functions** (`forward == 0`):
- `local_alloc` (`local-alloc.c:394-524`): 54/54 lines uncovered (e.g. 394, 408, 413, 424-429, 431, ... (+18 more))
- `block_alloc` (`local-alloc.c:1123-1566`): 17/166 lines uncovered (e.g. 1145, 1249-1253, 1261-1263, 1278-1279, 1331, ... (+2 more))
- `optimize_reg_copy_2` (`local-alloc.c:874-936`): 15/29 lines uncovered (e.g. 897, 903-904, 906, 908, 913-914, ... (+3 more))
- `optimize_reg_copy_1` (`local-alloc.c:700-857`): 9/59 lines uncovered (e.g. 787, 796, 802-804, 812-813, 844-845)
- `update_equiv_regs` (`local-alloc.c:947-1117`): 6/70 lines uncovered (e.g. 1107, 1109-1113)
