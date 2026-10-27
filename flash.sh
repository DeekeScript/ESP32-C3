#!/usr/bin/env bash
# 一键编译并烧录 DeekeScript 蓝牙 Hid 固件。
#
#   ./flash.sh
#       名称用 DeekeScript- 加芯片 MAC 后 4 位，例如 DeekeScript-5E68
#   ./flash.sh DeekeScript-0001
#   ./flash.sh 0001
#   串口自动识别：macOS 找 /dev/cu.usbmodem*，Linux 找 /dev/ttyACM*，
#   Windows（Git Bash）找 VID 303A 的 COM 口。多块板同时插着时再指定：
#   PORT=/dev/cu.usbmodem101 ./flash.sh DeekeScript-0001
#   PORT=COM3 ./flash.sh DeekeScript-0001
#
# 需要已安装 arduino-cli（含 esp32 核心）、esptool，板子插在 USB 上。

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$ROOT/deeke_hid"
BUILD="$ROOT/build"
STAGE="$ROOT/staging"
# 这块板的 Flash 只按 DIO 接线。QIO 引导能启动，但读分区表会得到错误魔数。
FQBN="${FQBN:-esp32:esp32:esp32c3:CDCOnBoot=cdc,UploadSpeed=921600,FlashSize=4M,FlashMode=dio}"
PORT="${PORT:-}"
NAME=""

usage() {
  sed -n '2,14p' "$0"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    -p|--port) PORT="${2:-}"; shift 2 ;;
    -n|--name) NAME="${2:-}"; shift 2 ;;
    *)
      if [[ -n "$NAME" ]]; then
        echo "多余参数: $1" >&2
        exit 1
      fi
      NAME="$1"
      shift
      ;;
  esac
done

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "未找到 arduino-cli" >&2
  exit 1
fi
if ! command -v esptool >/dev/null 2>&1; then
  echo "未找到 esptool" >&2
  exit 1
fi

if [[ -z "$PORT" ]]; then
  case "$(uname -s)" in
    Darwin)
      PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -n 1 || true)"
      ;;
    MINGW*|MSYS*|CYGWIN*)
      win_ports="$(powershell.exe -NoProfile -Command '
        Get-CimInstance Win32_PnPEntity |
          Where-Object { $_.PNPDeviceID -match "VID_303A" -and $_.Name -match "\(COM\d+\)" } |
          ForEach-Object {
            if ($_.Name -match "\((COM\d+)\)") { $Matches[1] }
          }
      ' 2>/dev/null | tr -d "\r" | sed "/^[[:space:]]*$/d" || true)"
      win_count="$(printf '%s\n' "$win_ports" | grep -c . || true)"
      if [[ "$win_count" -gt 1 ]]; then
        echo "找到多个 Espressif 串口，请指定其中一个，例如 PORT=COM3" >&2
        printf '%s\n' "$win_ports" >&2
        exit 1
      fi
      PORT="$(printf '%s\n' "$win_ports" | head -n 1)"
      ;;
    Linux)
      PORT="$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -n 1 || true)"
      ;;
  esac
fi
if [[ -z "$PORT" ]]; then
  echo "没有找到 USB 串口。把板子插上，或设置 PORT（macOS 如 /dev/cu.usbmodem101，Windows 如 COM3）" >&2
  exit 1
fi
if [[ ! "$PORT" =~ ^[Cc][Oo][Mm][0-9]+$ && ! -e "$PORT" ]]; then
  echo "串口不存在: $PORT" >&2
  exit 1
fi

chip_text="$(esptool --port "$PORT" chip-id 2>&1 || true)"
echo "$chip_text" | sed -n '1,20p'
if ! echo "$chip_text" | grep -q "ESP32-C3"; then
  echo "当前固件只支持 ESP32-C3，识别结果里没有 ESP32-C3" >&2
  exit 1
fi

if [[ -z "$NAME" ]]; then
  mac="$(echo "$chip_text" | awk -F': ' '/^MAC:/{print $2; exit}' | tr -d ':' | tr '[:lower:]' '[:upper:]')"
  if [[ ${#mac} -lt 4 ]]; then
    echo "没有读到 MAC，无法生成默认名称" >&2
    exit 1
  fi
  NAME="DeekeScript-${mac: -4}"
else
  suffix="$NAME"
  if [[ "$suffix" == DeekeScript-* ]]; then
    suffix="${suffix#DeekeScript-}"
  elif [[ "$suffix" == *-* ]]; then
    echo "名称写成 DeekeScript- 加 4 位，例如 DeekeScript-0001" >&2
    exit 1
  fi
  if [[ "$suffix" =~ ^[0-9]+$ ]]; then
    if [[ ${#suffix} -gt 4 ]]; then
      echo "编号只保留 4 位，例如 0001" >&2
      exit 1
    fi
    suffix="$(printf '%04d' "$((10#$suffix))")"
  fi
  if [[ ! "$suffix" =~ ^[A-Za-z0-9]{4}$ ]]; then
    echo "横线后面只保留 4 位字母或数字，例如 DeekeScript-0001，当前是: $suffix" >&2
    exit 1
  fi
  NAME="DeekeScript-$suffix"
fi

echo "烧录名称: $NAME"
echo "串口: $PORT"

if [[ ! -f "$BUILD/build.options.json" ]]; then
  rm -rf "$BUILD"
fi
mkdir -p "$BUILD" "$STAGE"
echo "编译固件..."
arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" "$SKETCH"

APP_BIN="$(find "$BUILD" -name 'deeke_hid.ino.bin' -o -name '*.ino.bin' | head -n 1)"
if [[ -z "$APP_BIN" ]]; then
  echo "编译结果里没有应用固件" >&2
  exit 1
fi

PATCHED="$STAGE/app.bin"
cp "$APP_BIN" "$PATCHED"
python3 - "$PATCHED" "$NAME" <<'PY'
import hashlib
import struct
import sys

path, name = sys.argv[1], sys.argv[2]
data = bytearray(open(path, "rb").read())
magic = b"DKNM"
idx = data.find(magic)
if idx < 0:
    sys.exit("固件里没有找到名称槽 DKNM")
slot = idx + 4
if slot + 20 > len(data):
    sys.exit("名称槽超出固件长度")
raw = name.encode("ascii")
if len(raw) > 20:
    sys.exit("名称超过 20 字节")
data[slot:slot + 20] = raw + bytes(20 - len(raw))

if data[0] != 0xE9:
    sys.exit("应用固件不是 ESP 镜像")
segments = data[1]
append_digest = data[23]
off = 24
checksum = 0xEF
for _ in range(segments):
    _, length = struct.unpack_from("<II", data, off)
    for byte in data[off + 8:off + 8 + length]:
        checksum ^= byte
    off += 8 + length
# 校验和放在 16 字节块的最后一字节，后面是 32 字节 SHA256
off += (15 - (off % 16)) % 16
data[off] = checksum
end = off + 1
if append_digest == 1:
    data[end:end + 32] = hashlib.sha256(data[:end]).digest()
open(path, "wb").write(data)
print(f"已写入名称槽: {name}")
PY

FLASH_ARGS="$(find "$BUILD" -name flash_args | head -n 1)"
if [[ -z "$FLASH_ARGS" ]]; then
  echo "没有找到 flash_args" >&2
  exit 1
fi

args=()
files=()
while IFS= read -r line || [[ -n "$line" ]]; do
  [[ -z "$line" ]] && continue
  if [[ "$line" == --* ]]; then
    # esptool 5 使用连字符参数
    line="${line//_/-}"
    # shellcheck disable=SC2206
    args+=($line)
    continue
  fi
  set -- $line
  off="$1"
  file="$2"
  base="$(basename "$file")"
  dir="$(dirname "$FLASH_ARGS")"
  if [[ "$base" == *.ino.bin ]]; then
    files+=("$off" "$PATCHED")
  elif [[ -f "$dir/$file" ]]; then
    files+=("$off" "$dir/$file")
  elif [[ -f "$file" ]]; then
    files+=("$off" "$file")
  else
    echo "缺少烧录文件: $file" >&2
    exit 1
  fi
done < "$FLASH_ARGS"

echo "开始烧录..."
esptool --chip esp32c3 --port "$PORT" --baud 921600 write-flash "${args[@]}" "${files[@]}"
echo "烧录完成: $NAME"
echo "手机蓝牙里搜索并配对这个名称后，脚本里调用 Hid.connect(true, 0)。"
