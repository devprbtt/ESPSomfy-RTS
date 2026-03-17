#!/usr/bin/env python3
import argparse
import math
import json
from pathlib import Path
import statistics
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

import requests


def fetch_state(base_url: str, timeout: Tuple[float, float] = (2.0, 5.0)) -> Dict[str, Any]:
    response = requests.get(f"{base_url}/api/state", timeout=timeout)
    response.raise_for_status()
    return response.json()


def fetch_last_frame(base_url: str) -> Optional[Dict[str, Any]]:
    response = requests.get(f"{base_url}/api/last-frame", timeout=(1.5, 2.5))
    response.raise_for_status()
    frame = response.json()
    if not frame or not frame.get("available"):
        return None
    return frame


def wait_for_new_frame(base_url: str, previous_id: int, timeout_s: float) -> Optional[Dict[str, Any]]:
    deadline = time.time() + timeout_s
    last_error: Optional[Exception] = None
    while time.time() < deadline:
        try:
            frame = fetch_last_frame(base_url)
        except requests.RequestException as exc:
            last_error = exc
            time.sleep(0.25)
            continue
        if frame and int(frame.get("id", 0)) > previous_id:
            return frame
        time.sleep(0.12)
    if last_error:
        print(f"  note: transient ESP/API timeout while polling ({last_error})")
    return None


def collect_frames(base_url: str, presses: int, window_s: float, lead_in_s: float) -> List[Dict[str, Any]]:
    frames: List[Dict[str, Any]] = []
    baseline = fetch_last_frame(base_url)
    last_id = int(baseline.get("id", 0)) if baseline else 0

    for idx in range(presses):
        print()
        print(f"Capture {idx + 1}/{presses}")
        print(f"Press the remote button after the countdown and hold it for about {window_s:.1f}s.")
        for countdown in range(int(lead_in_s), 0, -1):
            print(f"  starting in {countdown}...", flush=True)
            time.sleep(1)

        print("  capture window open: press now", flush=True)
        frame = wait_for_new_frame(base_url, last_id, timeout_s=window_s + 1.5)
        if not frame:
            print("  no new frame captured in this window")
            continue

        last_id = int(frame.get("id", last_id))
        frames.append(frame)
        print(
            f"  captured frame #{frame['id']} freq={frame['frequency']:.2f} "
            f"RSSI={frame['rssi']} pulses={frame['pulseCount']}"
        )

        time.sleep(0.5)

    return frames


def normalize_pulses(pulses: List[int]) -> Tuple[List[int], Dict[str, float]]:
    if not pulses:
        return [], {"short": 0.0, "long": 0.0}

    sorted_vals = sorted(pulses)
    median = statistics.median(sorted_vals)
    short_vals = [p for p in sorted_vals if p <= median]
    long_vals = [p for p in sorted_vals if p > median]
    short_avg = statistics.mean(short_vals) if short_vals else median
    long_avg = statistics.mean(long_vals) if long_vals else median

    normalized = []
    for pulse in pulses:
        if abs(pulse - short_avg) <= abs(pulse - long_avg):
            normalized.append(0)
        else:
            normalized.append(1)

    return normalized, {"short": short_avg, "long": long_avg}


def chunk_string(value: str, size: int) -> str:
    return " ".join(value[i:i + size] for i in range(0, len(value), size))


def position_variability(normalized_frames: List[List[int]]) -> List[float]:
    if not normalized_frames:
        return []
    width = len(normalized_frames[0])
    scores: List[float] = []
    for i in range(width):
        ones = sum(frame[i] for frame in normalized_frames)
        zeros = len(normalized_frames) - ones
        scores.append(min(ones, zeros) / len(normalized_frames))
    return scores


def bits_to_string(bits: List[int]) -> str:
    return "".join(str(bit) for bit in bits)


def slice_bits(bits: List[int], start: int, end: int) -> List[int]:
    if start < 0 or end < start:
        return []
    return bits[start:end + 1]


def pairwise_diff_string(reference: str, current: str) -> str:
    width = min(len(reference), len(current))
    markers = ["." if reference[i] == current[i] else "^" for i in range(width)]
    if len(current) > width:
        markers.extend("^" for _ in range(len(current) - width))
    return "".join(markers)


def looks_counter_like(payload_strings: List[str]) -> Dict[str, Any]:
    if len(payload_strings) < 3 or not payload_strings or any(not payload for payload in payload_strings):
        return {
            "looksCounterLike": False,
            "confidence": 0.0,
            "reason": "Not enough non-empty payload captures.",
        }

    try:
        values = [int(payload, 2) for payload in payload_strings]
    except ValueError:
        return {
            "looksCounterLike": False,
            "confidence": 0.0,
            "reason": "Payloads are not parseable as binary.",
        }

    unique_values = len(set(values))
    if unique_values < 3:
        return {
            "looksCounterLike": False,
            "confidence": 0.2,
            "reason": "Too few unique payload values across captures.",
        }

    deltas = [abs(values[i + 1] - values[i]) for i in range(len(values) - 1)]
    hamming_distances = []
    for i in range(len(payload_strings) - 1):
        a = payload_strings[i]
        b = payload_strings[i + 1]
        hamming_distances.append(sum(1 for x, y in zip(a, b) if x != y))

    monotonic = sum(1 for i in range(len(values) - 1) if values[i + 1] > values[i] or values[i + 1] < values[i])
    small_delta_ratio = sum(1 for delta in deltas if delta != 0 and delta < (1 << max(1, min(12, len(payload_strings[0]) // 4)))) / len(deltas)
    localized_changes_ratio = sum(1 for dist in hamming_distances if 0 < dist <= max(8, len(payload_strings[0]) // 4)) / len(hamming_distances)

    confidence = min(1.0, (small_delta_ratio * 0.45) + (localized_changes_ratio * 0.35) + ((monotonic / len(deltas)) * 0.20))
    looks_like = confidence >= 0.6
    if looks_like:
        reason = "Payload changes are relatively localized and progress in a counter-like way between captures."
    else:
        reason = "Payload changes look too broad or irregular to resemble a simple counter progression."

    return {
        "looksCounterLike": looks_like,
        "confidence": round(confidence, 3),
        "reason": reason,
        "deltas": deltas,
        "hammingDistances": hamming_distances,
    }


def bit_entropy(payload_strings: List[str]) -> List[float]:
    if not payload_strings:
        return []
    width = len(payload_strings[0])
    entropies: List[float] = []
    for i in range(width):
        ones = sum(1 for payload in payload_strings if payload[i] == "1")
        zeros = len(payload_strings) - ones
        if ones == 0 or zeros == 0:
            entropies.append(0.0)
            continue
        p0 = zeros / len(payload_strings)
        p1 = ones / len(payload_strings)
        entropies.append(-(p0 * math.log2(p0) + p1 * math.log2(p1)))
    return entropies


def bits_to_bytes(payload: str) -> List[str]:
    return [payload[i:i + 8] for i in range(0, len(payload), 8) if len(payload[i:i + 8]) == 8]


def byte_frequency_table(payload_strings: List[str]) -> List[Dict[str, Any]]:
    if not payload_strings:
        return []
    byte_count = len(payload_strings[0]) // 8
    table: List[Dict[str, Any]] = []
    for byte_index in range(byte_count):
        counts: Dict[str, int] = {}
        for payload in payload_strings:
            byte_value = payload[byte_index * 8:(byte_index + 1) * 8]
            counts[byte_value] = counts.get(byte_value, 0) + 1
        ranked = sorted(counts.items(), key=lambda item: (-item[1], item[0]))
        table.append(
            {
                "byteIndex": byte_index,
                "uniqueValues": len(counts),
                "topValues": [
                    {
                        "bits": bits,
                        "hex": f"0x{int(bits, 2):02X}",
                        "count": count,
                    }
                    for bits, count in ranked[:5]
                ],
            }
        )
    return table


def write_report_json(
    output_path: Path,
    frames: List[Dict[str, Any]],
    common_count: int,
    normalized_frames: List[List[int]],
    variable_positions: List[int],
    constant_prefix: int,
    constant_suffix: int,
    changing_start: int,
    changing_end: int,
    short_avgs: List[float],
    long_avgs: List[float],
) -> None:
    variability = position_variability(normalized_frames)
    normalized_bit_strings = [bits_to_string(bits) for bits in normalized_frames]
    payload_bit_strings = [bits_to_string(slice_bits(bits, changing_start, changing_end)) for bits in normalized_frames]
    counter_hint = looks_counter_like(payload_bit_strings)
    payload_entropies = bit_entropy(payload_bit_strings)
    payload_byte_frequencies = byte_frequency_table(payload_bit_strings)
    report = {
        "capturedFrameCount": len(frames),
        "frames": frames,
        "commonPulseCount": common_count,
        "constantPrefixLength": constant_prefix,
        "constantSuffixLength": constant_suffix,
        "changingRegion": {
            "start": changing_start,
            "end": changing_end,
            "length": (changing_end - changing_start + 1) if changing_start >= 0 else 0,
        },
        "variablePositions": variable_positions,
        "positionVariability": variability,
        "estimatedPulseAverages": {
            "short": statistics.mean(short_avgs) if short_avgs else 0.0,
            "long": statistics.mean(long_avgs) if long_avgs else 0.0,
        },
        "payloadAnalysis": {
            "payloadBits": payload_bit_strings,
            "payloadGrouped4": [chunk_string(bits, 4) for bits in payload_bit_strings],
            "payloadGrouped8": [chunk_string(bits, 8) for bits in payload_bit_strings],
            "bitEntropy": payload_entropies,
            "averageBitEntropy": statistics.mean(payload_entropies) if payload_entropies else 0.0,
            "highEntropyPositions": [i for i, entropy in enumerate(payload_entropies) if entropy >= 0.9],
            "byteFrequencies": payload_byte_frequencies,
            "counterHeuristic": counter_hint,
        },
        "normalizedFrames": [
            {
                "id": frame["id"],
                "rssi": frame["rssi"],
                "bits": bit_string,
                "bitsGrouped4": chunk_string(bit_string, 4),
                "bitsGrouped8": chunk_string(bit_string, 8),
                "payloadBits": payload,
                "payloadGrouped4": chunk_string(payload, 4),
                "payloadGrouped8": chunk_string(payload, 8),
            }
            for frame, bit_string, payload in zip(frames, normalized_bit_strings, payload_bit_strings)
        ],
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(report, indent=2))


def summarize_frames(frames: List[Dict[str, Any]]) -> str:
    if not frames:
        return "No frames captured."

    pulse_counts = [int(f["pulseCount"]) for f in frames]
    rssis = [int(f["rssi"]) for f in frames]
    freq = statistics.mean(float(f["frequency"]) for f in frames)

    lines = [
        f"Captured {len(frames)} frames",
        f"Average frequency: {freq:.3f} MHz",
        f"Pulse counts: {pulse_counts}",
        f"RSSI range: {min(rssis)} to {max(rssis)} dBm",
    ]

    common_count = statistics.mode(pulse_counts) if len(set(pulse_counts)) != len(pulse_counts) else pulse_counts[0]
    comparable = [f for f in frames if int(f["pulseCount"]) == common_count]
    lines.append(f"Frames with common pulse count ({common_count}): {len(comparable)}")

    if len(comparable) < 2:
        return "\n".join(lines)

    normalized_frames = []
    short_avgs = []
    long_avgs = []
    for frame in comparable:
        normalized, clusters = normalize_pulses([int(p) for p in frame["pulses"]])
        normalized_frames.append(normalized)
        short_avgs.append(clusters["short"])
        long_avgs.append(clusters["long"])

    variable_positions = []
    constant_prefix = 0
    constant_suffix = 0
    for i in range(common_count):
        values = [nf[i] for nf in normalized_frames]
        if len(set(values)) > 1:
            variable_positions.append(i)

    for i in range(common_count):
        values = [nf[i] for nf in normalized_frames]
        if len(set(values)) == 1:
            constant_prefix += 1
        else:
            break

    for i in range(common_count - 1, -1, -1):
        values = [nf[i] for nf in normalized_frames]
        if len(set(values)) == 1:
            constant_suffix += 1
        else:
            break

    stable_positions = [i for i in range(common_count) if i not in variable_positions]
    changing_start = variable_positions[0] if variable_positions else -1
    changing_end = variable_positions[-1] if variable_positions else -1
    changing_length = (changing_end - changing_start + 1) if changing_start >= 0 else 0
    variability = position_variability(normalized_frames)
    avg_variability = statistics.mean(variability) if variability else 0.0

    lines.extend(
        [
            f"Estimated short pulse avg: {statistics.mean(short_avgs):.1f} us",
            f"Estimated long pulse avg: {statistics.mean(long_avgs):.1f} us",
            f"Variable normalized pulse positions: {len(variable_positions)}",
            f"Constant prefix length: {constant_prefix}",
            f"Constant suffix length: {constant_suffix}",
            f"Changing region: {changing_start}..{changing_end}" if variable_positions else "Changing region: none",
            f"Changing region length: {changing_length}",
            f"Average per-position variability: {avg_variability:.3f}",
        ]
    )

    preview = variable_positions[:30]
    if preview:
        lines.append(f"First changing positions: {preview}")
    stable_preview = stable_positions[:30]
    if stable_preview:
        lines.append(f"First stable positions: {stable_preview}")

    lines.append("")
    lines.append("Field map:")
    if variable_positions:
        field_map = []
        for i in range(common_count):
            field_map.append("V" if i in variable_positions else "C")
        lines.append("  " + "".join(field_map[:120]))
        if common_count > 120:
            lines.append("  " + "".join(field_map[120:240]))

    lines.append("")
    lines.append("Grouped normalized streams:")
    for frame, bits in zip(comparable, normalized_frames):
        bit_string = bits_to_string(bits)
        lines.append(f"  #{frame['id']} 4b  {chunk_string(bit_string, 4)}")
        lines.append(f"  #{frame['id']} 8b  {chunk_string(bit_string, 8)}")

    if variable_positions:
        payload_strings = [bits_to_string(slice_bits(bits, changing_start, changing_end)) for bits in normalized_frames]
        payload_entropies = bit_entropy(payload_strings)
        payload_byte_frequencies = byte_frequency_table(payload_strings)
        lines.append("")
        lines.append("Payload-only normalized streams:")
        for frame, payload in zip(comparable, payload_strings):
            lines.append(f"  #{frame['id']} 4b  {chunk_string(payload, 4)}")
            lines.append(f"  #{frame['id']} 8b  {chunk_string(payload, 8)}")

        reference_frame = comparable[0]
        reference_payload = payload_strings[0]
        lines.append("")
        lines.append(f"Payload diffs vs frame #{reference_frame['id']}:")
        for frame, payload in zip(comparable, payload_strings):
            lines.append(f"  #{frame['id']} bits {payload}")
            lines.append(f"  #{frame['id']} diff {pairwise_diff_string(reference_payload, payload)}")

        counter_hint = looks_counter_like(payload_strings)
        lines.append("")
        lines.append("Counter heuristic:")
        verdict = "looks counter-like" if counter_hint["looksCounterLike"] else "does not look counter-like"
        lines.append(f"  Verdict: {verdict} (confidence {counter_hint['confidence']:.3f})")
        lines.append(f"  Reason: {counter_hint['reason']}")
        if counter_hint.get("deltas"):
            lines.append(f"  Integer deltas: {counter_hint['deltas']}")
        if counter_hint.get("hammingDistances"):
            lines.append(f"  Hamming distances: {counter_hint['hammingDistances']}")

        lines.append("")
        lines.append("Payload entropy:")
        if payload_entropies:
            lines.append(f"  Average bit entropy: {statistics.mean(payload_entropies):.3f}")
            high_entropy_positions = [i for i, entropy in enumerate(payload_entropies) if entropy >= 0.9]
            low_entropy_positions = [i for i, entropy in enumerate(payload_entropies) if entropy <= 0.2]
            lines.append(f"  High-entropy payload positions: {high_entropy_positions[:32]}")
            lines.append(f"  Low-entropy payload positions: {low_entropy_positions[:32]}")
            lines.append(f"  Entropy by payload bit: {[round(value, 3) for value in payload_entropies]}")

        lines.append("")
        lines.append("Payload byte frequency:")
        for entry in payload_byte_frequencies:
            top_values = ", ".join(
                f"{item['hex']}({item['bits']}) x{item['count']}"
                for item in entry["topValues"]
            )
            lines.append(
                f"  Byte {entry['byteIndex']}: {entry['uniqueValues']} unique values"
                + (f" | top {top_values}" if top_values else "")
            )

    lines.append("")
    lines.append("Per-frame details:")
    for frame in comparable:
        pulses = [int(p) for p in frame["pulses"]]
        normalized, clusters = normalize_pulses(pulses)
        bit_preview = "".join(str(b) for b in normalized[:80])
        lines.append(
            f"  #{frame['id']} RSSI={frame['rssi']} short≈{clusters['short']:.1f} "
            f"long≈{clusters['long']:.1f} bits[:80]={bit_preview}"
        )

    output_path = Path("CC1101-RF-Monitor/tools/reports/latest_capture_report.json")
    write_report_json(
        output_path=output_path,
        frames=comparable,
        common_count=common_count,
        normalized_frames=normalized_frames,
        variable_positions=variable_positions,
        constant_prefix=constant_prefix,
        constant_suffix=constant_suffix,
        changing_start=changing_start,
        changing_end=changing_end,
        short_avgs=short_avgs,
        long_avgs=long_avgs,
    )
    lines.append("")
    lines.append(f"JSON report written to: {output_path}")

    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture and analyze repeated RF button presses from the ESP RF monitor.")
    parser.add_argument("--host", default="192.168.4.1", help="ESP host or IP")
    parser.add_argument("--presses", type=int, default=12, help="Number of button presses to collect")
    parser.add_argument("--window", type=float, default=3.0, help="Seconds to allow for each capture window")
    parser.add_argument("--lead-in", type=float, default=3.0, help="Countdown seconds before each capture")
    args = parser.parse_args()

    base_url = f"http://{args.host}"
    print(f"Connecting to {base_url}")

    try:
        state = fetch_state(base_url)
    except Exception as exc:
        print(f"Failed to reach ESP at {base_url}: {exc}")
        return 1

    print(
        f"Connected. AP={state.get('apSsid', '') or state.get('apIp', '')} "
        f"radio={state['radio']['frequency']:.2f}MHz"
    )
    print("The script will collect one fresh frame per capture window and then compare them.")

    frames = collect_frames(base_url, presses=args.presses, window_s=args.window, lead_in_s=args.lead_in)
    print()
    print(summarize_frames(frames))
    return 0


if __name__ == "__main__":
    sys.exit(main())
