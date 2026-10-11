"""Optional real BLE adapter; requires bonded OS adapter and bleak."""

import asyncio
import json
import struct
from pathlib import Path
from protocol import metadata

CONTROL = "8e400002-b5a3-f393-e0a9-e50e24dcca9e"
DATA = "8e400003-b5a3-f393-e0a9-e50e24dcca9e"
STATUS = "8e400004-b5a3-f393-e0a9-e50e24dcca9e"


async def upload(address, firmware, disconnect=False):
    from bleak import BleakClient

    events = asyncio.Queue()

    def notification(sender, data):
        if len(data) == 16:
            events.put_nowait(struct.unpack(">IIII", data))

    async with BleakClient(address) as device:
        # Pair in the OS first with the provisioned six-digit passkey.
        await device.start_notify(STATUS, notification)
        mtu = device.mtu_size
        payload_size = min(mtu - 3 - 8, 244)
        if payload_size <= 0:
            raise RuntimeError("BLE MTU does not fit session and sequence header")
        image = Path(firmware).read_bytes()
        start = json.dumps({"op": "start", **metadata(firmware, payload_size)}).encode()
        await device.write_gatt_char(CONTROL, start, response=True)
        session, sequence, result, operation = await asyncio.wait_for(events.get(), 30)
        if result != 0 or operation != 1:
            raise RuntimeError(f"BLE start rejected: {result}")
        for sequence, offset in enumerate(range(0, len(image), payload_size)):
            await device.write_gatt_char(
                DATA,
                struct.pack(">II", session, sequence) + image[offset : offset + payload_size],
                response=True,
            )
            ack = await asyncio.wait_for(events.get(), 10)
            if ack[:3] != (session, sequence, 0):
                raise RuntimeError(f"BLE chunk rejected: {ack}")
            if disconnect:
                return {
                    "session_id": session,
                    "status": "disconnected",
                    "confirmation": "observe failed MQTT result and unchanged boot_slot",
                }
        await device.write_gatt_char(
            CONTROL, json.dumps({"op": "finish", "session_id": session}).encode(), response=True
        )
        ack = await asyncio.wait_for(events.get(), 30)
        if ack[2] != 0:
            raise RuntimeError(f"BLE finalize rejected: {ack}")
        return {
            "session_id": session,
            "status": "boot_selected",
            "confirmation": "observe confirmed MQTT result after reboot",
        }
