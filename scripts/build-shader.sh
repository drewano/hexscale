#!/usr/bin/env bash
# Regenerate layer/src/cas_spv.h from layer/src/cas.comp.
# Requires glslangValidator (glslang-tools package / homebrew glslang).
set -euo pipefail

cd "$(dirname "$0")/.."
command -v glslangValidator >/dev/null || {
    echo "glslangValidator not found (brew install glslang / apt install glslang-tools)" >&2
    exit 1
}

tmp="$(mktemp)"
glslangValidator -V --target-env vulkan1.1 -x layer/src/cas.comp -o "$tmp"

python3 - "$tmp" << 'EOF'
import sys
body = open(sys.argv[1]).read().strip()
words = [w.strip() for w in body.replace('\n', ' ').split(',') if w.strip()]
lines = [', '.join(words[i:i + 8]) for i in range(0, len(words), 8)]
out = '/* Generated from cas.comp by glslangValidator (scripts/build-shader.sh). Do not edit. */\n'
out += 'static const uint32_t k_cas_spv[] = {\n    ' + ',\n    '.join(lines) + '\n};\n'
open('layer/src/cas_spv.h', 'w').write(out)
print(f"layer/src/cas_spv.h: {len(words)} words")
EOF
rm -f "$tmp"
