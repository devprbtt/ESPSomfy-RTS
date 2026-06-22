#!/usr/bin/env python3
"""Deploy the experimental Savant HVAC gateway to a Savant host.

This tool is intended to help future operators or other agents repeat the
manual steps we already proved out:

- copy the helper to the Savant host
- copy a JSON config file
- optionally stop a previous helper instance
- optionally start the helper in the background
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


DEFAULT_REMOTE_DIR = "/data/codex_probe"
DEFAULT_REMOTE_SCRIPT = "savant_hvac_gateway.py"
DEFAULT_REMOTE_CONFIG = "savant_hvac_gateway.json"


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)


def build_expect_scp(files: list[str], user: str, host: str, password: str, remote_dir: str) -> str:
    joined = " ".join(files)
    return (
        "set timeout 30; "
        "spawn scp {files} {user}@{host}:{remote}; "
        "expect \"*?assword:*\" {{ send \"{password}\\r\" }}; "
        "expect eof"
    ).format(files=joined, user=user, host=host, remote=remote_dir, password=password)


def build_expect_ssh(command: str, user: str, host: str, password: str) -> str:
    return (
        "set timeout 30; "
        "spawn ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "
        "{user}@{host} \"{command}\"; "
        "expect \"*?assword:*\" {{ send \"{password}\\r\" }}; "
        "expect eof"
    ).format(user=user, host=host, command=command.replace("\"", "\\\""), password=password)


def ensure_files(config_path: Path) -> tuple[Path, Path]:
    repo_root = Path(__file__).resolve().parent
    script_path = repo_root / DEFAULT_REMOTE_SCRIPT
    if not script_path.exists():
        raise SystemExit("Missing helper script: %s" % script_path)
    if not config_path.exists():
        raise SystemExit("Missing config file: %s" % config_path)
    return script_path, config_path


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Deploy the experimental Savant HVAC gateway.")
    parser.add_argument("--host", required=True, help="Savant host IP or hostname")
    parser.add_argument("--user", default="RPM", help="SSH username (default: RPM)")
    parser.add_argument("--password", required=True, help="SSH password")
    parser.add_argument("--config", default=str(Path(__file__).with_name("savant_hvac_gateway.example.json")),
                        help="Path to local JSON config to deploy")
    parser.add_argument("--remote-dir", default=DEFAULT_REMOTE_DIR, help="Remote deployment directory")
    parser.add_argument("--start", action="store_true", help="Start the helper after copying files")
    parser.add_argument("--stop-first", action="store_true", help="Stop any previous helper instance before starting")
    args = parser.parse_args(argv)

    config_path = Path(args.config).resolve()
    script_path, config_path = ensure_files(config_path)

    expect_mkdir = build_expect_ssh("mkdir -p %s" % args.remote_dir, args.user, args.host, args.password)
    run(["expect", "-c", expect_mkdir])

    expect_scp = build_expect_scp(
        [str(script_path), str(config_path)],
        args.user,
        args.host,
        args.password,
        args.remote_dir + "/",
    )
    run(["expect", "-c", expect_scp])

    remote_config_name = Path(config_path).name
    remote_script_name = script_path.name

    if args.start:
        command_parts = ["cd %s" % args.remote_dir]
        if args.stop_first:
            command_parts.append(
                "PID=$(cat hvac_gateway.pid 2>/dev/null || true); "
                "if [ -n \"$PID\" ]; then kill \"$PID\" >/dev/null 2>&1 || true; fi"
            )
        command_parts.append(
            "nohup python {script} {config} > hvac_gateway.log 2>&1 & echo $! > hvac_gateway.pid; "
            "sleep 1; echo PID:$(cat hvac_gateway.pid); "
            "sed -n '1,20p' hvac_gateway.log"
            .format(script=remote_script_name, config=remote_config_name)
        )
        expect_start = build_expect_ssh("; ".join(command_parts), args.user, args.host, args.password)
        run(["expect", "-c", expect_start])

    print("\nDeployment complete.")
    print("Helper script: %s" % script_path.name)
    print("Remote config: %s" % remote_config_name)
    print("Savant target: %s:%s" % (args.host, "2323"))
    if args.start:
        print("The helper was started in the background.")
    else:
        print("The helper was copied only. Start it manually when ready.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
