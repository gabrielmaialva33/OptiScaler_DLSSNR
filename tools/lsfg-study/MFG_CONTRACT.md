# LSFG Multi-Frame Generation (MFG x3, x4) Presenter/Generator Contract

This document specifies the contract between the **Presenter** (the swapchain/pacing engine forking FFX's swapchain for $N$ frames) and the **Generator** (`retoque/lsfg/Lsfg.{h,cpp}` and `LsfgFrameGen_Dx12.{h,cpp}`).

---

## 1. Overview & Multiplier Math

For an $M\times$ frame generation multiplier ($M = 2, 3, 4$), LSFG produces $K = M - 1$ intermediate generated frames between base frame pair $(I_{t-1}, I_t)$:
- **$2\times$ ($K = 1$ generated frame):** $t = 1/2 = 0.50$
- **$3\times$ ($K = 2$ generated frames):** $t_0 = 1/3 \approx 0.3333$, $t_1 = 2/3 \approx 0.6667$
- **$4\times$ ($K = 3$ generated frames):** $t_0 = 1/4 = 0.25$, $t_1 = 2/4 = 0.50$, $t_2 = 3/4 = 0.75$

General timestamp formula for 0-based index $k \in [0, K - 1]$:
$$t_k = \frac{k + 1}{K + 1} = \frac{k + 1}{M}$$

---

## 2. API Signatures

The presenter side can consume LSFG generation using either **Sequential Mode** or **Batch Mode**:

### A. Sequential Mode (`RecordGenerated`)

Ideal for presenters that schedule generated frames across separate VBlanks / presentation intervals on separate command lists:

```cpp
bool Lsfg::RecordGenerated(
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* presentColor,
    D3D12_RESOURCE_STATES presentState,
    uint32_t index,
    uint32_t count,
    ID3D12Resource* output,
    D3D12_RESOURCE_STATES outputState,
    bool reset = false,
    uint64_t frameId = 0
);
```

#### Parameters:
- `list`: Graphics command list (DIRECT or COMPUTE) on which to record dispatches and barriers.
- `presentColor`: The game's backbuffer resource ($I_t$). Ingested only when `index == 0`.
- `presentState`: Current state of `presentColor` (e.g. `D3D12_RESOURCE_STATE_PRESENT` or `D3D12_RESOURCE_STATE_COMMON`). Restored before return.
- `index`: 0-based index of this generated frame ($0 \le \text{index} < \text{count}$).
- `count`: Total generated frames between this base frame pair ($K = M - 1$).
- `output`: Destination resource receiving the interpolated frame.
- `outputState`: Resource state to leave `output` in upon completion (e.g. `D3D12_RESOURCE_STATE_PRESENT` or `D3D12_RESOURCE_STATE_COMMON`).
- `reset`: Set to true on scene cuts, camera cuts, or resize.
- `frameId`: Incremental game frame counter (used to detect stream gaps).

#### Return Value:
- `true`: Generated frame recorded successfully into `output`.
- `false`: LSFG is disabled, not ready (building on worker), or warming up (after reset / first frame). The presenter should present the real frame or fallback to FFX interpolation.

---

### B. Batch Mode (`RecordBatch` or `FfxDispatch`)

Ideal for presenters that record all $K$ generated frames into $K$ output textures in a single command list:

```cpp
bool Lsfg::RecordBatch(
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* presentColor,
    D3D12_RESOURCE_STATES presentState,
    uint32_t count,
    ID3D12Resource* const* outputs,
    const D3D12_RESOURCE_STATES* outputStates,
    bool reset = false,
    uint64_t frameId = 0
);
```

Alternatively, if driving through the standard FFX dispatch descriptor:
```cpp
params->numGeneratedFrames = K; // 1, 2, or 3
params->outputs[0] = ...;
params->outputs[1] = ...;
uint32_t res = Lsfg::FfxDispatch(params);
```

---

## 3. Work Distribution & State Machine

### Execution Lifecycle for One Base Frame Pair

```text
Game presents frame I_t
   │
   ├─► index == 0 (First Generated Frame, t = 1/(K+1)):
   │     1. Ingest: Copy presentColor -> sourceImageArray[slot]
   │     2. Pre-Pass (34 dispatches, Stages 0..9):
   │        - mipmaps: 7-level luminance pyramid
   │        - alpha0..3: 7-scale feature pyramid (pinned in alphaArray[0..6])
   │        - beta0..4: motion candidate guidance (pinned in betaImageArray)
   │     3. Main-Pass (66 dispatches, Stages 10..45):
   │        - gamma0..4, delta0..4, epsilon0..4, generate
   │        - Evaluated with timestamp t_0 = 1/(K+1)
   │        - Writes output_0
   │
   ├─► index == 1 (Second Generated Frame, t = 2/(K+1)):
   │     1. Ingest SKIPPED (I_{t-1} and I_t remain in sourceImageArray)
   │     2. Pre-Pass SKIPPED (alphaArray and betaImageArray REUSED directly)
   │     3. Main-Pass (66 dispatches, Stages 10..45):
   │        - Evaluated with timestamp t_1 = 2/(K+1)
   │        - Writes output_1
   │
   ├─► index == 2 (Third Generated Frame for x4, t = 3/(K+1)):
   │     1. Ingest & Pre-Pass SKIPPED
   │     2. Main-Pass evaluated with t_2 = 3/(K+1) -> writes output_2
   │
   └─► index == count - 1 (Final Generated Frame):
         - Advance history counter (`SourceSlot()` flips for the NEXT base frame pair)
```

---

## 4. Resource State & Barrier Guarantees

1. **`presentColor`:**
   - Ingested on `index == 0` via `CopyTextureRegion`.
   - Restored to `presentState` in the barrier batch before the first compute pass runs.
2. **`output`:**
   - Transitioned to `D3D12_RESOURCE_STATE_UNORDERED_ACCESS` for compute writes (or written via DirectOutput format).
   - Fully resolved and transitioned to `outputState` before `RecordGenerated` returns.
3. **Internal Pinned Resources (`alphaArray`, `betaImageArray`):**
   - Remain in `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE` at the end of the pre-pass and stay valid across all subsequent `index > 0` dispatches.
4. **Intra-stage Concurrency:**
   - All dispatches within a single stage operate on disjoint subresources and execute concurrently.

---

## 5. Cost Model & Amortization

| Multiplier ($M$) | Generated Frames ($K$) | Pre-Pass Cost ($C_{pre}$) | Main-Pass Cost ($C_{main}$) | Total Cost | Avg Cost / Gen Frame |
|---|---|---|---|---|---|
| **$2\times$** | 1 | $0.32\text{ ms}$ | $1 \times 0.48\text{ ms}$ | **$0.80\text{ ms}$** | $0.80\text{ ms}$ |
| **$3\times$** | 2 | $0.32\text{ ms}$ | $2 \times 0.48\text{ ms}$ | **$1.28\text{ ms}$** | $0.64\text{ ms}$ ($-20\%$) |
| **$4\times$** | 3 | $0.32\text{ ms}$ | $3 \times 0.48\text{ ms}$ | **$1.76\text{ ms}$** | $0.59\text{ ms}$ ($-26\%$) |

*Measured in-engine on an RTX 4090 at 1440p Quality FP32.*

---

## 6. Reference Verification Data

Ground truth frames generated from `lsfg-vk-cli debug` are available in `Tmp/lsfg-study/`:
- `ref/`: $2\times$ reference frames (`1.dds` at $t = 0.50$).
- `ref_m3/`: $3\times$ reference frames:
  - `1_1.dds` ($t = 1/3$)
  - `1_2.dds` ($t = 2/3$)
- `ref_m4/`: $4\times$ reference frames:
  - `1_1.dds` ($t = 0.25$)
  - `1_2.dds` ($t = 0.50$, byte-identical to $2\times$ reference)
  - `1_3.dds` ($t = 0.75$)
