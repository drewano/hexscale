#!/usr/bin/env bash
# Regenerate layer/src/cas_spv.h from layer/src/cas.vert and cas.frag.
# Requires glslangValidator (glslang-tools package / homebrew glslang).
set -euo pipefail

cd "$(dirname "$0")/.."
command -v glslangValidator >/dev/null || {
    echo "glslangValidator not found (brew install glslang / apt install glslang-tools)" >&2
    exit 1
}

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

glslangValidator -V --target-env vulkan1.1 -o "$tmpdir/cas.vert.spv" layer/src/cas.vert
glslangValidator -V --target-env vulkan1.1 -o "$tmpdir/cas.frag.spv" layer/src/cas.frag
glslangValidator -V --target-env vulkan1.1 -DSWAP_RB -o "$tmpdir/cas_bgra.frag.spv" layer/src/cas.frag

python3 - "$tmpdir" << 'PYEOF'
import struct, sys

def to_header(spv_path, name):
    spv = open(spv_path, 'rb').read()
    words = struct.unpack('<%dI' % (len(spv) // 4), spv)
    lines = [', '.join('0x%08x' % w for w in words[i:i + 8])
             for i in range(0, len(words), 8)]
    return ('/* Generated from layer/src/cas.vert / cas.frag by scripts/build-shader.sh. '
            'Do not edit. */\nstatic const uint32_t %s[] = {\n    ' % name) \
        + ',\n    '.join(lines) + '\n};\n'

parts = [
    to_header(sys.argv[1] + '/cas.vert.spv', 'k_cas_vert_spv'),
    to_header(sys.argv[1] + '/cas.frag.spv', 'k_cas_frag_spv'),
    to_header(sys.argv[1] + '/cas_bgra.frag.spv', 'k_cas_frag_bgra_spv'),
]
open('layer/src/cas_spv.h', 'w').write('\n'.join(parts))
print('layer/src/cas_spv.h regenerated')
PYEOF
