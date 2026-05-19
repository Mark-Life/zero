#!/usr/bin/env bash
# Install a target-capable C toolchain on macOS so libm-linked conformance
# fixtures (math-sqrtf, math-expf, math-cosf-sinf, math-powf, math-absf-floorf,
# math-rmsnorm-smoke, math-softmax-smoke) can validate numerics locally.
#
# Without one, `zero build --target linux-musl-x64` fails to link `-lm` and the
# fixtures skip via BLD003 through `assertDirectRuntimeOrUnsupported`'s
# tolerant path. Numerics only validate in Linux CI / the Vercel sandbox.
#
# On Linux x86-64 this script only verifies — direct builds for
# linux-musl-x64 already work via the host toolchain.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

verify() {
  if [[ ! -x ./bin/zero ]]; then
    echo "skip verify: ./bin/zero not built. Run \`make -C native/zero-c\` then re-run this script." >&2
    return 0
  fi
  local fixture="conformance/native/pass/math-sqrtf-known-values.0"
  local out_dir=".zero/setup-cross-toolchain"
  mkdir -p "$out_dir"
  local log="$out_dir/build.json"
  echo "verifying via $fixture..."
  if ./bin/zero build --json --emit exe --target linux-musl-x64 "$fixture" --out "$out_dir/math-sqrtf-known-values" >"$log" 2>&1; then
    echo "ok: libm fixture built without BLD003 — cross-toolchain ready."
    return 0
  fi
  if grep -q '"code": *"BLD003"' "$log"; then
    echo "fail: libm fixture still hits BLD003 — cross-toolchain not picked up." >&2
    echo "      ensure zig is on PATH, or set ZERO_CC to a linux-musl-x64-capable compiler." >&2
    cat "$log" >&2
    return 1
  fi
  echo "fail: build error other than BLD003 — see $log." >&2
  cat "$log" >&2
  return 1
}

uname_s="$(uname -s)"
case "$uname_s" in
  Darwin)
    if command -v zig >/dev/null 2>&1; then
      echo "zig already on PATH: $(zig version)"
    elif [[ -n "${ZERO_CC:-}" ]]; then
      echo "ZERO_CC is set to '$ZERO_CC' — using it instead of zig."
    else
      if ! command -v brew >/dev/null 2>&1; then
        echo "Homebrew is required to install the bundled cross-toolchain on macOS." >&2
        echo "  - install Homebrew: https://brew.sh" >&2
        echo "  - alternative: install zig manually (https://ziglang.org) or set" >&2
        echo "    ZERO_CC=x86_64-linux-musl-gcc after \`brew install FiloSottile/musl-cross/musl-cross\`." >&2
        exit 1
      fi
      echo "installing zig via Homebrew (bundled cross-toolchain)..."
      brew install zig
    fi
    ;;
  Linux)
    arch="$(uname -m)"
    if [[ "$arch" == "x86_64" ]]; then
      echo "Linux x86-64 host: direct linux-musl-x64 builds work without a cross-toolchain. Verifying."
    else
      echo "Linux $arch host: install a linux-musl-x64-capable C compiler before running libm fixtures."
      echo "  - try \`pkg install zig\` / \`apt install zig\`, or"
      echo "  - set ZERO_CC=x86_64-linux-musl-gcc after installing musl-cross."
    fi
    ;;
  *)
    echo "Unsupported host: $uname_s." >&2
    echo "Install a target-capable C toolchain (zig recommended) and re-run." >&2
    exit 1
    ;;
esac

verify
