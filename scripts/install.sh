#!/usr/bin/env bash
# Install hexscale from a release tarball (bin/, layer/, scripts/) into
# ~/.local — no custom OS image required. Safe: everything lives in the
# user's home and the effect is gated behind ENABLE_HEXSCALE=1.
set -euo pipefail

cd "$(dirname "$0")"

DEST_BIN="$HOME/.local/bin"
DEST_LIB="$HOME/.local/lib"
DEST_LAYER="$HOME/.local/share/vulkan/implicit_layer.d"

mkdir -p "$DEST_BIN" "$DEST_LIB" "$DEST_LAYER"

install -m 0755 bin/hexscaled "$DEST_BIN/hexscaled"
install -m 0755 bin/hexscale-cli "$DEST_BIN/hexscale-cli"
install -m 0755 layer/libVkLayer_hexscale.so "$DEST_LIB/libVkLayer_hexscale.so"
install -m 0644 layer/VkLayer_hexscale.json "$DEST_LAYER/VkLayer_hexscale.json"

# Daemon service (user-level, socket in XDG_RUNTIME_DIR; no root needed).
mkdir -p "$HOME/.config/systemd/user"
install -m 0644 scripts/hexscaled-user.service \
    "$HOME/.config/systemd/user/hexscaled.service"
systemctl --user daemon-reload
systemctl --user enable --now hexscaled.service 2>/dev/null || {
    echo "NOTE: user service failed to start (no systemd --user session?);"
    echo "      start hexscaled manually: $DEST_BIN/hexscaled"
}

# FastRPC access is optional (only used by the NPU path, not GPU CAS).
if [[ ! -r /dev/fastrpc-cdsp && ! -w /dev/fastrpc-cdsp ]]; then
    echo "NOTE: /dev/fastrpc-cdsp not accessible — NPU dma-buf mapping disabled."
    echo "      GPU sharpening works regardless."
fi

cat << 'EOF'

Installed. To enable the sharpening layer for a game or the whole session:

  # Per-game (Steam launch options):
  ENABLE_HEXSCALE=1 VK_LAYER_PATH=$HOME/.local/share/vulkan/implicit_layer.d %command%

  # Whole session (persisted, needs re-login):
  mkdir -p ~/.config/environment.d
  echo "ENABLE_HEXSCALE=1" > ~/.config/environment.d/60-hexscale.conf
  echo "VK_LAYER_PATH=$HOME/.local/share/vulkan/implicit_layer.d" >> ~/.config/environment.d/60-hexscale.conf

Kill switch: DISABLE_HEXSCALE=1. Sharpness: HEXSCALE_SHARPNESS=0..1 or from
the Decky plugin (QAM) while the daemon runs.

EOF
