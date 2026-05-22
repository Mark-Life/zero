#!/usr/bin/env bash
# Phase 8 validation — numerical parity of llama2.zero vs karpathy/llama2.c.
#
# Builds the Zero example, fetches + builds the C reference, runs both on the
# real stories15M checkpoint, and diffs the generated token streams. At
# temperature 0 (greedy argmax) the streams must be byte-identical; with
# temperature > 0 they must also match once the seed (12345) and sampler (plain
# multinomial — top-p disabled via `-p 0`) are aligned, because the xorshift*
# PRNG is reproduced bit-for-bit (Phase 6).
#
# The one expected, documented difference: llama2.zero does not expand raw-byte
# `<0xNN>` fallback tokens in v0.1 (it prints the literal `<0xNN>`), whereas
# llama2.c emits the byte. The token *ids* are identical; this script normalizes
# `<0xNN>` back to the raw byte before diffing, so it compares token streams, not
# the cosmetic rendering of that one decode path.
#
# Both binaries are built and run under an amd64 Alpine container so they link
# the *same* musl libm (the Zero exe statically links musl libm via zig; the
# reference is compiled against Alpine's musl) — eliminating libm as a variable.
# Requires Docker and node (already a repo dependency). Override the data/cache
# dir with LLAMA2_DATA.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
data="${LLAMA2_DATA:-$root/.zero/llama2-data}"
zero="$root/bin/zero"
img="alpine"
plat="linux/amd64"

model_url="https://huggingface.co/karpathy/tinyllamas/resolve/main/stories15M.bin"
tok_url="https://github.com/karpathy/llama2.c/raw/master/tokenizer.bin"
runc_url="https://github.com/karpathy/llama2.c/raw/master/run.c"

command -v docker >/dev/null 2>&1 || { echo "error: docker is required (runs the linux-musl-x64 binaries + builds the musl reference)." >&2; exit 1; }
command -v node >/dev/null 2>&1 || { echo "error: node is required (normalizes raw-byte tokens before diffing)." >&2; exit 1; }
[ -x "$zero" ] || { echo "error: $zero not built. Run: make -C native/zero-c" >&2; exit 1; }

mkdir -p "$data"
echo "==> fetching weights + reference into $data"
[ -f "$data/stories15M.bin" ] || curl -fSL -o "$data/stories15M.bin" "$model_url"
[ -f "$data/tokenizer.bin" ]  || curl -fSL -o "$data/tokenizer.bin"  "$tok_url"
[ -f "$data/run.c" ]          || curl -fSL -o "$data/run.c"          "$runc_url"

echo "==> building llama2.zero (--backend zero-elf64, linux-musl-x64)"
"$zero" build --backend zero-elf64 --emit exe --target linux-musl-x64 "$root/examples/llama2" --out "$data/llama2" >/dev/null
echo "==> building llama2.c reference (gcc + musl libm, inside $img)"
# run.c relies on transitive stdint.h (glibc); musl needs it forced.
docker run --rm --platform "$plat" -v "$data":/work -w /work "$img" sh -c \
  "apk add --no-cache gcc musl-dev >/dev/null 2>&1 && gcc -O3 -include stdint.h -o run_ref run.c -lm"

# Normalize llama2.zero's literal <0xNN> raw-byte tokens back to the actual byte,
# then byte-compare against the reference. node is used (guaranteed present) so we
# need no perl/python inside the busybox container.
norm_and_diff() {
  node -e '
    const fs = require("fs");
    const z = fs.readFileSync(process.argv[1], "latin1")
      .replace(/<0x([0-9A-Fa-f]{2})>/g, (_, h) => String.fromCharCode(parseInt(h, 16)));
    const r = fs.readFileSync(process.argv[2], "latin1");
    if (z === r) process.exit(0);
    process.stderr.write("--- llama2.zero (normalized)\n" + z + "\n--- llama2.c\n" + r + "\n");
    process.exit(1);
  ' "$1" "$2"
}

# parity_one <temperature> <tokens> <prompt>
parity_one() {
  t="$1"; n="$2"; p="$3"
  if [ "$t" = "0" ]; then ref_extra=""; else ref_extra="-p 0 -s 12345"; fi
  docker run --rm --platform "$plat" -v "$data":/work -w /work "$img" sh -c "
    ./llama2 stories15M.bin --prompt '$p' --tokens $n --temperature $t 2>/dev/null > z.txt
    ./run_ref stories15M.bin -z tokenizer.bin -t $t $ref_extra -n $n -i '$p' 2>/dev/null > r.txt
  "
  if norm_and_diff "$data/z.txt" "$data/r.txt"; then
    echo "  OK   [t=$t, $n tok] \"$p\""
  else
    echo "  FAIL [t=$t, $n tok] \"$p\""
    return 1
  fi
}

fail=0
echo "==> temperature 0 (deterministic argmax) parity"
parity_one 0 100 "Once upon a time" || fail=1
parity_one 0 120 "Lily and Tom went to the park" || fail=1
parity_one 0 80  "The little robot" || fail=1
parity_one 0 256 "One day" || fail=1
parity_one 0 60  "" || fail=1

echo "==> temperature > 0 (softmax + xorshift* PRNG + multinomial) parity"
parity_one 0.8 100 "Once upon a time" || fail=1
parity_one 1.0 100 "The dragon" || fail=1
parity_one 0.5 80  "" || fail=1

if [ "$fail" -eq 0 ]; then
  echo "==> PASS: llama2.zero matches llama2.c token-for-token on stories15M."
else
  echo "==> FAIL: divergence found (see diffs above)." >&2
  exit 1
fi
