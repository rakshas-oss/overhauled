#!/usr/bin/env python3
"""
Test client for the Tensor Schema Integration Server.
Demonstrates ONNX model loading and inference over the ADI protocol.
"""

import socket
import struct
import numpy as np
import torch
import torch.nn as nn
import sys
import os

MAGIC_ADI1 = 0x41444931
OP_LOAD_MODEL = 0x01
OP_EXECUTE_INFERENCE = 0x02

class LinearModel(nn.Module):
    """Simple linear model: y = 2*x (5-dimensional)"""
    def __init__(self):
        super().__init__()
        self.fc = nn.Linear(5, 5, bias=False)
        with torch.no_grad():
            self.fc.weight.copy_(torch.eye(5) * 2.0)

    def forward(self, x):
        return self.fc(x)

def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    onnx_file = "/tmp/sample_model.onnx"

    # Generate ONNX model
    print("[*] Generating sample ONNX model...")
    torch.onnx.export(
        LinearModel(),
        torch.randn(1, 5),
        onnx_file,
        input_names=["input"],
        output_names=["output"],
        dynamic_axes={"input": {0: "batch"}, "output": {0: "batch"}}
    )
    print(f"[*] Generated ONNX model at {onnx_file}")

    # Connect to server
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    print(f"[*] Connected to {host}:{port}")

    # Send LOAD_MODEL opcode
    model_id = 101
    load_payload = struct.pack("!I", model_id) + onnx_file.encode("utf-8")
    load_header = struct.pack("!IIBB3s", MAGIC_ADI1, len(load_payload), OP_LOAD_MODEL, 0, b"\x00\x00\x00")

    sock.sendall(load_header + load_payload)
    resp_hdr = sock.recv(20)
    ts, seq, ptype, status, _, length = struct.unpack("!QIBBHI", resp_hdr)
    print(f"[*] Model Loaded. Status: {status}")

    # Send 3 inference frames
    for frame in range(3):
        print(f"\n[Frame {frame}]")
        input_tensor = np.array([[1.0 + frame, 2.0 + frame, 3.0, 4.0, 5.0]], dtype=np.float32)
        raw_data = input_tensor.tobytes()
        
        inf_subhdr = struct.pack("!III", model_id, 1, 1)
        payload = inf_subhdr + raw_data
        req_header = struct.pack("!IIBB3s", MAGIC_ADI1, len(payload), OP_EXECUTE_INFERENCE, 0, b"\x00\x00\x00")
        
        sock.sendall(req_header + payload)
        print(f"  Sent input: {input_tensor[0]}")
        
        # Receive primary packet
        resp_hdr_bytes = sock.recv(20)
        ts, seq, ptype, status, _, length = struct.unpack("!QIBBHI", resp_hdr_bytes)
        out_bytes = sock.recv(length)
        output_arr = np.frombuffer(out_bytes, dtype=np.float32)
        print(f"  Primary  (Seq {seq}): {output_arr}")
        
        # Receive delta packet on subsequent frames
        if frame > 0:
            delta_hdr = sock.recv(20)
            ts_d, seq_d, ptype_d, status_d, _, len_d = struct.unpack("!QIBBHI", delta_hdr)
            delta_bytes = sock.recv(len_d)
            delta_arr = np.frombuffer(delta_bytes, dtype=np.float32)
            print(f"  Delta    (Seq {seq_d}): {delta_arr}")

    sock.close()
    print("\n[*] Test completed successfully.")

if __name__ == "__main__":
    main()
