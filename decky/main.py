import os
import socket
import struct
import asyncio
from typing import Dict, Any

SOCKET_PATH = "/run/hexscale/control.sock"
PROTOCOL_MAGIC = 0x48455853  # "HEXS"
PROTOCOL_VERSION = 1

CMD_GET_STATUS = 0x0001
CMD_SET_ENABLED = 0x0002
CMD_SET_SHARPNESS = 0x0003
CMD_SET_PROFILE = 0x0004

class Plugin:
    async def _main(self):
        pass

    async def _unload(self):
        pass

    def _send_command(self, msg_type: int, payload_bytes: bytes = b"") -> Dict[str, Any]:
        if not os.path.exists(SOCKET_PATH):
            return {
                "success": False,
                "error": "Daemon offline (socket not found at /run/hexscale/control.sock)",
                "data": None
            }

        # Header: magic (I), version (I), msg_type (H), payload_size (H), sequence (I)
        # Total header size: 16 bytes
        header = struct.pack("<IIHHI", PROTOCOL_MAGIC, PROTOCOL_VERSION, msg_type, len(payload_bytes), 0)
        packet = header + payload_bytes

        # CommandPacket has fixed union size (pad to match C++ struct size of 24 bytes)
        if len(packet) < 24:
            packet += b"\x00" * (24 - len(packet))

        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(0.5)
                client.connect(SOCKET_PATH)
                client.sendall(packet)

                # ResponsePacket: Header (16) + status (H) + StatusPayload (82 bytes)
                raw_resp = client.recv(1024)
                if len(raw_resp) < 18:
                    return {"success": False, "error": "Invalid response size", "data": None}

                _, _, _, _, _, status_code = struct.unpack("<IIHHIH", raw_resp[:18])
                payload_data = raw_resp[18:]

                if len(payload_data) >= 80:
                    # Parse StatusPayload
                    # uint8_t enabled, uint8_t profile, uint16_t reserved, float sharpness,
                    # float last_ms, float avg_ms, uint64_t total_frames, 32s model, 16s soc, 16s backend
                    enabled, profile, _, sharpness, last_ms, avg_ms, total_frames = struct.unpack(
                        "<BBHfffQ", payload_data[:28]
                    )
                    model_name = payload_data[28:60].decode('utf-8', errors='ignore').split('\x00')[0]
                    soc = payload_data[60:76].decode('utf-8', errors='ignore').split('\x00')[0]
                    backend = payload_data[76:92].decode('utf-8', errors='ignore').split('\x00')[0]

                    return {
                        "success": True,
                        "data": {
                            "enabled": bool(enabled),
                            "profile": profile,
                            "sharpness": round(sharpness, 2),
                            "last_inference_ms": round(last_ms, 2),
                            "avg_inference_ms": round(avg_ms, 2),
                            "total_frames": total_frames,
                            "model_name": model_name,
                            "soc": soc,
                            "backend": backend
                        }
                    }

                return {"success": True, "status_code": status_code, "data": None}

        except Exception as e:
            return {"success": False, "error": str(e), "data": None}

    async def get_status(self) -> Dict[str, Any]:
        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(None, self._send_command, CMD_GET_STATUS)

    async def set_enabled(self, enabled: bool) -> Dict[str, Any]:
        payload = struct.pack("<B", 1 if enabled else 0)
        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(None, self._send_command, CMD_SET_ENABLED, payload)

    async def set_sharpness(self, sharpness: float) -> Dict[str, Any]:
        payload = struct.pack("<f", float(sharpness))
        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(None, self._send_command, CMD_SET_SHARPNESS, payload)

    async def set_profile(self, profile: int) -> Dict[str, Any]:
        payload = struct.pack("<B", int(profile))
        loop = asyncio.get_event_loop()
        return await loop.run_in_executor(None, self._send_command, CMD_SET_PROFILE, payload)
