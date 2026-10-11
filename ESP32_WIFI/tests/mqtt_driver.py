import json
import logging
import queue
import struct
import time
from pathlib import Path
from protocol import decrypt, metadata, signed_request


class Device:
    def __init__(self, device_id, broker="broker.emqx.io", port=1883, key=None):
        import paho.mqtt.client as mqtt

        self.id, self.key, self.events = device_id.upper(), key, queue.Queue()
        self.history = []
        self.missed_reports = []
        self.seen_reports = set()
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.client.on_connect = self._connected
        self.client.on_message = self._message
        self.client.connect(broker, port, 60)
        self.client.loop_start()
        self.wait(lambda e: e.get("connected"), 15)

    def _connected(self, client, userdata, flags, rc, properties):
        if rc == 0:
            client.subscribe(f"{self.id}/ota/#", qos=1)
            client.subscribe(f"{self.id}/ota_status", qos=1)
            client.subscribe(f"{self.id}/log", qos=0)
            self.events.put({"connected": True})

    def _message(self, client, userdata, message):
        try:
            text = decrypt(message.payload)
            event = json.loads(text) if text.startswith("{") else {"text": text}
            event["topic"] = message.topic
        except (ValueError, UnicodeError):
            event = {"topic": message.topic, "decode_error": True}
        self.history.append(event)
        report = event.get("report_id")
        if report:
            event["duplicate_report"] = report in self.seen_reports
            self.seen_reports.add(report)
        logging.info("device_event %s", json.dumps(event))
        self.events.put(event)

    def publish(self, suffix, data, qos=1):
        message = self.client.publish(f"{self.id}/{suffix}", data, qos=qos)
        message.wait_for_publish(timeout=10)
        if not message.is_published():
            raise TimeoutError("broker did not acknowledge publication")

    def wait(self, predicate, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                event = self.events.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if predicate(event):
                return event
        raise TimeoutError("expected device event not received")

    def request(self, fields):
        if not self.key:
            raise ValueError("provisioned OTA key required for extended commands")
        self.publish("ota/request", signed_request(fields, self.key))

    def snapshot(self):
        self.request({"op": "status"})
        return self.wait(lambda e: e.get("status") == "snapshot")

    def http(self, firmware, url, expect="confirmed"):
        before = self.snapshot()
        self.request({"op": "http", "url": url, **metadata(firmware, 1024)})
        start = self.wait(
            lambda e: e.get("status") == "started"
            or e.get("status") == "failed"
            and not e.get("session_id"),
            30,
        )
        session = start.get("session_id")
        result = (
            start
            if not session
            else self.wait(
                lambda e: e.get("session_id") == session
                and e.get("status") in ("failed", "confirmed", "rolled_back"),
                180,
            )
        )
        if result["status"] != expect:
            raise AssertionError(f"expected {expect}, got {result}")
        after = self.snapshot()
        if expect == "failed":
            assert after["current_slot"] == before["current_slot"]
            assert after["boot_slot"] == before["boot_slot"], "failed OTA changed boot selection"
        elif expect == "confirmed":
            assert (
                after["current_slot"] != before["current_slot"]
            ), "successful update did not switch slot"
        else:
            assert (
                after["current_slot"] == before["current_slot"]
            ), "rollback did not restore previous slot"
        return {"before": before, "result": result, "after": after, "events": self.history}

    def mqtt(self, firmware, fault=None, legacy=False):
        image = Path(firmware).read_bytes()
        chunk_size = 7168
        meta = metadata(firmware, chunk_size)
        if fault == "wrong-size":
            meta["size"] += 1
        before = self.snapshot() if self.key else None
        if legacy:
            self.publish("ota/begin", json.dumps(meta))
            session = self.wait(lambda e: e.get("status") == "started")["session_id"]
            self.wait(lambda e: e.get("text") == "UPDATING")
        else:
            self.request({"op": "mqtt", **meta})
            session = self.wait(lambda e: e.get("status") == "started")["session_id"]
        if fault == "concurrent":
            if not self.key:
                raise ValueError("concurrent test requires a provisioned key")
            self.request({"op": "mqtt", **meta})
            self.wait(lambda e: e.get("error_code") == "OTA_BUSY", 30)
        for sequence, offset in enumerate(range(0, len(image), chunk_size)):
            if fault == "incomplete" and sequence == meta["chunks"] - 1:
                break
            if fault == "missing" and sequence == 0:
                continue
            packet = struct.pack(">I", sequence) + image[offset : offset + chunk_size]
            suffix = "ota/chunk" if legacy else "ota/v2/chunk"
            if not legacy:
                packet = struct.pack(">I", session) + packet
            if fault == "corrupt" and sequence == 0:
                # Keep the ESP image header intact so corruption is detected at final integrity checks.
                packet = packet[:-1] + bytes([packet[-1] ^ 1])
            self.publish(suffix, packet, qos=0)
            if fault == "disconnect":
                self.client.disconnect()
                time.sleep(35)
                self.client.reconnect()
                break
            if fault == "timeout":
                time.sleep(35)
                break
            if fault == "missing":
                break
            predicate = lambda e: (
                e.get("text") == str(sequence)
                if legacy
                else e.get("sequence") == sequence and e.get("session_id") == session
            )
            for attempt in range(8):
                try:
                    self.wait(predicate, 3)
                    break
                except TimeoutError:
                    if attempt == 7:
                        raise
                    self.publish(suffix, packet, qos=0)
            if fault == "duplicate":
                self.publish(suffix, packet, qos=0)
                self.wait(
                    lambda e: (
                        e.get("text") == str(sequence)
                        if legacy
                        else e.get("sequence") == sequence and e.get("session_id") == session
                    ),
                    10,
                )
        if fault not in ("timeout", "disconnect", "missing"):
            self.publish(
                "ota/end" if legacy else "ota/v2/end",
                b"END" if legacy else struct.pack(">I", session),
            )
        result = self.wait(
            lambda e: e.get("session_id") == session
            and e.get("status") in ("confirmed", "failed", "rolled_back"),
            180,
        )
        expected = (
            "failed"
            if fault in ("missing", "disconnect", "timeout", "incomplete", "corrupt", "wrong-size")
            else "confirmed"
        )
        assert result["status"] == expected, result
        after = self.snapshot() if self.key else None
        if expected == "failed" and before and after:
            assert (
                before["boot_slot"] == after["boot_slot"]
                and before["current_slot"] == after["current_slot"]
            )
        return {"before": before, "result": result, "after": after, "events": self.history}

    def close(self):
        self.client.disconnect()
        self.client.loop_stop()
