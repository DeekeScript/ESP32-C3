# ESP32-C3 蓝牙 Hid 烧录

DeekeScript Pro 的蓝牙 Hid 固件。只支持 **ESP32-C3**，ESP32、S2、S3 不能用这份脚本。

完整说明见 [Hid 烧录](https://script.deeke.cn/hid/bluetooth.html)。

## 准备

- [arduino-cli](https://arduino.github.io/arduino-cli/)，并装好 ESP32 核心和 NimBLE-Arduino
- [esptool](https://github.com/espressif/esptool)
- Python 3

macOS：

```bash
brew install arduino-cli esptool
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "NimBLE-Arduino"
```

Windows 用 Git Bash，并且上面三个命令能直接运行。

用 USB 接上板子。正常烧录不用按 BOOT 或 RST。只有一直停在等待下载时，才按住 BOOT、点一下 RST、再松开 BOOT。

## 烧录

```bash
git clone https://github.com/DeekeScript/ESP32-C3.git
cd ESP32-C3
```

名称是 `DeekeScript-` 加 4 位。只写数字会补零，`1` 会变成 `0001`。

```bash
./flash.sh DeekeScript-0001
./flash.sh 0001
./flash.sh
```

不写名称时，用 `DeekeScript-` 加芯片 MAC 后 4 位，例如 `DeekeScript-5E68`。

串口自动识别：macOS 找 `/dev/cu.usbmodem*`，Linux 找 `/dev/ttyACM*` 或 `/dev/ttyUSB*`，Windows 找乐鑫原生 USB 串口（`VID_303A`）。多块板同时插着，或 Windows 上是 CH340 这类串口芯片时，再指定端口：

```bash
PORT=/dev/cu.usbmodem101 ./flash.sh DeekeScript-0001
PORT=COM3 ./flash.sh DeekeScript-0001
```

烧录结束会打印 `烧录完成: DeekeScript-0001`。Flash 按 DIO 接线，不要改成 QIO。

手机蓝牙里搜索这个名称并配对。换过板子或改了名称时，取消旧名称的配对，只保留当前这块板。
