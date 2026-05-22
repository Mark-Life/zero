# Open bug: quantized **temp>0** parity divergence vs `runq.c` on real models

**Status:** OPEN — discovered 2026-05-22 while validating Q-opt; **not yet fixed**.
**Scope:** the int8 (runq.c "version 2") path only. The f32 path is unaffected.
**Severity:** blocks the upstream PR's claim of token-for-token int8 parity for any
sampled (temperature > 0) generation. Greedy (temp 0) generation is *mostly* fine.

This doc is a self-contained handoff so a future agent can finish the diagnosis and
fix without re-deriving the context. Read [`quantization.md`](./quantization.md) (the
G + Q-opt plan) first for the format and kernel background.

---

## 1. Symptom

Running `examples/llama2/validate.sh` against a **real** quantized `stories15M_q80.bin`
(see §4 for how to make one without PyTorch) gives:

| Matrix | Result |
|--------|--------|
| f32 — temp 0, temp>0, top-p (vs `run.c`) | **all PASS, token-for-token** |
| int8 — temp 0 / argmax (vs `runq.c`) | **mostly pass**: 3/4 prompts byte-identical; one long prompt ("Lily and Tom went to the park", 120 tok) flips |
| int8 — temp>0 multinomial (vs `runq.c`) | **ALL diverge** |
| int8 — temp>0 + top-p (vs `runq.c`) | **ALL diverge** |

The divergence is **gradual**: the two outputs agree for ~30–40 tokens, then one
sampled token flips and everything after differs. Example (`-t 1.0 -s 12345`,
prompt "The dragon"):

```
zero: ... carrying a whip like a sword, so noone scared him. The dragon kept looking around ...
runq: ... carrying a whip like a sword, so one of the tornadoes was in the clouds. The dragon dragged ...
                                          ^ first divergent token (~token 30)
```

This is the fingerprint of a **ULP-level difference in the quantized logits**: argmax
is robust to it (most temp-0 runs survive 100+ steps), but multinomial/top-p sampling
turns a sub-ULP probability difference into a different drawn token, after which the
sequences decorrelate completely.

## 2. Critical facts (established, do not re-litigate)

1. **The f32 path is perfect.** zero's f32 `forward` matches `run.c` token-for-token on
   all temp 0 / temp>0 / top-p cases. So the shared f32 kernels (`rmsnorm`, `rope`,
   `softmax`, attention, `swiglu`, residual adds) and the sampler are correct vs the
   reference. The bug is in the **int8-specific** path.

2. **It is PRE-EXISTING in item G — NOT introduced by Q-opt.** Proven decisively:
   `git stash` the Q-opt changes, rebuild the committed-G compiler + example, run the
   same q80 model — the committed-G binary produces **byte-identical** divergent output
   to the Q-opt binary. Q-opt only swapped the arithmetic int8 decode for a `movsbl`
   load and packed activations into `i8`; that is value-preserving (the decoded weight
   value and the activation quants are bit-identical either way), so it cannot be the
   cause.

3. **It was never caught before** because G's quantized branch in `validate.sh`
   *self-skips* when `stories15M_q80.bin` is absent, and no q80 model had ever been
   produced (needs `export.py` + PyTorch, or the §4 converter). The CI fixture
   `conformance/native/pass/generate-argmax-q.0` only tests **argmax on a tiny model**
   (dim 4, 2 layers) and is bit-exact there — it is too small/short to surface a
   ULP-level accumulation, and it never exercises the sampler.

4. **Both engines read the identical q80 bytes.** The §4 converter's quantization
   quality is therefore irrelevant to *parity* — whatever int8 weights/scales the file
   holds, `zero` and `runq_ref` both read the same ones. A divergence between the two
   engines is purely a **difference in computation**, not in the data.

## 3. What has been ruled out (with evidence)

- **`roundHA` (activation-quant rounding).** `ops.0`'s `roundHA` uses
  `floorf(v + 0.5)`, which is genuinely *not* bit-equal to C `round()` for a float just
  below `n.5` (the float sum rounds up across the integer boundary). It was rewritten to
  a bit-exact frac-compare (`fl = floorf(v); frac = v - fl; frac >= 0.5 ? fl+1 : fl`) and
  the real-model output **did not change at all** → this model's activations never land
  on that boundary, so `roundHA` is not the cause here. (The rewrite was reverted to keep
  Q-opt value-preserving, but the `floorf(v+0.5)` justification comment in `ops.0` is
  mathematically wrong and the imprecision is a real latent bug worth fixing anyway.)
- **The sampler.** `run.c` and `runq.c` have **byte-identical** `softmax`,
  `sample_mult`, and `sample_topp` (verified by `diff`). Since the f32 sampler parity
  passes, zero's sampler is correct vs both. Not the cause.
- **The forward structure.** `diff`'ing `run.c`'s `forward` against `runq.c`'s shows the
  only differences are (a) `matmul` → `quantize` + quantized `matmul`, and (b) the
  key/value vectors are computed into scratch then `memcpy`'d into the kv cache *after*
  rope, vs run.c writing straight into the cache — which is **mathematically
  equivalent** (same values land in the cache). zero uses the run.c style (kv as cache
  sub-views); not a numerical difference.
- **`rmsnorm`** is byte-identical between `run.c` and `runq.c`.
- **The quantized kernels' source.** `matmulQ` (per-group int32 accumulate, reset,
  `(ivalf * wscale) * xscale` left-to-right), `quantizeActs` (`scale = wmax/127`,
  `round(x/scale)`), and `dequantRow` (`q[i] * s[i/gs]`) all match `runq.c`'s
  `matmul`/`quantize`/`dequantize` line-for-line, and are bit-exact vs the
  runq.c-derived oracle on the tiny fixture.
- **FMA / `-ffp-contract`.** Ruled out indirectly: if `gcc -O3` were forming FMAs in the
  reference, the *f32* matmul would diverge too (it has the same `val += a*b` shape) —
  but f32 parity is perfect. So the emulated/target CPU isn't contracting, and the
  quantized matmul isn't either.

## 4. How to reproduce (no PyTorch needed)

`validate.sh` needs `stories15M_q80.bin` in `$LLAMA2_DATA` (default
`.zero/llama2-data/`). The canonical way is `export.py --version 2` (needs torch +
`stories15M.pt`). Without torch, convert the **f32** `stories15M.bin` directly — both
engines read the same bytes, so this is a valid parity input (see §2.4). GS=32 divides
both inner matmul dims (dim 288, hidden 768); stories15M is a shared-classifier model.

Save as a throwaway (e.g. `.zero/probe/f32_to_q80.py`), run
`python3 f32_to_q80.py stories15M.bin stories15M_q80.bin`, then `bash
examples/llama2/validate.sh`. Expected output size for stories15M: **17,101,696 bytes**
(256 header + 14,976 f32 rmsnorm + 17,086,464 quant/scale blocks).

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

### Focused one-shot comparison (faster than the whole matrix)

```sh
# build runq_ref once (validate.sh also does this):
#   gcc -O3 -o runq_ref runq.c -lm   (runq.c fetched from karpathy/llama2.c master)
docker run --rm --platform linux/amd64 -v "$(pwd)":/work -w /work/.zero/llama2-data alpine sh -c '
  /work/.zero/out/llama2 stories15M_q80.bin --prompt "The dragon" --tokens 100 --temperature 1.0 > z.txt
  ./runq_ref stories15M_q80.bin -z tokenizer.bin -t 1.0 -p 0 -s 12345 -n 100 -i "The dragon" > r.txt
  diff z.txt r.txt
'
```

Zero's PRNG seed is hard-coded 12345 (see `sampler.0`); pass `-s 12345` to runq_ref, and
`-p 0` to disable runq's default top-p when testing plain multinomial.

## 5. Remaining suspect & how to investigate

The difference is a **sub-ULP perturbation of the quantized logits** that argmax mostly
absorbs. Everything in §3 matched on inspection, so the next step is **runtime
numerical bisection** — find *where* the first divergence appears, not *what looks*
different in source.

The obstacle: Zero has no float formatter, so you can't `printf` logits from the example
directly. Options, roughly in order of effort:

1. **Dump raw f32 bytes from both engines and diff offline.** Add a temporary
   `world.out.write` of the raw `Span<u8>` view over a chosen activation buffer (e.g.
   `logits`, or `xb` after layer 0) in `forwardQ` for `pos == 0` only, and the
   equivalent `fwrite(s->logits, sizeof(float), …)` in a local copy of `runq.c`. Run
   both on the **same single token** (no sampling), compare the f32 arrays element-wise
   to find the first differing element and its magnitude (1 ULP? more?).
2. **Bisect by layer.** Once you know pos-0 logits differ, dump the residual stream `x`
   after each layer to find the first layer that diverges, then dump each sub-step
   (after `quantizeActs`, after each `matmulQ`, after attention) within that layer.
3. **Bisect by op.** The prime suspects, given §3 cleared the obvious ones:
   - **`quantizeActs` producing different int8 quants** for some group — even one quant
     off by 1 perturbs a dot product. Dump `xq.q`/`xq.s` after a `quantizeActs` call and
     compare to runq's `s->xq.q`/`s->xq.s`. (roundHA was cleared for the *boundary* case,
     but check `wmax`/`scale` agree bit-for-bit; note runq.c's `wmax` is accumulated as
     `float val = fabs(double)` — confirm zero's `absf` path matches for every value.)
   - **`matmulQ` float accumulation** — confirm the `(float)ival * ws * xs` chain and the
     `val` running sum are bit-identical (they look identical in source; verify in
     emitted code that zero isn't, e.g., keeping an intermediate in a wider register).
   - **The shared classifier path** (stories15M is shared; the tiny fixture is
     *unshared*, so this path is **untested** by CI). Verify `mapWeightsQ` points the
     classifier at `q_tokens` with the right base/`size_each`, and that `dequantRow`
     (embedding) and the classifier `matmulQ` agree with runq's whole-table
     `dequantize` + classifier `matmul`.
   - **MHA vs GQA** — the tiny fixture is GQA (`kv_mul=2`); stories15M is MHA
     (`kv_mul=1`). The MHA path is exercised by the f32 parity (which passes), but
     re-confirm the quantized forward's head indexing matches under `kv_mul=1`.

The cheapest high-value experiment: **dump pos-0 logits from both engines and diff.**
That immediately says whether it's a single-forward bug (look within one forward) or a
cross-position/kv-cache accumulation (look at the cache path).

## 6. Definition of done

`bash examples/llama2/validate.sh` with a real `stories15M_q80.bin` present prints
`PASS: llama2.zero matches runq.c token-for-token` for the **int8 temp 0, temp>0, and
top-p** matrices (the f32 matrix already passes). Then ideally tighten the CI fixture so
it would have caught this — e.g. a longer/shared-classifier tiny model, or an explicit
temp>0 (sampled) assertion in `generate-argmax-q.0`'s sibling.

## 7. Pointers

- `examples/llama2/src/ops.0` — `matmulQ`, `quantizeActs`, `dequantRow`, `roundHA`.
- `examples/llama2/src/transformer.0` — `forwardQ`, `layerQWeight`, `mallocRunStateQ`.
- `examples/llama2/src/checkpoint.0` — `mapWeightsQ` (weight base offsets, shared-cls).
- `examples/llama2/validate.sh` — `parity_one_q` / `parity_topp_q` (the failing matrix).
- `conformance/native/pass/generate-argmax-q.0` — the tiny CI parity gate (argmax-only).
- Reference: `runq.c` from `karpathy/llama2.c` master (`quantize` / `matmul` /
  `dequantize` / `forward`). `validate.sh` fetches it next to `run.c`.
