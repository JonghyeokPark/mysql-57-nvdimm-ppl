# LLT v3 — OPPL on (snapshot+chunks + O_DIRECT + 3-way 분기)

**셋업**: 10W TPC-C, 48 OLTP + 1 LLT, 600s, no delay
**LLT query**: `SELECT * FROM stock s, warehouse w;` (Cartesian, open trx loop)
**OPPL**: prebuilt cache OFF, snap 4KB + chunks ≤4KB per stock page, O_DIRECT 8KB pread

---

## TpmC + LLT iter 시간 (r11)

| iter | OPPL on (s) |
|---:|---:|
| 1 | **40** |
| 2 | **42** |
| 3 | **52** |
| 4 | **60** |
| 5 | **66** |
| 6 | **74** |
| 7 | **82** |
| 8 | **95** |
| 9 | 99 (cut) |
| **총 iter 수** | **9** |
| **wall** | ~610s |
| **TpmC** | **18,727** |

LLT 시간 monotonic 증가 (40→99s) — view 고정 + OLTP commit 누적으로 invisible-row 비율 증가 → version build 호출 횟수 증가가 원인. per-call 비용은 일정 (Path C avg 95μs).

---

## OPPL 6-way 분포 (LLT readview만)

| 경로 | Calls | Call% | Avg μs | Total | Time% | Chain |
|---|---:|---:|---:|---:|---:|---:|
| **1. Snap (visible 그대로, Branch 1)** | 2,459,927 | 87.38% | **95.2** | 234.11s | **73.83%** | 0 |
| **2. Snap + Redo (Branch 3)** | 25,863 | 0.92% | 104.9 | 2.71s | 0.85% | 2.03 |
| **3. .ibd + NVDIMM Redo** (non-OPPL Path C) | 319,228 | 11.34% | 92.2 | 29.43s | 9.28% | 0.002 |
| **4. Snap + Undo (Branch 2)** | 4,463 | 0.16% | 1,157.5 | 5.17s | 1.63% | — |
| **5. .ibd + Undo** (page too new) | 71 | 0.003% | 120.1 | 0.009s | 0.003% | 0 |
| **6. Latest + Undo (stock)** | 5,870 | 0.21% | 1,366.4 | 8.02s | 2.53% | 4.58 |
| Warehouse | 88 | 0.003% | 427,575 | 37.63s | 11.87% | 7,235 |
| **Σ** | **2,815,510** | 100% | **113** | **317.08s** | 100% | |

**경로 해석**:
- 1. Snap = snap.rec.trx_id ≤ view AND chunks 첫 entry > view → snap 직접 반환 (apply 0회)
- 2. Snap+Redo = snap 위에 chunks visibility-gated forward apply
- 3. .ibd+NVDIMM Redo = OPPL entry 없는 페이지 (fil_io + 현재 NVDIMM PPL chain apply)
- 4. Snap+Undo = snap.rec invisible → snap에서 undo back
- 5. .ibd+Undo = old_page max_trx > view → undo from disk rec
- 6. Latest+Undo = OPPL 실패 후 caller가 BP latest에서 vanilla undo

---

## 핵심 관찰

| 항목 | 값 | 의미 |
|---|---:|---|
| Snap 단독 비중 (Branch 1) | **87.4% calls / 73.8% time** | 대부분 chunks도 안 적용하고 그대로 |
| OPPL Snap+Redo 호출 | 25,863 (0.92%) | chunks가 view 끼고 있을 때만 fire |
| OPPL Snap+Undo 호출 | 4,463 (0.16%) | snap이 view보다 새로울 때 (drop) |
| Latest+Undo fallback | 5,870 (0.21%) | OPPL 다 실패 후 caller 처리 |
| Path C avg μs (Snap) | **95.2** | per-call 비용 일정 |
| Total avg μs | 113 | (vanilla 911 대비 8×) |
| Warehouse 시간 비중 | 11.87% | OPPL 미적용, 시간 따라 증가 |

oppl.dat = 669MB (단발 capture, 8KB × ~84K entries)
trace count: OPPL=CAPTURE/READ_PATH/DRAIN 합쳐 341건 (1k-throttled stderr)

---

## LLT 시간이 늘어나는 이유 (정리)

```
LLT 시간 = call_count(t) × per_call_cost(t)
   OPPL:  per_call_cost 일정 (~95μs)
          call_count(t) ∝ "invisible-latest row 비율" — t 따라 ↑
```

view가 고정된 long-running trx이므로 OLTP commit 누적될수록 BP latest의 rec.trx_id > view 비율 증가 → version build 호출 빈도 증가. OPPL은 호출당 비용은 못 늘게 막아주지만 호출 횟수 자체는 못 막음.

호출 횟수까지 막으려면 LLT row scan을 **BP latest 우회 + snap에서 시작**하는 별도 read path 필요 (별도 BP / LLT 전용 fetch).
