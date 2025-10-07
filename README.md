| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | -------- |

# _BLE OTA Server_

This example creates **GATT server** and then starts advertising, waiting to be connected to a GATT client. It can receive firmware from a GATT client **Over-the-Air (OTA)**, and update to the new firmware.

It uses **ESP32** Bluetooth controller and **NimBLE stack** based BLE host.

To test this demo, any firmware.bin file can be used in combination with the ble_ota_sender.py script.

## Dependencies

To run the python script, the _**bleak**_ module is necessary. It is recommended to install the same in a virtual environment for project. Follow the below steps in terminal to create a virtual environment, and install the module.

```bash
cd <path_to_project_directory>
python3 -m venv <virtual_environment_name>
source <virtual_environment_name>/bin/activate
pip3 install bleak
```

## How to Use Example

Before build, be sure to set the correct chip target using:

```bash
idf.py set-target <chip_name>
```

### Configure the project

Open the project configuration menu to:

- Enable custom partition table (partitions.csv, in the project directory)
- Configure 4MB flash size
- Enable NimBLE

```bash
idf.py menuconfig
```

### Build and Flash

Run `idf.py -p PORT flash monitor` to build, flash and monitor the project.
(To exit the serial monitor, type `Ctrl-]`.)

### GATT Client end (Sending Firmware)

The above steps allow to set up a GATT server. Once, the server is running, modify the server details and target firmware binary file path in the _ble_ota_sender.py_ and run it. It will automatically connect to the server and send across the code.

**The binary output file of any project can be obtained by building the project inside the build directory as \<project-name>.bin**

## BLE Process Flow

When the server is connected to the client, a series of steps listed below has to be followed to update firmware OTA. The same is followed by this example code, as well:

- Firmware size has to be sent across to the server to the OTA service's command characteristic. It must be packaged as a 4-byte package and sent, regardless of the actual size being less than the maximum of a _uint8_t_ or a _uint16_t_.
  - This firmware size is what is used to determine the progress of the OTA update on the server end (when it receives the new firmware).
- OTA start command has to be sent as a 2 byte package, specifically `0x2025` is to be sent, to the OTA service's command characteristic.
- A ring buffer is created upon receiving the OTA initialization command, and partition details (current and update) are fetched.
- Firmware is then continuously written to the server's OTA firmware characteristic, which gets stored in the ring buffer.
- The OTA task then, fetches data from the ring buffer to store in a temporary buffer to write to the flash in chunks (4kB in this example)
  - The ring buffer serves a very important purpose in this process flow, it is an internally mutexed type of buffer whose size one can set to their requirement. So, any reads/writes to the buffer at high speeds will not add additional complexity when implementing (as end user).
  - The data once sent to the ring buffer, is fetched by the OTA task. It is of utmost importance that each ring buffer element be fetched and returned back to the ring buffer. Else, the buffer will overflow (since the fetched element is locked until the read is done and released).
  - The flash writes are done in 4kB (the size has been arbitrarily decided, in this implementation) instead of doing it as is because the flash write speed might be slower than BLE reception speed (might end up causing packet loss) and extremely frequent, repeated access to the flash for very small data sizes might cause data corruption.
- All firmware data, up to the size of the firmware is written to the flash. Additionally, any left over data is also flushed to the same before ending the OTA.
- The boot partition is then updated, and the system is restarted to boot up with the new firmware.

The outputs of the process with a sample code is presented [here](demo.mp4)

## Troubleshooting

For any technical queries, contact _**Adithya-187326**_. For issues, please open an issue.
