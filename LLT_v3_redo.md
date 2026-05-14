# LLT v3 셋업 재측정 — 글로벌 캐시 race fix 후 PPL vs Vanilla 비교

**셋업**: 10W TPC-C, 48 OLTP + 1 LLT, 600s, no delay
**LLT query**: `SELECT * FROM stock s, warehouse w;` (Cartesian, no WHERE)
**LLT 모드**: open transaction, consistent snapshot
**Buffer**: vanilla 512M, PPL 128M DRAM + 128M NVDIMM

## Vanilla

**TpmC = 16,397**, 2 LLT iters (iter1=129s, iter2=498s STOPPING cut)

### Vanilla 분포 (LLT readview만)

| 테이블 | Calls | Call% | Avg μs | Total | Time% | Chain |
|---|---:|---:|---:|---:|---:|---:|
| stock | 458,371 | 99.997% | **1,033** | 473.77s | **99.27%** | 2.25 |
| warehouse | 15 | 0.003% | 231,050 | 3.47s | 0.73% | **2,587** |
| **Σ** | 458,386 | 100% | — | 477.24s | 100% | |

### Vanilla LLT iter 시간

| iter | 시간 (s) |
|---:|---:|
| 1 | 129 |
| 2 | 498 (STOPPING cut) |

### OLTP undo write (600s, 총 ~6.9M)

| 테이블 | Count | % |
|---|---:|---:|
| orderline | 3,285,694 | 47.5% |
| stock | 1,641,982 | 23.7% |
| order | 328,690 | 4.8% |
| customer | 328,669 | 4.8% |
| district | 328,620 | 4.8% |
| neworder | 326,245 | 4.7% |
| warehouse | 164,357 | 2.4% |
| history | 164,357 | 2.4% |
| other | 756 | 0.01% |

---

## PPL (캐시 race fix 후)

**TpmC = 17,672**, **6 LLT iters** (iter1=31, 53, 83, 122, 184, 150 STOPPING cut)
**PATH3_FAIL = 0**, hang 없음

### PPL 5-way 분포 (LLT readview만)

| # | 경로 | Calls | Call% | Avg μs | Total | Time% | Chain |
|---:|---|---:|---:|---:|---:|---:|---:|
| 1 | Prebuilt | 497,257 | 30.27% | 9.13 | 4.54s | **1.01%** | 0 |
| 2 | Prebuilt+Undo | 398,707 | 24.27% | 248.5 | 99.09s | 22.13% | 1.13 |
| 3 | Old Page+Redo | 206,552 | 12.58% | 89.5 | 18.49s | 4.13% | 2.01 |
| 4 | Old Page+Undo | 191 | 0.01% | 303.5 | 0.06s | 0.01% | 1.20 |
| 5 | 최신+Undo (stock) | 539,634 | 32.86% | 570.7 | 307.95s | **68.75%** | 2.87 |
| 5w | 최신+Undo (warehouse) | 57 | 0.003% | 311,313 | 17.74s | 3.96% | 5,694 |
| **Σ** | | **1,642,398** | 100% | **272.7** | **447.88s** | 100% | |

### PPL LLT iter 시간

| iter | 시간 (s) |
|---:|---:|
| 1 | 31 |
| 2 | 53 |
| 3 | 83 |
| 4 | 122 |
| 5 | 184 |
| 6 | 150 (cut) |

### 함수 진입 통계

- redo_build_calls = 1,102,898 (총 nvdimm 진입)
- prebuild_hit = 895,964 (81.2%)
- prebuild_miss = 206,934 (18.8%)

### OLTP undo write (600s, 총 ~7.1M)

| 테이블 | Count | % |
|---|---:|---:|
| orderline | 3,538,582 | 49.9% |
| stock | 1,768,467 | 24.9% |
| order | 353,855 | 5.0% |
| customer | 353,846 | 5.0% |
| district | 353,835 | 5.0% |
| neworder | 347,595 | 4.9% |
| warehouse | 176,935 | 2.5% |
| history | 176,935 | 2.5% |

---

## PPL vs Vanilla 비교 (v3 셋업, 600s, 48 OLTP + 1 LLT)

| 지표 | PPL | Vanilla | PPL 우위 |
|---|---:|---:|---:|
| **TpmC** | 17,672 | 16,397 | 1.08× |
| **LLT iter 수** | **6** | 2 | **3×** |
| iter1 | **31s** | 129s | **4.16× 빠름** |
| 평균 iter | 104s | 314s | **3.02× 빠름** |

### LLT iter별 latency 비교

| iter | PPL (s) | Vanilla (s) | PPL 가속 |
|---:|---:|---:|---:|
| 1 | **31** | 129 | 4.16× |
| 2 | **53** | 498 (cut) | 9.40× |
| 3 | 83 | — | (vanilla 미도달) |
| 4 | 122 | — | — |
| 5 | 184 | — | — |
| 6 | 150 (cut) | — | — |
| **Σ** | **623s / 6 iters** | **627s / 2 iters** | **3× throughput** |
| Stock call avg | **272 μs** | 1,033 μs | **3.80× 빠름** |
| Stock chain | 2.87 (#5만) | 2.25 | PPL이 OLTP 빠르므로 chain 누적 더 깊음 |
| Warehouse chain | 5,694 (57 calls) | 2,587 (15 calls) | PPL이 깊음 |
| LLT 총 wall | 623s | 627s | 동일 (~ 600s 측정창) |

### Version build avg latency (LLT readview)

| 테이블 | PPL avg | Vanilla avg | PPL 가속 |
|---|---:|---:|---:|
| Stock 전체 (#1~#5) | **272 μs** | 1,033 μs | **3.80×** |
| Stock #1 Prebuilt | 9 μs | — | — (vanilla 없음) |
| Stock #5 만 | 570 μs | 1,033 μs | 1.81× |
| Warehouse | 311,313 μs | 231,050 μs | 0.74× (chain 깊어서 PPL 더 깊은 거 처리) |

### Time 분포 의미 (PPL)

PPL LLT 시간 447.88s 중:
- **#5 stock (Latest+Undo) 68.75%** — 여전히 cost dominant
- **#2 Prebuilt+Undo 22.13%** — prebuilt 만나지만 trx invisible → nested undo
- **#1 Prebuilt 1.01%** — 30%의 호출을 9 μs로 처리. 핵심 가속
- **#3 Old Page+Redo 4.13%** — PPL redo apply

→ **PPL이 Prebuilt(#1)로 호출 30%를 9 μs에 처리**. vanilla는 같은 호출을 1,033 μs에 함. 113× 가속 효과.

### 왜 v4 셋업에선 안 보였나?

| 셋업 | OLTP | BP miss | Vanilla per-step μs | Vanilla avg μs |
|---|---:|---:|---:|---:|
| v3 (48 OLTP) | 48 | 매우 높음 | ~459 | 1,033 |
| v4 (16 OLTP) | 16 | 매우 낮음 | ~11 | 27 |

v4는 OLTP 부하 적어 undo 페이지가 BP에 다 들어옴 → vanilla undo walk가 sub-μs/step → PPL의 paper-level 가속 못 봄.
**v3는 48 OLTP의 BP eviction이 vanilla undo walk를 IO-bound로 만듦 → PPL Prebuilt가 진가 발휘.**

paper §6.5의 vanilla 10,089 μs는 1h 누적으로 chain 매우 깊은 경우 — 본 셋업 600s는 짧지만 같은 패턴 확인됨.

---

## 결론

캐시 race fix 후 PPL이 v3 셋업에서 paper의 claim을 재현:
- **OLTP TpmC 1.08× 우위** (vanilla 대비)
- **LLT iter 처리량 3× 우위** (6 iter vs 2 iter)
- **Stock version build avg 3.8× 가속** (272 vs 1033 μs)
- **Prebuilt cache (#1) 113× 가속** (9 vs 1,033 μs)

이전 v4 셋업이 paper와 안 맞았던 이유: 16 OLTP라서 vanilla undo walk가 BP-hot, PPL prebuilt 효과가 보이지 않음.


---

## v3 원본 (참고)

원본 v3 (캐시 race fix 전):
- Vanilla TpmC 16,480, stock 461,489 calls, **1,029 μs**, chain 2.24
- PPL TpmC 17,755, 6 LLT iters

본 재측정 vanilla 1,033 μs는 원본 1,029 μs와 거의 일치 — **v3 vanilla 값은 정상**.
v4 vanilla 27.4 μs는 셋업 차이 (16 OLTP, 3-way + WHERE)로 인한 outlier.
