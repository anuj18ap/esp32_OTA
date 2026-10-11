"""Laptop test runner. Hardware runs are explicit; host results never imply radio validation."""

import argparse
import asyncio
import datetime
import hashlib
import http.client
import json
import logging
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import threading
import zlib

from fault_server import make_server
from protocol import decrypt, encrypt, metadata

ROOT = Path(__file__).resolve().parent.parent
CASES = (
    "startup_busy",
    "oversize",
    "begin_failure",
    "persist_failure",
    "busy",
    "journal_pressure",
    "sequence",
    "length",
    "write_failure",
    "hash_failure",
    "timeout",
    "expiration",
    "cancel",
    "ble_disconnect",
    "mqtt_disconnect",
    "incomplete",
    "crc",
    "duplicate_changed",
    "reset_receiving",
    "image_invalid",
    "boot_failure",
    "version",
    "sha_mismatch",
    "duplicate",
    "success",
    "rollback",
    "repeat_ab",
    "relay_parser",
    "metadata_parse",
    "report_replay",
    "selection_intent",
    "interrupted_report_persistence",
    "rollback_report_persistence",
    "boot_cleanup_persistence",
)
HARDWARE = (
    "HTTP/HTTPS device download and failure recovery",
    "MQTT board/broker disconnect and result replay",
    "BLE iOS/Android pairing, upload and disconnect",
    "ESP-NOW two-board loss/retry transfer",
    "Startup rollback and repeated A/B updates",
    "GPIO relay/RGB and Arduino application compatibility",
)


def compile_native(idf, compiler):
    build = ROOT / "tests" / "native-build"
    build.mkdir(exist_ok=True)
    include = build / "include"
    include.mkdir(exist_ok=True)
    headers = (
        "esp_err.h",
        "esp_app_desc.h",
        "esp_flash.h",
        "esp_ota_ops.h",
        "esp_random.h",
        "nvs.h",
        "freertos/FreeRTOS.h",
        "freertos/semphr.h",
        "mbedtls/sha256.h",
    )
    for name in headers:
        path = include / name
        path.parent.mkdir(exist_ok=True)
        path.write_text('#include "native_stubs.h"\n')
    cjson = Path(idf) / "components" / "json" / "cJSON"
    mbedtls = Path(idf) / "components" / "mbedtls" / "mbedtls"
    executable = build / ("manager.exe" if os.name == "nt" else "manager")
    command = [compiler]
    if Path(compiler).stem == "zig":
        command += ["cc"]
        if os.name == "nt":
            command += ["-target", "x86_64-windows-gnu"]
    command += [
        "-std=c11",
        "-O0",
        "-g",
        "-I" + str(include),
        "-I" + str(ROOT / "tests"),
        "-I" + str(cjson),
        "-I" + str(mbedtls / "include"),
        '-DMBEDTLS_CONFIG_FILE="native_mbedtls_config.h"',
        str(ROOT / "tests" / "native_manager.c"),
        str(ROOT / "main" / "mqtt_crypto.c"),
        str(ROOT / "main" / "ota_protocol.c"),
        str(cjson / "cJSON.c"),
        str(mbedtls / "library" / "base64.c"),
        str(mbedtls / "library" / "constant_time.c"),
        "-o",
        str(executable),
    ]
    environment = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(build / "zig-cache"))
    output = subprocess.run(command, capture_output=True, text=True, env=environment, timeout=180)
    logging.info("Native compile: %s\n%s\n%s", command, output.stdout, output.stderr)
    if output.returncode:
        raise RuntimeError(output.stderr)
    return executable


def espnow_simulation(loss, ack_loss, seed):
    """Stop-and-wait model, separate from real radio and the firmware callbacks."""
    rng = random.Random(seed)
    image = bytes(range(256)) * 8
    next_sequence, received, retries, duplicates = 0, bytearray(), 0, 0
    for sequence, offset in enumerate(range(0, len(image), 224)):
        payload = image[offset : offset + 224]
        for attempt in range(8):
            if rng.random() < loss:
                retries += 1
                continue
            if sequence == next_sequence:
                received.extend(payload)
                next_sequence += 1
            elif sequence == next_sequence - 1:
                duplicates += 1
            else:
                raise AssertionError("out-of-order acceptance")
            if rng.random() < ack_loss:
                retries += 1
                continue
            break
        else:
            return {"status": "timeout", "boot_selected": False, "retries": retries}
    assert bytes(received) == image
    assert zlib.crc32(received) == zlib.crc32(image)
    return {"status": "complete", "retries": retries, "duplicates": duplicates}


def run_host(args, record):
    compiler = args.compiler or shutil.which("zig") or shutil.which("cc") or shutil.which("gcc")
    if compiler and args.idf:
        try:
            executable = compile_native(args.idf, compiler)
        except Exception as error:
            record("native compile", "firmware build or static validation", "failed", str(error))
        else:
            record("native compile", "firmware build or static validation", "passed")
            for case in CASES:
                output = subprocess.run(
                    [str(executable), case], capture_output=True, text=True, timeout=15
                )
                logging.info("%s: %s %s", case, output.stdout, output.stderr)
                record(
                    case,
                    "host-side unit test (actual C manager; mocked IDF/flash/SHA/NVS)",
                    "passed" if output.returncode == 0 else "failed",
                    output.stdout + output.stderr,
                )
            wire = encrypt("Arduino MQTT compatibility", bytes.fromhex("0102030405060708")).decode()
            output = subprocess.run(
                [str(executable), "crypto_wire", wire], capture_output=True, text=True, timeout=15
            )
            encrypted = next(
                (line for line in output.stdout.splitlines() if line.startswith("ENC1:")), ""
            )
            ok = (
                output.returncode == 0
                and encrypted
                and decrypt(encrypted.encode()) == "Arduino MQTT compatibility"
            )
            record(
                "actual C ENC1 interoperability with Python",
                "host-side unit test",
                "passed" if ok else "failed",
                output.stdout + output.stderr,
            )
    else:
        record(
            "native manager tests",
            "host-side unit test",
            "skipped",
            "Use --idf and --compiler to compile actual C sources",
        )
    for text in (
        "",
        "Arduino-compatible payload",
        '{"ssid":"example","password":"test"}',
        "Device \u03a9",
    ):
        assert decrypt(encrypt(text, bytes.fromhex("0102030405060708"))) == text
    record("ENC1 round trip", "host-side unit test (Python wire helper)", "passed")
    for name, loss, ack_loss, seed, expected in (
        ("ESP-NOW reliable", 0, 0, 1, "complete"),
        ("ESP-NOW loss/retries/duplicates", 0.2, 0.4, 10, "complete"),
        ("ESP-NOW total loss timeout", 1, 0, 1, "timeout"),
    ):
        result = espnow_simulation(loss, ack_loss, seed)
        assert result["status"] == expected, result
        record(name, "simulated transport test", "passed", result)
    fixture = ROOT / "tests" / "native-build" / "http-fixture.bin"
    fixture.parent.mkdir(exist_ok=True)
    fixture.write_bytes(bytes(range(256)) * 8)
    server = make_server(fixture, port=0)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for path in ("valid", "missing", "status", "truncated", "interrupted", "corrupt"):
            connection = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=5)
            connection.request("GET", f"/{path}.bin")
            response = connection.getresponse()
            try:
                body = response.read()
            except http.client.IncompleteRead as error:
                body = error.partial
                assert path == "interrupted"
            if path == "valid":
                assert response.status == 200 and body == fixture.read_bytes()
            elif path in ("missing", "status"):
                assert response.status == (404 if path == "missing" else 503)
            elif path in ("truncated", "interrupted"):
                assert len(body) == fixture.stat().st_size // 2
            else:
                assert body != fixture.read_bytes() and len(body) == fixture.stat().st_size
            record("HTTP fixture " + path, "host-side unit test (fault server only)", "passed")
            connection.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    config = (ROOT / "sdkconfig").read_text()
    binary = ROOT / "build-idf54" / "esp32_home_automation_ota.bin"
    if binary.exists():
        assert binary.stat().st_size < 0x1E0000
        assert "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" in config
        assert "CONFIG_PARTITION_TABLE_CUSTOM=y" in config
        record(
            "built binary fits A/B slots; rollback configuration",
            "firmware build or static validation",
            "passed",
            {
                "image_bytes": binary.stat().st_size,
                "slot_bytes": 0x1E0000,
                "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            },
        )
    else:
        record(
            "ESP-IDF binary check", "firmware build or static validation", "skipped", "Build first"
        )
    for name in HARDWARE:
        record(name, "real-device integration test", "hardware_required")
    record(
        "Controlled target power interruption at each OTA stage",
        "physical power-interruption test",
        "hardware_required",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report-dir", default=str(ROOT / "tests" / "results"))
    sub = parser.add_subparsers(dest="command", required=True)
    host = sub.add_parser("host")
    host.add_argument("--idf", default=os.getenv("IDF_PATH"))
    host.add_argument("--compiler")
    serve = sub.add_parser("serve")
    serve.add_argument("firmware")
    serve.add_argument("--bind", default="0.0.0.0")
    serve.add_argument("--port", type=int, default=8000)
    simulate = sub.add_parser("simulate-espnow")
    simulate.add_argument("--loss", type=float, default=0.2)
    simulate.add_argument("--ack-loss", type=float, default=0.4)
    for name in ("http", "mqtt", "status", "espnow-send"):
        command = sub.add_parser(name)
        command.add_argument("--device", required=True)
        command.add_argument("--broker", default="broker.emqx.io")
        command.add_argument("--port", type=int, default=1883)
        command.add_argument(
            "--key-file", help="Text file containing the provisioned 64-hex HMAC key"
        )
        if name in ("http", "mqtt"):
            command.add_argument("firmware")
        if name == "http":
            command.add_argument("--url", required=True)
            command.add_argument(
                "--expect", choices=("confirmed", "failed", "rolled_back"), default="confirmed"
            )
        if name == "mqtt":
            command.add_argument("--legacy", action="store_true")
            command.add_argument(
                "--fault",
                choices=(
                    "missing",
                    "duplicate",
                    "timeout",
                    "disconnect",
                    "incomplete",
                    "corrupt",
                    "wrong-size",
                    "concurrent",
                ),
            )
    ble = sub.add_parser("ble")
    ble.add_argument("firmware")
    ble.add_argument("--address", required=True)
    ble.add_argument("--disconnect", action="store_true")
    args = parser.parse_args()
    folder = Path(args.report_dir)
    folder.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(
        filename=folder / "ota-tests.log", level=logging.INFO, format="%(asctime)s %(message)s"
    )
    results = []

    def record(name, category, status, details=None):
        results.append(dict(name=name, category=category, status=status, details=details))
        print(f"{status.upper():18} {name}")
        logging.info("%s", results[-1])

    try:
        if args.command == "host":
            run_host(args, record)
        elif args.command == "serve":
            server = make_server(args.firmware, args.bind, args.port)
            print(f"Serving fault fixtures on port {server.server_port}; Ctrl+C to stop")
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass
            finally:
                server.server_close()
        elif args.command == "simulate-espnow":
            record(
                "ESP-NOW simulation",
                "simulated transport test",
                "passed",
                espnow_simulation(args.loss, args.ack_loss, 10),
            )
        elif args.command == "ble":
            from ble_driver import upload

            result = asyncio.run(upload(args.address, args.firmware, args.disconnect))
            record("BLE transfer", "real-device integration test", "passed", result)
            record(
                "BLE final boot verification",
                "real-device integration test",
                "hardware_required",
                "Observe MQTT result and verify slot separately",
            )
        else:
            from mqtt_driver import Device

            key = Path(args.key_file).read_text().strip() if args.key_file else None
            device = Device(args.device, args.broker, args.port, key)
            try:
                if args.command == "http":
                    result = device.http(args.firmware, args.url, args.expect)
                elif args.command == "mqtt":
                    result = device.mqtt(args.firmware, args.fault, args.legacy)
                elif args.command == "espnow-send":
                    device.request({"op": "espnow_send_running"})
                    result = {
                        "status": "command_sent",
                        "note": "Observe receiver confirmation with status/MQTT monitor",
                    }
                else:
                    result = device.snapshot()
                record(args.command, "real-device integration test", "passed", result)
            finally:
                device.close()
    except Exception as error:
        logging.exception("Test failed")
        record(args.command, "test execution", "failed", str(error))
    counts = {
        status: sum(r["status"] == status for r in results)
        for status in ("passed", "failed", "skipped", "hardware_required")
    }
    report = {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "summary": counts,
        "tests": results,
    }
    (folder / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(counts))
    return 1 if counts["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
