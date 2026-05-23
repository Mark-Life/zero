# RESOLVED: quantized temp>0 parity divergence was a libm (`expf`) mismatch

**Status:** RESOLVED 2026-05-23. **Not a Zero bug** — no kernel or `forwardQ`
change. The fault was in `examples/llama2/validate.sh`: it built the C reference
with the **container's Alpine gcc/musl**, while the Zero exe statically links
**zig's bundled musl**. Those two musl builds' `expf` (and `sinf`/`cosf`/`powf`/
`sqrtf`) differ by ~1 ULP, which is enough to flip sampled tokens. Fix: build the
reference with the same `zig cc` toolchain the Zero exe links, so both sides
share one libm. The full f32 + int8 (temp 0, temp>0, top-p) matrix then matches
`run.c`/`runq.c` token-for-token.

The original open-bug write-up is preserved below §"Original handoff" for context;
§1–§3 here record the actual root cause, evidence, and fix.

---

## 1. Root cause

- Zero's int8 kernels are **bit-exact** vs `runq.c`: a pos-0 bisection confirmed
  `dequantRow` (embedding), `quantizeActs`, and every `matmulQ` (q/k/v/wo/w1/w3,
  and the shared classifier) produce byte-identical f32 to the reference.
- The **only** difference is libm. The Zero exe links zig's musl; `validate.sh`
  built the reference with Alpine's musl. On a single layer-0 forward, `expf`
  disagreed on **72 / 768** FFN activations, each by **exactly 1 ULP**. The
  Alpine result was the correctly-rounded f32 in all 72 cases; zig's musl `expf`
  was 1 ULP off (faithfully but not correctly rounded; 43 low / 29 high).
- Why temp 0 *mostly* survived but temp>0 *always* diverged: argmax absorbs a
  sub-ULP perturbation of the logits, but multinomial / top-p sampling turns it
  into a different drawn token, after which the sequences decorrelate.
- Why the **f32** path "passed" while int8 failed: identical libm skew is present
  in *both* paths (same shared kernels, same `expf`). The f32 path simply has
  wider logit margins on the tested prompts and never flipped a token — it passed
  by luck, not by bit-exactness. Once the reference uses the matching libm, the
  f32 path is bit-exact too.

## 2. How it was found (runtime numerical bisection)

A throwaway probe (`.zero/probe/llama2-dump/`, a copy of the example with a
truncated `forwardQ`) dumped raw f32 buffers from Zero; `runq.c` was patched
(`.zero/llama2-data/runq_dump.c`) to dump the same buffers; both ran on the same
`stories15M_q80.bin` under the amd64 container and were diffed offline.

1. **pos-0 logits differ** (22294/32000, ~1 ULP) with identical argmax → a
   single-forward bug (every int8 kernel runs in forward 0), so bisect *within*.
2. **embedding** `x` (dequantRow) — bit-identical (real f32-vs-f32).
3. **per-layer residual trace** — first divergence appears after **layer 0**.
4. **layer-0 sub-step snapshots** — identical through `xb_attn`, `wo`, the attn
   residual, `w1`, and `w3`; first divergence is the **`swiglu` output**.
5. `swiglu` *inputs* are identical but its output differs → isolate the one
   transcendental: dump `expf(-hb[i])` from both → **72/768 differ by 1 ULP**.
   Compared against the f64 `exp` truth: Alpine `expf` correctly-rounded, zig's
   1 ULP off → **libm mismatch**, not a Zero kernel issue.
6. **Confirm fix:** rebuild `runq.c` with `zig cc -target x86_64-linux-musl`
   (the toolchain the Zero exe links) → Zero **matches** it token-for-token, and
   **differs** from the Alpine-gcc build. Full matrix passes.

(The `1.0 + e` / `1.0 / denom` bare-float literals in `swiglu` were also checked
— Zero defaults bare float literals to f64 — but an f32-strict rewrite produced
*identical* output, so they are not involved here and were left unchanged.)

## 3. The fix

`examples/llama2/validate.sh` now compiles `run.c` and `runq.c` with
`zig cc -target x86_64-linux-musl` (a `build_ref` helper) instead of the
container's `apk add gcc && gcc`. Both binaries are static linux-musl-x64 ELFs
that link the *same* zig-bundled musl as the Zero exe; the amd64 Alpine container
is now used only to *run* them (qemu on non-x64 hosts). Requires `zig` on the
host (already required, since `zero build --target linux-musl-x64` shells out to
`zig cc`).

Note on the CI fixture (`conformance/native/pass/generate-argmax-q.0`): it is
argmax on a tiny model with a 0.151 winner margin — robust to libm ULP noise *by
design*, so it cannot catch a sampling-level divergence, and a deterministic
temp>0 fixture would itself be libm-fragile. The real regression gate is
`validate.sh` now building the reference against the shared libm.

---

## Original handoff (for context; the suspects below were all cleared)

The reproduction recipe in §4 still applies (how to make a `stories15M_q80.bin`
without PyTorch). The "remaining suspects" the original doc listed —
`quantizeActs`, `matmulQ`, the shared-classifier path, MHA-vs-GQA — were each
verified bit-exact during the bisection; none was the cause.

### How to reproduce / make the quantized model (no PyTorch needed)

`validate.sh` needs `stories15M_q80.bin` in `$LLAMA2_DATA` (default
`.zero/llama2-data/`). Without torch, convert the **f32** `stories15M.bin`
directly — both engines read the same bytes, so this is a valid parity input.
GS=32 divides both inner matmul dims (dim 288, hidden 768); stories15M is a
shared-classifier model. Save the script as a throwaway, run
`python3 f32_to_q80.py stories15M.bin stories15M_q80.bin`. Expected size:
**17,101,696 bytes** (256 header + 14,976 f32 rmsnorm + 17,086,464 quant/scale).

```python
import struct, sys
from array import array

src, dst = sys.argv[1], sys.argv[2]
with open(src, 'rb') as f:
    dim, hidden, n_layers, n_heads, n_kv_heads, vocab, seq_len = struct.unpack('<7i', f.read(28))
    shared = vocab > 0
    vocab = abs(vocab)
    head_size = dim // n_heads
    kv_dim = n_kv_heads * head_size
    rest = f.read()
w = array('f'); w.frombytes(rest)
GS = 32

pos = 0
def take(n):
    global pos
    s = w[pos:pos+n]; pos += n
    return s

tok   = take(vocab * dim)
rmsa  = take(n_layers * dim)
wq    = take(n_layers * dim * dim)
wk    = take(n_layers * dim * kv_dim)
wv    = take(n_layers * dim * kv_dim)
wo    = take(n_layers * dim * dim)
rmsf  = take(n_layers * dim)
w1    = take(n_layers * hidden * dim)
w2    = take(n_layers * dim * hidden)
w3    = take(n_layers * hidden * dim)
rmsfin= take(dim)
take(seq_len * head_size // 2)   # freq_cis_real (unused)
take(seq_len * head_size // 2)   # freq_cis_imag (unused)

def quantize(t):
    qb = bytearray(); sc = array('f'); n = len(t); g = 0
    while g < n:
        grp = t[g:g+GS]
        wmax = 0.0
        for v in grp:
            a = v if v >= 0 else -v
            if a > wmax: wmax = a
        scale = wmax / 127.0
        sc.append(scale)
        if scale == 0.0:
            qb.extend(b'\x00' * GS)
        else:
            for v in grp:
                q = int(round(v / scale))
                q = max(-127, min(127, q))
                qb.append(q & 0xFF)
        g += GS
    return bytes(qb), sc.tobytes()

out = bytearray()
out += struct.pack('<I', 0x616b3432)   # "ak42"
out += struct.pack('<i', 2)
out += struct.pack('<7i', dim, hidden, n_layers, n_heads, n_kv_heads, vocab, seq_len)
out += struct.pack('<B', 1 if shared else 0)
out += struct.pack('<i', GS)
out += b'\x00' * (256 - len(out))
out += rmsa.tobytes(); out += rmsf.tobytes(); out += rmsfin.tobytes()

def emit(t, count, se):
    for l in range(count):
        q, s = quantize(t[l*se:(l+1)*se])
        out.extend(q); out.extend(s)

emit(tok, 1, vocab * dim)
emit(wq, n_layers, dim * dim)
emit(wk, n_layers, dim * kv_dim)
emit(wv, n_layers, dim * kv_dim)
emit(wo, n_layers, dim * dim)
emit(w1, n_layers, dim * hidden)
emit(w2, n_layers, hidden * dim)
emit(w3, n_layers, dim * hidden)
# shared classifier => wcls aliases tok; nothing more to emit.

open(dst, 'wb').write(out)
print(f"wrote {dst}: {len(out)} bytes, GS={GS}, shared={shared}")
```

### Pointers

- `examples/llama2/src/ops.0` — `matmulQ`, `quantizeActs`, `dequantRow` (all
  verified bit-exact vs `runq.c`); `swiglu` (calls `expf`).
- `examples/llama2/validate.sh` — `build_ref` (the fix: zig-cc reference build).
- Reference: `runq.c` from `karpathy/llama2.c` master. `validate.sh` fetches it.
