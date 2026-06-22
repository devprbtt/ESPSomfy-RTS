#!/usr/bin/env python3
"""Scan a subnet for Gree HVACs and print Savant JSON snippets.

This is the operator-side companion to the Savant-host gateway. It is meant to
be run from a laptop on the same LAN as the Gree units before building the
final Savant config JSON.
"""

from __future__ import annotations

import argparse
import base64
import ipaddress
import json
import socket
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Any

try:
    from Crypto.Cipher import AES
except ImportError:
    AES = None


DEFAULT_PORT = 7000
DEFAULT_TIMEOUT = 1.5
DEFAULT_WORKERS = 48
GCM_IV = b"\x54\x40\x78\x44\x49\x67\x5a\x51\x6c\x5e\x63\x13"
GCM_ADD = b"qualcomm-test"
GENERIC_GREE_DEVICE_KEY = b"a3K8Bx%2r8Y7#xDh"
GENERIC_GREE_DEVICE_KEY_GCM = b"{yxAHAY_Lm6pbC/<"
STATUS_COLS = [
    "Pow",
    "Mod",
    "SetTem",
    "WdSpd",
    "TemSen",
    "TemRec",
    "Lig",
    "Blo",
    "Health",
    "Air",
]
CODE_TO_MODE = {
    0: "auto",
    1: "cool",
    2: "dry",
    3: "fan_only",
    4: "heat",
}
CODE_TO_FAN = {
    0: "auto",
    1: "low",
    2: "medium_low",
    3: "medium",
    4: "medium_high",
    5: "high",
}


def ensure_aes() -> None:
    if AES is None:
        raise SystemExit("Missing dependency: Crypto.Cipher.AES. Install pycryptodome or pycrypto first.")


def normalize_mac(mac: str) -> str:
    return (mac or "").replace(":", "").replace("-", "").lower()


def pad(value: str) -> bytes:
    block_size = 16
    pad_len = block_size - len(value) % block_size
    return (value + chr(pad_len) * pad_len).encode("utf-8")


def unpad_text(text: str) -> str:
    if not text:
        return text
    pad_len = ord(text[-1])
    if 1 <= pad_len <= 16:
        return text[:-pad_len]
    return text.replace("\x0f", "")


def gcm_cipher(key: bytes):
    cipher = AES.new(key, AES.MODE_GCM, nonce=GCM_IV)
    cipher.update(GCM_ADD)
    return cipher


def encrypt_gcm(key: bytes, plaintext: str) -> tuple[str, str]:
    cipher = gcm_cipher(key)
    encrypted_data, tag = cipher.encrypt_and_digest(plaintext.encode("utf-8"))
    return base64.b64encode(encrypted_data).decode("utf-8"), base64.b64encode(tag).decode("utf-8")


def decode_pack_v1(pack: str) -> dict[str, Any]:
    cipher = AES.new(GENERIC_GREE_DEVICE_KEY, AES.MODE_ECB)
    decoded = cipher.decrypt(base64.b64decode(pack)).decode("utf-8", errors="ignore")
    decoded = unpad_text(decoded)
    last_brace = decoded.rfind("}")
    if last_brace >= 0:
        decoded = decoded[: last_brace + 1]
    return json.loads(decoded)


def recv_json(host: str, port: int, payload: bytes, timeout: float) -> dict[str, Any]:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(payload, (host, port))
        data, _ = sock.recvfrom(65535)
        return json.loads(data.decode("utf-8", errors="ignore"))
    finally:
        sock.close()


def decrypt_response(response: dict[str, Any], key: bytes, encryption_version: int) -> dict[str, Any]:
    decoded_pack = base64.b64decode(response["pack"])
    if encryption_version == 2:
        cipher = gcm_cipher(key)
        decrypted = cipher.decrypt(decoded_pack)
        if response.get("tag"):
            cipher.verify(base64.b64decode(response["tag"]))
    else:
        cipher = AES.new(key, AES.MODE_ECB)
        decrypted = cipher.decrypt(decoded_pack)
    decoded_text = decrypted.decode("utf-8", errors="ignore")
    decoded_text = unpad_text(decoded_text)
    last_brace = decoded_text.rfind("}")
    if last_brace >= 0:
        decoded_text = decoded_text[: last_brace + 1]
    return json.loads(decoded_text)


def request_pack(host: str, port: int, payload_obj: dict[str, Any], key: bytes, encryption_version: int, timeout: float) -> dict[str, Any]:
    response = recv_json(
        host,
        port,
        json.dumps(payload_obj, separators=(",", ":")).encode("utf-8"),
        timeout,
    )
    return decrypt_response(response, key, encryption_version)


def discover_host(host: str, port: int, timeout: float) -> dict[str, Any] | None:
    try:
        response = recv_json(host, port, b'{"t":"scan"}', timeout)
    except Exception:
        return None

    result: dict[str, Any] = {"host": host, "port": port, "raw": response}
    try:
        if "pack" in response:
            result["device"] = decode_pack_v1(response["pack"])
        else:
            result["device"] = response
    except Exception as exc:
        result["decode_error"] = str(exc)
    return result


def get_device_key_v1(mac_addr: str, ip_addr: str, port: int, timeout: float) -> bytes | None:
    cipher = AES.new(GENERIC_GREE_DEVICE_KEY, AES.MODE_ECB)
    pack = base64.b64encode(cipher.encrypt(pad('{"mac":"%s","t":"bind","uid":0}' % mac_addr))).decode("utf-8")
    payload = {"cid": "app", "i": 1, "pack": pack, "t": "pack", "tcid": mac_addr, "uid": 0}
    try:
        result = request_pack(ip_addr, port, payload, GENERIC_GREE_DEVICE_KEY, 1, timeout)
        return result["key"].encode("utf-8")
    except Exception:
        return None


def get_device_key_v2(mac_addr: str, ip_addr: str, port: int, timeout: float) -> bytes | None:
    plaintext = json.dumps({"cid": mac_addr, "mac": mac_addr, "t": "bind", "uid": 0}, separators=(",", ":"))
    pack, tag = encrypt_gcm(GENERIC_GREE_DEVICE_KEY_GCM, plaintext)
    payload = {"cid": "app", "i": 1, "pack": pack, "t": "pack", "tcid": mac_addr, "uid": 0, "tag": tag}
    try:
        result = request_pack(ip_addr, port, payload, GENERIC_GREE_DEVICE_KEY_GCM, 2, timeout)
        return result["key"].encode("utf-8")
    except Exception:
        return None


def detect_encryption(mac_addr: str, ip_addr: str, port: int, timeout: float) -> tuple[int | None, bytes | None]:
    key = get_device_key_v1(mac_addr, ip_addr, port, timeout)
    if key:
        return 1, key
    key = get_device_key_v2(mac_addr, ip_addr, port, timeout)
    if key:
        return 2, key
    return None, None


def get_status(mac_addr: str, ip_addr: str, port: int, encryption_version: int, key: bytes, timeout: float) -> dict[str, Any]:
    plaintext = json.dumps({"cols": STATUS_COLS, "mac": mac_addr, "t": "status"}, separators=(",", ":"))
    if encryption_version == 1:
        cipher = AES.new(key, AES.MODE_ECB)
        pack = base64.b64encode(cipher.encrypt(pad(plaintext))).decode("utf-8")
        payload = {"cid": "app", "i": 0, "pack": pack, "t": "pack", "tcid": mac_addr, "uid": 0}
        return request_pack(ip_addr, port, payload, key, 1, timeout)

    pack, tag = encrypt_gcm(key, plaintext)
    payload = {"cid": "app", "i": 0, "pack": pack, "t": "pack", "tcid": mac_addr, "uid": 0, "tag": tag}
    return request_pack(ip_addr, port, payload, key, 2, timeout)


def decode_status(status: dict[str, Any]) -> dict[str, Any]:
    cols = status.get("cols", [])
    dat = status.get("dat", [])
    if len(dat) == 1 and isinstance(dat[0], list):
        dat = dat[0]
    values = {}
    for index, col in enumerate(cols):
        if index < len(dat):
            values[col] = dat[index]

    target_temp = values.get("SetTem")
    if target_temp is not None and values.get("TemRec") == 1:
        target_temp = target_temp + 0.5
    current_temp = values.get("TemSen")
    if current_temp is not None and current_temp >= 40:
        current_temp = current_temp - 40
    return {
        "power": "on" if values.get("Pow") == 1 else "off",
        "mode": CODE_TO_MODE.get(values.get("Mod"), values.get("Mod")),
        "fan": CODE_TO_FAN.get(values.get("WdSpd"), values.get("WdSpd")),
        "target_temp_c": target_temp,
        "current_temp_c": current_temp,
    }


def build_entry(result: dict[str, Any], thermostat_id: int) -> dict[str, Any]:
    return {
        "thermostat_id": thermostat_id,
        "host": result["host"],
        "port": result["port"],
        "mac": result["mac"],
        "encryption_version": result["encryption_version"],
        "key": result["key"],
    }


def probe_host(host: str, port: int, timeout: float) -> dict[str, Any] | None:
    discovered = discover_host(host, port, timeout)
    if not discovered:
        return None

    device = discovered.get("device", {})
    mac = normalize_mac(device.get("mac", ""))
    if not mac:
        return {
            "host": host,
            "port": port,
            "error": "Discovery responded but MAC could not be determined",
            "discovery": discovered,
        }

    encryption_version, key = detect_encryption(mac, host, port, timeout)
    if encryption_version is None or key is None:
        return {
            "host": host,
            "port": port,
            "mac": mac,
            "error": "Could not retrieve encryption key",
            "discovery": discovered,
        }

    result = {
        "host": host,
        "port": port,
        "mac": mac,
        "encryption_version": encryption_version,
        "key": key.decode("utf-8", errors="ignore"),
        "discovery": discovered,
    }

    try:
        status = get_status(mac, host, port, encryption_version, key, timeout)
        result["status_raw"] = status
        result["status_decoded"] = decode_status(status)
    except Exception as exc:
        result["status_error"] = str(exc)

    return result


def hosts_from_args(subnet: str | None, hosts: list[str]) -> list[str]:
    if hosts:
        return hosts
    if not subnet:
        raise SystemExit("Provide either --subnet or one or more --host values")
    network = ipaddress.ip_network(subnet, strict=False)
    return [str(ip) for ip in network.hosts()]


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Scan a subnet for Gree HVACs and print Savant JSON entries.")
    parser.add_argument("--subnet", help="Subnet to scan, for example 192.168.5.0/24")
    parser.add_argument("--host", action="append", default=[], help="Specific host to probe; repeatable")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="UDP port, default 7000")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="Per-host timeout in seconds")
    parser.add_argument("--workers", type=int, default=DEFAULT_WORKERS, help="Concurrent probes")
    parser.add_argument("--start-id", type=int, default=1, help="Starting thermostat ID for generated JSON")
    parser.add_argument("--pretty", action="store_true", help="Pretty-print more raw discovery data")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    ensure_aes()
    args = parse_args(argv)
    hosts = hosts_from_args(args.subnet, args.host)

    print("Scanning %d host(s) on UDP %d..." % (len(hosts), args.port))
    results: list[dict[str, Any]] = []
    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as executor:
        future_map = {
            executor.submit(probe_host, host, args.port, args.timeout): host
            for host in hosts
        }
        for future in as_completed(future_map):
            host = future_map[future]
            try:
                result = future.result()
            except Exception as exc:
                print("[!] %s probe failed: %s" % (host, exc))
                continue
            if result is None:
                continue
            results.append(result)
            if result.get("error"):
                print("[?] %s responded but is incomplete: %s" % (host, result["error"]))
            else:
                print("[+] %s mac=%s enc=v%s" % (host, result["mac"], result["encryption_version"]))

    good_results = [item for item in results if not item.get("error")]
    good_results.sort(key=lambda item: tuple(int(part) for part in item["host"].split(".")))

    print("")
    print("Discovered %d candidate(s), %d fully usable." % (len(results), len(good_results)))

    if not good_results:
        print("No complete Gree entries were found.")
        if results and args.pretty:
            print(json.dumps(results, indent=2))
        return 1

    entries = []
    devices = []
    for index, result in enumerate(good_results, start=args.start_id):
        entries.append(build_entry(result, index))
        name = "Gree HVAC %d" % index
        if result.get("status_decoded"):
            name = "Gree HVAC %d (%s)" % (index, result["host"])
        devices.append(
            {
                "thermostat_id": index,
                "name": name,
                "mode": "OFF",
                "power": False,
                "setpoint": 24,
                "current_temp": int(round(result.get("status_decoded", {}).get("current_temp_c", 25) or 25)),
                "fan": "FANAUTO",
            }
        )

    print("Ready-to-paste gree_local.devices:")
    print(json.dumps(entries, indent=2))
    print("")
    print("Suggested top-level devices:")
    print(json.dumps(devices, indent=2))
    print("")
    print("Suggested full config merge:")
    print(json.dumps({"devices": devices, "gree_local": {"devices": entries}}, indent=2))

    if args.pretty:
        print("")
        print("Detailed probe results:")
        print(json.dumps(good_results, indent=2))

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
