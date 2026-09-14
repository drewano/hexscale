import os
import json
import socket
import struct
import asyncio
from typing import Dict, Any

SOCKET_PATH = "/run/hexscale/control.sock"
SETTINGS_PATH = os.path.join(
    os.environ.get("DECKY_PLUGIN_SETTINGS_DIR", "/var/home/armada/homebrew/settings/hexscale"),
    "settings.json"
)
PROTOCOL_MAGIC = 0x48455853  # "HEXS"
PROTOCOL_VERSION = 1

CMD_GET_STATUS = 0x0001
CMD_SET_ENABLED = 0x0002
CMD_SET_SHARPNESS = 0x0003
CMD_SET_PROFILE = 0x0004

DEFAULT_CONFIG = {
    "enabled": True,
    "sharpness": 0.75,
    "profile": 1
}

def load_config() -> Dict[str, Any]:
    try:
        if os.path.exists(SETTINGS_PATH):
            with open(SETTINGS_PATH, "r") as f:
                return {**DEFAULT_CONFIG, **json.load(f)}
    except Exception:
        pass
    return DEFAULT_CONFIG.copy()

def save_config(cfg: Dict[str, Any]):
    try:
        os.makedirs(os.path.dirname(SETTINGS_PATH), exist_ok=True)
        with open(SETTINGS_PATH, "w") as f:
            json.dump(cfg, f, indent=2)
    except Exception:
        pass

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

                raw_resp = client.recv(1024)
                if len(raw_resp) < 18:
                    return {"success": False, "error": "Invalid response size", "data": None}

                _, _, _, _, _, status_code = struct.unpack("<IIHHIH", raw_resp[:18])
                payload_data = raw_resp[18:]

                if len(payload_data) >= 88:
                    enabled, profile, _, sharpness, last_ms, avg_ms, total_frames = struct.unpack(
                        "<BBHfffQ", payload_data[:24]
                    )
                    model_name = payload_data[24:56].decode('utf-8', errors='ignore').split('\x00')[0]
                    soc = payload_data[56:72].decode('utf-8', errors='ignore').split('\x00')[0]
                    backend = payload_data[72:88].decode('utf-8', errors='ignore').split('\x00')[0]

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
        resp = await loop.run_in_executor(None, self._send_command, CMD_GET_STATUS)
        cfg = load_config()
        if resp.get("success") and resp.get("data"):
            return {
                "success": True,
                "online": True,
                "data": resp["data"]
            }
        else:
            return {
                "success": True,
                "online": False,
                "data": {
                    "enabled": cfg["enabled"],
                    "sharpness": cfg["sharpness"],
                    "profile": cfg["profile"],
                    "model_name": "HTP v73 Super-Resolution",
                    "soc": "Snapdragon 8 Gen 2 (SM8550)",
                    "backend": "Hexagon CDSP (FastRPC)",
                    "last_inference_ms": 0.0,
                    "avg_inference_ms": 0.0,
                    "total_frames": 0
                }
            }

    async def set_enabled(self, enabled: bool) -> Dict[str, Any]:
        cfg = load_config()
        cfg["enabled"] = bool(enabled)
        save_config(cfg)
        payload = struct.pack("<B", 1 if enabled else 0)
        loop = asyncio.get_event_loop()
        await loop.run_in_executor(None, self._send_command, CMD_SET_ENABLED, payload)
        return {"success": True}

    async def set_sharpness(self, sharpness: float) -> Dict[str, Any]:
        cfg = load_config()
        cfg["sharpness"] = float(sharpness)
        save_config(cfg)
        payload = struct.pack("<f", float(sharpness))
        loop = asyncio.get_event_loop()
        await loop.run_in_executor(None, self._send_command, CMD_SET_SHARPNESS, payload)
        return {"success": True}

    async def set_profile(self, profile: int) -> Dict[str, Any]:
        cfg = load_config()
        cfg["profile"] = int(profile)
        save_config(cfg)
        payload = struct.pack("<B", int(profile))
        loop = asyncio.get_event_loop()
        await loop.run_in_executor(None, self._send_command, CMD_SET_PROFILE, payload)
        return {"success": True}
