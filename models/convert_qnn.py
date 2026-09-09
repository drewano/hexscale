#!/usr/bin/env python3
"""
Hexscale: Qualcomm AI Engine Direct (QNN) Converter for XLSR-x1.5
Target Architecture: Qualcomm Hexagon Tensor Processor (HTP v73 - Snapdragon 8 Gen 2 / SM8550)
Precision: INT8 (W8A8)
"""

import os
import sys
import argparse
import subprocess

def check_qnn_sdk():
    qnn_sdk_root = os.environ.get("QNN_SDK_ROOT")
    if not qnn_sdk_root:
        print("[!] QNN_SDK_ROOT environment variable not set.")
        print("    Please source the Qualcomm AI Engine Direct SDK environment:")
        print("    source /opt/qcom/qnn-sdk/bin/envsetup.sh")
        return None
    return qnn_sdk_root

def convert_onnx_to_qnn(onnx_path, output_dir, target_soc="sm8550"):
    print(f"[*] Converting {onnx_path} for target {target_soc} (HTP v73)...")
    os.makedirs(output_dir, exist_ok=True)
    
    qnn_root = check_qnn_sdk()
    if not qnn_root:
        print("[!] Running in dry-run mode (QNN SDK not detected).")
        print("    Command pipeline to be executed on target workstation:")
        
        step1 = f"qnn-onnx-converter -i {onnx_path} -o {output_dir}/xlsr.cpp --input_layout 'input_rgb' NHWC"
        step2 = f"qnn-model-lib-generator -c {output_dir}/xlsr.cpp -b {output_dir}/xlsr.bin -t aarch64-oe-linux-gcc"
        step3 = f"qnn-context-binary-generator --model {output_dir}/libxlsr.so --backend libQnnHtp.so --output_dir {output_dir} --binary_file xlsr_htp_v73.bin --soc_model {target_soc}"
        
        print(f"    1. {step1}")
        print(f"    2. {step2}")
        print(f"    3. {step3}")
        return True

    # Real execution when QNN SDK is sourced
    converter_bin = os.path.join(qnn_root, "bin", "x86_64-linux-gnu", "qnn-onnx-converter")
    cmd = [
        converter_bin,
        "-i", onnx_path,
        "-o", os.path.join(output_dir, "xlsr.cpp"),
        "--input_layout", "input_rgb", "NHWC"
    ]
    
    print(f"[>] Executing: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)
    print("[+] Model successfully converted to QNN representation.")
    return True

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Hexscale QNN Model Converter")
    parser.add_argument("--onnx", default="models/xlsr/xlsr_x1.5.onnx", help="Path to input ONNX model")
    parser.add_argument("--output", default="build/models", help="Output directory for context binary")
    parser.add_argument("--soc", default="sm8550", help="Target Qualcomm SoC (sm8550 / sm8650 / sm8750)")
    
    args = parser.parse_args()
    convert_onnx_to_qnn(args.onnx, args.output, args.soc)
