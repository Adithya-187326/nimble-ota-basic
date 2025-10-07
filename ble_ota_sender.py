import asyncio
import os
from bleak import BleakClient as Client, BleakScanner as Scanner

# === Config ===
# TODO Update with the name of your ESP32 server
OTA_DEVICE_NAME = "nimble-ota-server"
# TODO Update with the path to your new ESP32 firmware
FIRMWARE_PATH = "new-firmware.bin"

# UUIDs
# TODO Update with whatever your ESP32 server uses
OTA_FIRMWARE_CHR_UUID     = "01010101-0101-0101-0101-010101010101"
OTA_COMMANDS_CHR_UUID     = "10101010-1010-1010-1010-101010101010"

async def main():
    print("Scanning for BLE devices...\n")
    # Scan for required BLE server
    device = await Scanner.find_device_by_name(OTA_DEVICE_NAME)
    if device is None:
        print("No device found. Exiting.\n")
        return

    print(f"Found device: {device}\nConnecting...\n")
    # Connect to required device
    async with Client(device) as central:
        if not central.is_connected:
            print("Connection failed.\n")
            return

        print("Connected!\n")

        # Determine MTU of the bond and subsequently chunk size
        if central._backend.__class__.__name__ == "BleakClientBlueZDBus":
            await central._backend._acquire_mtu()
        CHUNK_SIZE = central.mtu_size - 3  # subtract ATT overhead
        print(f"MTU size: {central.mtu_size}, chunk size: {CHUNK_SIZE}\n")

        # Get firmware size without consuming file
        firmware_size = os.path.getsize(FIRMWARE_PATH)
        print(f"Firmware size: {firmware_size} bytes\n")

        # Send firmware size
        await central.write_gatt_char(OTA_COMMANDS_CHR_UUID, firmware_size.to_bytes(4, "little"), response=True)
        await asyncio.sleep(1)
        # Send OTA start command (adjust to your ESP32 firmware expectations)
        ota_command = 0x2025
        await central.write_gatt_char(OTA_COMMANDS_CHR_UUID, ota_command.to_bytes(2, "big"), response=True)
        # It takes around 10 seconds for the ring buffer to get allocated, and the partitions to be fetched (do not lower this delay)
        await asyncio.sleep(10)


        total_sent = 0
        chunk_index = 0
        # Send firmware chunks
        with open(FIRMWARE_PATH, "rb") as f:
            while True:
                chunk = f.read(CHUNK_SIZE)
                if not chunk:  # EOF
                    break

                await central.write_gatt_char(OTA_FIRMWARE_CHR_UUID, chunk, response=False)
                total_sent += len(chunk)
                chunk_index += 1

                print(f"Chunk {chunk_index}: Sent {len(chunk)} bytes ({total_sent}/{firmware_size})")
                # Allow time for the data to be received and read from buffer - Do not lower this delay, as any lower delays will cause loss of packet reception
                await asyncio.sleep(0.005) 

        print("Firmware upload complete!")

if __name__ == "__main__":
    asyncio.run(main())
