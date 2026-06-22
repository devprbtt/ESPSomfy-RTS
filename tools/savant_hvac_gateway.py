#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Simple Savant-facing HVAC gateway scaffold.

Designed to run manually on the Savant Linux box as the unprivileged RPM user.
Defaults to a non-privileged TCP port so it can bind without root.

This script intentionally supports Python 2.7 and Python 3.x.
"""

from __future__ import print_function

import base64
import json
import logging
import os
import socket
import sys
import threading
import time

try:
    import SocketServer as socketserver
except ImportError:
    import socketserver

try:
    from Crypto.Cipher import AES
except ImportError:
    AES = None


DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 2323
DEFAULT_CONFIG = "savant_hvac_gateway.json"
GATEWAY_VERSION = "0.1.1"
GENERIC_GREE_DEVICE_KEY = b"a3K8Bx%2r8Y7#xDh"
GENERIC_GREE_DEVICE_KEY_GCM = b"{yxAHAY_Lm6pbC/<"
GCM_IV = b"\x54\x40\x78\x44\x49\x67\x5a\x51\x6c\x5e\x63\x13"
GCM_ADD = b"qualcomm-test"
GREE_DEFAULT_PORT = 7000
GREE_STATE_OPTIONS = [
    "Pow", "Mod", "SetTem", "WdSpd", "Air", "Blo", "Health",
    "SwhSlp", "Lig", "SwingLfRig", "SwUpDn", "Quiet", "Tur",
    "StHt", "TemUn", "HeatCoolType", "TemRec", "SvSt", "SlpMod", "TemSen"
]
GREE_MODE_TO_CODE = {
    "AUTO": 0,
    "COOL": 1,
    "DRY": 2,
    "FAN": 3,
    "HEAT": 4,
}
GREE_CODE_TO_MODE = dict((value, key) for key, value in GREE_MODE_TO_CODE.items())
GREE_FAN_TO_CODE = {
    "FANAUTO": 0,
    "FANLOW": 1,
    "FANMID": 3,
    "FANHIGH": 5,
}
GREE_CODE_TO_FAN = {
    0: "FANAUTO",
    1: "FANLOW",
    2: "FANLOW",
    3: "FANMID",
    4: "FANMID",
    5: "FANHIGH",
}
GREE_PRESERVED_COMMAND_KEYS = [
    "Pow", "Mod", "SetTem", "WdSpd", "Air", "Blo", "Health",
    "SwhSlp", "Lig", "SwingLfRig", "SwUpDn", "Quiet", "Tur",
    "StHt", "TemRec"
]
_LOGGER = logging.getLogger(__name__)

if not _LOGGER.handlers:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")


def now_ms():
    return int(time.time() * 1000)


def ensure_bytes(value):
    if isinstance(value, bytes):
        return value
    try:
        return value.encode("utf-8")
    except Exception:
        return bytes(value)


def gree_pad(payload):
    block = 16
    pad_len = block - (len(payload) % block)
    return payload + chr(pad_len) * pad_len


def gree_unpad(payload):
    if not payload:
        return payload
    try:
        pad_len = ord(payload[-1])
    except TypeError:
        pad_len = payload[-1]
    if pad_len < 1 or pad_len > 16:
        return payload.replace("\x0f", "")
    return payload[:-pad_len]


def gree_decode_mac(mac_value):
    normalized = (mac_value or "").replace(":", "").replace("-", "").lower()
    if "@" in normalized:
        sub_mac, main_mac = normalized.split("@", 1)
        return sub_mac, main_mac
    return normalized, normalized


def gree_gcm_cipher(key):
    cipher = AES.new(key, AES.MODE_GCM, nonce=GCM_IV)
    cipher.update(GCM_ADD)
    return cipher


def gree_encrypt_gcm(key, plaintext):
    cipher = gree_gcm_cipher(key)
    encrypted_data, tag = cipher.encrypt_and_digest(ensure_bytes(plaintext))
    return (
        base64.b64encode(encrypted_data).decode("utf-8"),
        base64.b64encode(tag).decode("utf-8"),
    )


def gree_decode_temperature(set_temp, temp_rec):
    if set_temp is None:
        return None
    value = float(set_temp)
    if temp_rec == 1:
        value += 0.5
    return value


def gree_decode_current_temperature(temp_sen):
    if temp_sen is None:
        return None
    try:
        value = float(temp_sen)
    except (TypeError, ValueError):
        return None
    if value >= 40:
        value -= 40
    return value


def gree_state_dict(status):
    cols = status.get("cols", [])
    dat = status.get("dat", [])
    if len(dat) == 1 and isinstance(dat[0], list):
        dat = dat[0]
    values = {}
    for idx, col in enumerate(cols):
        if idx < len(dat):
            values[col] = dat[idx]
    return values


def gree_normalize_key(value):
    if not value:
        return None
    if isinstance(value, bytes):
        return value
    if isinstance(value, str):
        try:
            decoded = base64.b64decode(value)
            if len(decoded) in (16, 24, 32):
                return decoded
        except Exception:
            pass
        return value.encode("utf-8")
    return ensure_bytes(value)


class HvacState(object):
    def __init__(self, thermostat_id, name, mode="OFF", power=False, setpoint=24, current_temp=25, fan="FANAUTO"):
        self.thermostat_id = int(thermostat_id)
        self.name = name
        self.mode = mode
        self.power = bool(power)
        self.setpoint = int(setpoint)
        self.current_temp = int(current_temp)
        self.fan = fan
        self.last_update_ms = now_ms()

    def status_lines(self):
        mode = self.mode if self.power else "OFF"
        return [
            "R:HVAC %s %d" % (mode, self.thermostat_id),
            "R:HVAC %s %d" % (self.fan, self.thermostat_id),
            "R:HVAC SETPOINT %d %d" % (self.thermostat_id, self.setpoint),
            "R:HVAC CURRENTTEMP %d %d" % (self.thermostat_id, self.current_temp),
        ]


class BackendBase(object):
    backend_name = "base"

    def __init__(self, devices):
        self._states = {}
        for device in devices:
            thermostat_id = int(device["thermostat_id"])
            self._states[thermostat_id] = HvacState(
                thermostat_id=thermostat_id,
                name=device.get("name", "HVAC %d" % thermostat_id),
                mode=device.get("mode", "OFF"),
                power=device.get("power", False),
                setpoint=device.get("setpoint", 24),
                current_temp=device.get("current_temp", 25),
                fan=device.get("fan", "FANAUTO"),
            )

    def list_states(self):
        return [self._states[k] for k in sorted(self._states)]

    def get_state(self, thermostat_id):
        state = self._states.get(int(thermostat_id))
        if state is None:
            raise ValueError("Unknown thermostat id %s" % thermostat_id)
        return state

    def refresh(self, thermostat_id=None):
        if thermostat_id is None:
            return self.list_states()
        return [self.get_state(thermostat_id)]

    def apply_command(self, thermostat_id, action, value=None):
        raise NotImplementedError


class MockBackend(BackendBase):
    backend_name = "mock"

    def apply_command(self, thermostat_id, action, value=None):
        state = self.get_state(thermostat_id)
        action = action.upper()
        if action == "OFF":
            state.power = False
            state.mode = "OFF"
        elif action in ("COOL", "HEAT", "FAN"):
            state.power = True
            state.mode = action
        elif action in ("FANAUTO", "FANLOW", "FANMID", "FANHIGH"):
            state.fan = action
        elif action == "SETPOINT":
            if value is None:
                raise ValueError("SETPOINT requires a value")
            state.setpoint = max(16, min(30, int(float(value))))
        else:
            raise ValueError("Unsupported action %s" % action)
        state.last_update_ms = now_ms()
        return state


class LGThinQBackend(BackendBase):
    backend_name = "lg_thinq"

    def __init__(self, devices, cloud):
        BackendBase.__init__(self, devices)
        self.cloud = cloud or {}

    def apply_command(self, thermostat_id, action, value=None):
        raise NotImplementedError(
            "LG ThinQ cloud control is not implemented yet. "
            "This scaffold is ready for a PAT-based backend."
        )


class GreeLocalBackend(BackendBase):
    backend_name = "gree_local"

    def __init__(self, devices, local_config):
        BackendBase.__init__(self, devices)
        self.local_config = local_config or {}
        self._device_configs = {}
        self._device_raw_state = {}
        self._index_devices(devices)
        if AES is None:
            raise ValueError(
                "gree_local backend requires PyCrypto/PyCryptodome (Crypto.Cipher.AES) on the Savant host"
            )

    def _index_devices(self, devices):
        local_devices = self.local_config.get("devices", [])
        local_by_id = {}
        for device in local_devices:
            local_by_id[int(device["thermostat_id"])] = device

        for device in devices:
            thermostat_id = int(device["thermostat_id"])
            merged = dict(device)
            if thermostat_id in local_by_id:
                merged.update(local_by_id[thermostat_id])
            merged["thermostat_id"] = thermostat_id
            merged["host"] = merged.get("host")
            merged["port"] = int(merged.get("port", GREE_DEFAULT_PORT))
            merged["encryption_version"] = merged.get("encryption_version")
            merged["key"] = gree_normalize_key(merged.get("key"))
            merged["sub_mac"], merged["main_mac"] = gree_decode_mac(merged.get("mac", ""))
            if not merged["host"]:
                raise ValueError("gree_local device %s is missing host" % thermostat_id)
            if not merged["main_mac"]:
                raise ValueError("gree_local device %s is missing mac" % thermostat_id)
            self._device_configs[thermostat_id] = merged

    def _send_packet(self, host, port, payload):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(float(self.local_config.get("timeout", 2.5)))
        try:
            sock.sendto(ensure_bytes(payload), (host, int(port)))
            response, _ = sock.recvfrom(65535)
            return json.loads(response.decode("utf-8", "ignore"))
        finally:
            sock.close()

    def _decrypt_response(self, response, key, encryption_version):
        pack = response.get("pack")
        if not pack:
            return response
        decoded_pack = base64.b64decode(pack)
        if int(encryption_version) == 2:
            cipher = gree_gcm_cipher(key)
            decrypted = cipher.decrypt(decoded_pack)
            tag = response.get("tag")
            if tag:
                cipher.verify(base64.b64decode(tag))
        else:
            cipher = AES.new(key, AES.MODE_ECB)
            decrypted = cipher.decrypt(decoded_pack)
        text = decrypted.decode("utf-8", "ignore")
        text = gree_unpad(text)
        last_brace = text.rfind("}")
        if last_brace >= 0:
            text = text[:last_brace + 1]
        return json.loads(text)

    def _packet_payload(self, main_mac, plaintext, key, encryption_version, uid, bind=False):
        if int(encryption_version) == 2:
            pack, tag = gree_encrypt_gcm(key, plaintext)
            payload = {
                "cid": "app",
                "i": 1 if bind else 0,
                "pack": pack,
                "t": "pack",
                "tcid": main_mac,
                "uid": uid,
                "tag": tag,
            }
            return json.dumps(payload, separators=(",", ":"))
        cipher = AES.new(key, AES.MODE_ECB)
        pack = base64.b64encode(cipher.encrypt(ensure_bytes(gree_pad(plaintext)))).decode("utf-8")
        payload = {
            "cid": "app",
            "i": 1 if bind else 0,
            "pack": pack,
            "t": "pack",
            "tcid": main_mac,
            "uid": uid,
        }
        return json.dumps(payload, separators=(",", ":"))

    def _request(self, device, plaintext, key, encryption_version, uid, bind=False):
        payload = self._packet_payload(device["main_mac"], plaintext, key, encryption_version, uid, bind=bind)
        response = self._send_packet(device["host"], device["port"], payload)
        return self._decrypt_response(response, key, encryption_version)

    def _detect_device_key(self, device):
        bind_v1 = json.dumps({"mac": device["main_mac"], "t": "bind", "uid": 0}, separators=(",", ":"))
        try:
            result = self._request(
                device,
                bind_v1,
                ensure_bytes(GENERIC_GREE_DEVICE_KEY),
                1,
                0,
                bind=True,
            )
            key = gree_normalize_key(result.get("key"))
            if key:
                device["encryption_version"] = 1
                device["key"] = key
                return
        except Exception as exc:
            _LOGGER.info("Gree bind v1 failed for thermostat %s: %s", device["thermostat_id"], exc)

        bind_v2 = json.dumps(
            {"cid": device["main_mac"], "mac": device["main_mac"], "t": "bind", "uid": 0},
            separators=(",", ":"),
        )
        result = self._request(
            device,
            bind_v2,
            GENERIC_GREE_DEVICE_KEY_GCM,
            2,
            0,
            bind=True,
        )
        key = gree_normalize_key(result.get("key"))
        if not key:
            raise ValueError("Unable to retrieve Gree key for thermostat %s" % device["thermostat_id"])
        device["encryption_version"] = 2
        device["key"] = key

    def _ensure_device_auth(self, device):
        if device.get("key") and device.get("encryption_version") in (1, 2, "1", "2"):
            device["encryption_version"] = int(device["encryption_version"])
            return
        self._detect_device_key(device)

    def _status_plaintext(self, device, cols=None):
        cols = cols or GREE_STATE_OPTIONS
        return json.dumps({"cols": cols, "mac": device["sub_mac"], "t": "status"}, separators=(",", ":"))

    def _refresh_device(self, thermostat_id):
        state = self.get_state(thermostat_id)
        device = self._device_configs[int(thermostat_id)]
        self._ensure_device_auth(device)
        status = self._request(
            device,
            self._status_plaintext(device),
            device["key"],
            int(device["encryption_version"]),
            0,
        )
        raw_values = gree_state_dict(status)
        self._device_raw_state[int(thermostat_id)] = raw_values

        power = raw_values.get("Pow") == 1
        mode_code = raw_values.get("Mod")
        mode = GREE_CODE_TO_MODE.get(mode_code, "OFF")
        fan = GREE_CODE_TO_FAN.get(raw_values.get("WdSpd"), "FANAUTO")
        setpoint = gree_decode_temperature(raw_values.get("SetTem"), raw_values.get("TemRec"))
        current_temp = gree_decode_current_temperature(raw_values.get("TemSen"))

        state.power = power
        state.mode = mode if power else "OFF"
        state.fan = fan
        if setpoint is not None:
            state.setpoint = int(round(setpoint))
        if current_temp is not None:
            state.current_temp = int(round(current_temp))
        state.last_update_ms = now_ms()
        return state

    def _build_command_values(self, current_values, action, value):
        updates = {}
        if action == "OFF":
            updates["Pow"] = 0
        elif action in ("COOL", "HEAT", "FAN"):
            updates["Pow"] = 1
            updates["Mod"] = GREE_MODE_TO_CODE[action]
        elif action in ("FANAUTO", "FANLOW", "FANMID", "FANHIGH"):
            updates["Tur"] = 0
            updates["Quiet"] = 0
            updates["WdSpd"] = GREE_FAN_TO_CODE[action]
        elif action == "SETPOINT":
            if value is None:
                raise ValueError("SETPOINT requires a value")
            target = float(value)
            target = max(16.0, min(30.0, target))
            half = int(round(target * 2))
            updates["SetTem"] = half >> 1
            updates["TemRec"] = half & 1
        else:
            raise ValueError("Unsupported action %s" % action)

        merged = {}
        for key in GREE_PRESERVED_COMMAND_KEYS:
            if key in updates:
                merged[key] = updates[key]
            elif key in current_values and current_values[key] is not None:
                merged[key] = int(current_values[key])

        merged["Buzzer_ON_OFF"] = 0
        merged["BuzzerCtrl"] = 1
        return merged

    def refresh(self, thermostat_id=None):
        if thermostat_id is None:
            states = []
            for state in self.list_states():
                states.append(self._refresh_device(state.thermostat_id))
            return states
        return [self._refresh_device(thermostat_id)]

    def apply_command(self, thermostat_id, action, value=None):
        thermostat_id = int(thermostat_id)
        device = self._device_configs[thermostat_id]
        self._ensure_device_auth(device)
        self._refresh_device(thermostat_id)
        current_values = self._device_raw_state.get(thermostat_id, {})
        command_values = self._build_command_values(current_values, action.upper(), value)
        opts = []
        params = []
        for key in command_values:
            opts.append(key)
            params.append(command_values[key])
        plaintext = json.dumps(
            {"opt": opts, "p": params, "t": "cmd", "sub": device["sub_mac"]},
            separators=(",", ":"),
        )
        self._request(
            device,
            plaintext,
            device["key"],
            int(device["encryption_version"]),
            0,
        )
        return self._refresh_device(thermostat_id)


class GatewayState(object):
    def __init__(self, config):
        self.config = config
        backend_name = config.get("backend", "mock")
        devices = config.get("devices", [])
        if backend_name == "mock":
            self.backend = MockBackend(devices)
        elif backend_name == "gree_local":
            self.backend = GreeLocalBackend(devices, config.get("gree_local"))
        elif backend_name == "lg_thinq":
            self.backend = LGThinQBackend(devices, config.get("lg_thinq"))
        else:
            raise ValueError("Unsupported backend %s" % backend_name)
        self.lock = threading.Lock()
        self.started_at = time.time()

    def hello_lines(self):
        return [
            "R:READY 1",
            "R:GATEWAY VERSION %s" % GATEWAY_VERSION,
            "R:GATEWAY BACKEND %s" % self.backend.backend_name,
        ]


class HvacRequestHandler(socketserver.StreamRequestHandler):
    def handle(self):
        gateway = self.server.gateway_state
        self.wfile.write(("R:WELCOME Savant HVAC Gateway %s\r\n" % GATEWAY_VERSION).encode("utf-8"))
        for line in gateway.hello_lines():
            self._send(line)
        while True:
            raw = self.rfile.readline()
            if not raw:
                break
            if not isinstance(raw, str):
                raw = raw.decode("utf-8", "replace")
            line = raw.strip()
            if not line:
                continue
            try:
                responses = self._dispatch(line)
                for response in responses:
                    self._send(response)
            except Exception as exc:
                self._send("ERR %s" % exc)

    def _send(self, line):
        self.wfile.write((line + "\r\n").encode("utf-8"))

    def _dispatch(self, line):
        tokens = line.split()
        cmd = tokens[0].upper()
        gateway = self.server.gateway_state
        with gateway.lock:
            if cmd in ("HELP", "?"):
                return [
                    "OK",
                    "R:CMDS HELP STA LIST REFRESH HVAC <MODE> <ID> [VALUE]",
                ]
            if cmd in ("STA", "LIST"):
                responses = ["OK"]
                for state in gateway.backend.list_states():
                    responses.extend(state.status_lines())
                return responses
            if cmd == "REFRESH":
                responses = ["OK"]
                for state in gateway.backend.refresh():
                    responses.extend(state.status_lines())
                return responses
            if cmd == "HVAC":
                if len(tokens) < 3:
                    raise ValueError("Usage: HVAC <ACTION> <ID> [VALUE]")
                action = tokens[1].upper()
                thermostat_id = int(tokens[2])
                value = tokens[3] if len(tokens) > 3 else None
                if action == "STATUS":
                    state = gateway.backend.get_state(thermostat_id)
                elif action == "REFRESH":
                    state = gateway.backend.refresh(thermostat_id)[0]
                else:
                    state = gateway.backend.apply_command(thermostat_id, action, value)
                return ["OK"] + state.status_lines()
            if cmd in ("QUIT", "EXIT", "BYE"):
                return ["BYE"]
            raise ValueError("Unknown command")


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, server_address, request_handler_class, gateway_state):
        self.gateway_state = gateway_state
        socketserver.TCPServer.__init__(self, server_address, request_handler_class)


def load_config(path):
    with open(path, "r") as handle:
        data = json.load(handle)
    if "devices" not in data or not data["devices"]:
        raise ValueError("Config must define at least one device")
    return data


def main(argv):
    config_path = DEFAULT_CONFIG
    if len(argv) > 1:
        config_path = argv[1]
    config = load_config(config_path)
    host = config.get("listen_host", DEFAULT_HOST)
    port = int(config.get("listen_port", DEFAULT_PORT))
    gateway_state = GatewayState(config)
    print("Starting Savant HVAC gateway %s on %s:%d using backend=%s" % (
        GATEWAY_VERSION, host, port, gateway_state.backend.backend_name
    ))
    server = ThreadedTCPServer((host, port), HvacRequestHandler, gateway_state)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
