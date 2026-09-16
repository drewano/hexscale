#!/usr/bin/env bash
# Prepare the NPU (Hexagon HTP) artifacts for hexscale on SM8550 (Odin 2):
#
#   1. Qualcomm AI Runtime (QAIRT) Community SDK — public download, no login
#      (softwarecenter.qualcomm.com, "Qualcomm AI Runtime SDK - Community
#      Edition"; check the catalog page for the current version).
#   2. XLSR x4 super-resolution model, w8a8 quantized QNN DLC — public
#      (HuggingFace qualcomm/XLSR -> AI Hub public S3 mirror).
#   3. HTP context binary specialized for SM8550 (compiled once, in an
#      x86_64 container as Qualcomm ships host tools only for x86_64).
#   4. qairt-sample-app-hl inference harness built natively for aarch64.
#
# Output: build/npu-deploy/hexscale-npu/ — copy to the device and run
# ./run-htp-test.sh. Requires: docker (arm + amd64 platforms), curl, unzip,
# python3 with onnx (optional, for custom input shapes).
set -euo pipefail
cd "$(dirname "$0")/.."

QAIRT_VERSION="${QAIRT_VERSION:-2.50.40.260831}"
QAIRT_URL="https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VERSION}/v${QAIRT_VERSION}.zip"
XLSR_DLC_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/xlsr/releases/v0.62.2/xlsr-qnn_dlc-w8a8.zip"
WORK=build/npu
OUT=build/npu-deploy/hexscale-npu

mkdir -p "$WORK" "$OUT/lib" "$OUT/hexagon-unsigned" "$OUT/models"

# --- 1. QAIRT SDK ---------------------------------------------------------
if [ ! -d "$WORK/sdk/qairt/$QAIRT_VERSION" ]; then
    echo ">> Downloading QAIRT Community $QAIRT_VERSION (~2.5 GB, public)..."
    curl -L --retry 3 -o "$WORK/qairt.zip" "$QAIRT_URL"
    unzip -q -o "$WORK/qairt.zip" \
        "qairt/$QAIRT_VERSION/lib/aarch64-oe-linux-gcc11.2/*" \
        "qairt/$QAIRT_VERSION/lib/hexagon-v73/*" \
        "qairt/$QAIRT_VERSION/lib/x86_64-linux-clang/*" \
        "qairt/$QAIRT_VERSION/bin/x86_64-linux-clang/*" \
        "qairt/$QAIRT_VERSION/include/*" \
        "qairt/$QAIRT_VERSION/examples/QAIRT/SampleApp/*" \
        -d "$WORK/sdk"
fi
SDK="$WORK/sdk/qairt/$QAIRT_VERSION"

# --- 2. XLSR model (public S3 mirror) -------------------------------------
if [ ! -f "$WORK/xlsr.dlc" ]; then
    echo ">> Downloading XLSR w8a8 DLC (public AI Hub S3)..."
    curl -L --retry 3 -o "$WORK/xlsr_dlc.zip" "$XLSR_DLC_URL"
    unzip -q -o "$WORK/xlsr_dlc.zip" -d "$WORK/dlc"
    find "$WORK/dlc" -name '*.dlc' -exec cp {} "$WORK/xlsr.dlc" \;
fi

# --- 3. SM8550 HTP context binary (host tools are x86_64 only) ------------
if [ ! -f "$WORK/xlsr_htp_v73_sm8550.SM8550.bin" ]; then
    echo ">> Generating SM8550 context binary in an amd64 container..."
    docker run --rm --platform linux/amd64 \
        -v "$PWD/$WORK":/work -v "$PWD/$WORK/sdk":/sdk debian:bookworm bash -c "
        apt-get update -qq >/dev/null && apt-get install -y -qq libatomic1 libc++1 >/dev/null
        Q=/sdk/qairt/$QAIRT_VERSION
        export LD_LIBRARY_PATH=\$Q/lib/x86_64-linux-clang
        \$Q/bin/x86_64-linux-clang/qnn-context-binary-generator \
          --dlc_path /work/xlsr.dlc \
          --backend \$Q/lib/x86_64-linux-clang/libQairtHtp.so \
          --htp_socs sm8550 \
          --output_dir /work \
          --binary_file xlsr_htp_v73_sm8550
    "
fi

# --- 4. aarch64 harness (official QAIRT SampleApp, high-level API) --------
echo ">> Building qairt-sample-app-hl for aarch64..."
docker run --rm --platform linux/arm64 \
    -v "$PWD/$WORK/sdk":/sdk -v "$PWD/$WORK":/work debian:bookworm bash -c "
    apt-get update -qq >/dev/null && apt-get install -y -qq g++ >/dev/null
    S=/sdk/qairt/$QAIRT_VERSION/examples/QAIRT/SampleApp
    Q=/sdk/qairt/$QAIRT_VERSION/include/QAIRT
    g++ -std=c++17 -fno-rtti -O2 -w -pthread -ldl \
      -I\$S/src -I\$S/src/HighLevel -I\$S/src/PAL/include -I\$Q -I\$Q/QairtCpp \
      \$S/src/QairtSampleAppUtils.cpp \$S/src/HighLevel/*.cpp \
      \$S/src/PAL/src/linux/*.cpp \$S/src/PAL/src/common/*.cpp \
      -o /work/qairt-sample-app-hl
"

# --- 5. Assemble the device package ---------------------------------------
echo ">> Assembling $OUT ..."
cp "$WORK/qairt-sample-app-hl" "$OUT/"
cp "$SDK"/lib/aarch64-oe-linux-gcc11.2/libQairtHtp.so "$OUT/lib/"
cp "$SDK"/lib/aarch64-oe-linux-gcc11.2/libQairtHtpV73Stub.so "$OUT/lib/"
cp "$SDK"/lib/aarch64-oe-linux-gcc11.2/libQairtSystem.so "$OUT/lib/" 2>/dev/null || true
cp "$SDK"/lib/hexagon-v73/unsigned/*.so "$OUT/hexagon-unsigned/"
cp "$WORK/xlsr.dlc" "$OUT/models/"
cp "$WORK/xlsr_htp_v73_sm8550.SM8550.bin" "$OUT/models/"

python3 - << 'EOF'
import os
d = 'build/npu-deploy/hexscale-npu'
data = bytearray()
for y in range(128):
    for x in range(128):
        data += bytes([x * 2, y * 2, (x ^ y) * 2])
open(os.path.join(d, 'input_128x128_rgb.raw'), 'wb').write(bytes(data))
open(os.path.join(d, 'input_list.txt'), 'w').write(':= input_128x128_rgb.raw image\n')
EOF

cat > "$OUT/run-htp-test.sh" << 'EOF'
#!/usr/bin/env bash
# NPU test: real XLSR inference on the Hexagon HTP (CDSP) of the SM8550.
set -e
cd "$(dirname "$0")"
export LD_LIBRARY_PATH="$PWD/lib"
export ADSP_LIBRARY_PATH="$PWD/hexagon-unsigned"
./qairt-sample-app-hl \
  --dlc models/xlsr.dlc \
  --input_list input_list.txt \
  --output_dir ./out \
  --backend htp \
  --perf_profile burst \
  --log_level info "$@"
EOF
chmod +x "$OUT/run-htp-test.sh"

echo
echo "Done. Deploy to the device:"
echo "  scp -r $OUT armada@<console>:/var/home/armada/.local/"
echo "  ssh armada@<console> '.local/hexscale-npu/run-htp-test.sh'"
