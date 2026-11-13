#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
BMS 上位机：解析 0x5A 0xA5 二进制帧，打印/记录 CSV，可下发命令。

用法：
  python bms_monitor.py --port COM7                 # 实时表格
  python bms_monitor.py --port COM7 --csv log.csv   # 同时存 CSV
  python bms_monitor.py --port COM7 --text          # 切换成文本模式
  python bms_monitor.py --port COM7 --clear         # 清故障锁存
  python bms_monitor.py --port COM7 --fets 3        # 手动开 CHG+DSG
  python bms_monitor.py --demo                      # 无硬件，看输出格式

依赖：pip install pyserial
"""
import argparse
import struct
import sys
import time

SOF1, SOF2 = 0x5A, 0xA5
CMD_TELEM, CMD_FETS, CMD_CLEAR, CMD_BAL, CMD_VER, CMD_TEXT = 1, 2, 3, 4, 5, 6

FAULTS = [
    (0x0001, "OV"), (0x0002, "UV"), (0x0004, "OCD"), (0x0008, "SCD"),
    (0x0010, "CELL_HOT"), (0x0020, "CELL_COLD"), (0x0040, "MOS_HOT"),
    (0x0080, "COMM"), (0x0100, "AFE"), (0x0200, "BAL_HOT"),
]


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    body = bytes([cmd, len(payload)]) + payload
    c = crc16(body)
    return bytes([SOF1, SOF2]) + body + bytes([c >> 8, c & 0xFF])


def decode_telemetry(p: bytes):
    """解析 0x81 的 payload"""
    if len(p) < 24:
        return None
    ver, cells = p[0], p[1]
    cell = list(struct.unpack_from("<5H", p, 2))
    pack, cur, temp, soc, rem, flt, fet, bal = struct.unpack_from("<HhhBHIBB", p, 12)
    return dict(ver=ver, cells=cells, cell=cell[:cells], pack=pack,
                current=cur, temp=temp / 10.0, soc=soc, remain=rem,
                faults=flt, fet=fet, bal=bal)


def fault_str(flt: int) -> str:
    act = flt & 0xFFFF
    lat = (flt >> 16) & 0xFFFF
    names = [n for bit, n in FAULTS if act & bit]
    if not names:
        names = ["-"]
    s = ",".join(names)
    if lat:
        s += f" (latched:0x{lat:04X})"
    return s


class Reader:
    def __init__(self, port=None, baud=115200):
        self.buf = bytearray()
        self.ser = None
        if port:
            import serial  # noqa
            self.ser = serial.Serial(port, baud, timeout=0.1)

    def feed(self, data: bytes):
        self.buf.extend(data)

    def frames(self):
        while True:
            i = self.buf.find(bytes([SOF1, SOF2]))
            if i < 0:
                if len(self.buf) > 256:
                    del self.buf[:-1]
                return
            if len(self.buf) < i + 4:
                return
            cmd, ln = self.buf[i + 2], self.buf[i + 3]
            if len(self.buf) < i + 6 + ln:
                return
            payload = bytes(self.buf[i + 4:i + 4 + ln])
            crc_rx = (self.buf[i + 4 + ln] << 8) | self.buf[i + 5 + ln]
            ok = crc16(bytes([cmd, ln]) + payload) == crc_rx
            del self.buf[:i + 6 + ln]
            if ok:
                yield cmd, payload

    def read(self):
        return self.ser.read(64) if self.ser else b""

    def send(self, cmd, payload=b""):
        if self.ser:
            self.ser.write(build_frame(cmd, payload))


def print_row(t):
    cells = " ".join(f"{v:4d}" for v in t["cell"])
    print(f"{time.strftime('%H:%M:%S')} | {cells} mV | {t['pack']:5d} mV | "
          f"{t['current']:+6d} mA | {t['temp']:5.1f}C | SOC {t['soc']:3d}% | "
          f"{t['remain']:4d} mAh | FET {t['fet']:02X} BAL {t['bal']:02X} | "
          f"{fault_str(t['faults'])}")


def demo():
    """没有硬件时，合成几帧看看输出"""
    print("== DEMO 模式（假数据）==")
    for k in range(5):
        cells = [4050 - k * 5, 4060 - k * 5, 4040 - k * 5]
        payload = struct.pack("<BB5H", 1, 3, *cells, 0, 0)
        payload += struct.pack("<HhhBHIbb", sum(cells), -1200 - k * 10, 300,
                               85 - k, 2900, 0x0000, 0x03, 0x00)
        t = decode_telemetry(payload)
        print_row(t)
        time.sleep(0.15)


def main():
    ap = argparse.ArgumentParser(description="BMS 上位机")
    ap.add_argument("--port", help="串口，如 COM7 或 /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--csv", help="把遥测写入 CSV")
    ap.add_argument("--text", action="store_true", help="运行时切换文本模式")
    ap.add_argument("--clear", action="store_true", help="清除故障锁存")
    ap.add_argument("--fets", type=lambda x: int(x, 0), help="0x01=CHG 0x02=DSG，如 3")
    ap.add_argument("--bal", type=lambda x: int(x, 0), help="均衡掩码测试，如 0x01")
    ap.add_argument("--version", action="store_true", help="读固件版本")
    ap.add_argument("--demo", action="store_true", help="无硬件演示")
    args = ap.parse_args()

    if args.demo:
        demo()
        return

    if not args.port:
        ap.error("需要 --port，或加 --demo")

    r = Reader(args.port, args.baud)
    csv_f = open(args.csv, "w", encoding="utf-8") if args.csv else None
    if csv_f:
        csv_f.write("time,cell1,cell2,cell3,pack_mv,current_ma,temp_c,soc,remain_mah,faults,fet,bal\n")

    if args.text:
        r.send(CMD_TEXT, b"\x01")
    if args.clear:
        r.send(CMD_CLEAR)
    if args.fets is not None:
        r.send(CMD_FETS, bytes([args.fets]))
    if args.bal is not None:
        r.send(CMD_BAL, bytes([args.bal]))
    if args.version:
        r.send(CMD_VER)

    print("监听中，Ctrl+C 退出 …")
    try:
        while True:
            data = r.read()
            if data:
                r.feed(data)

            for cmd, payload in r.frames():
                if cmd == CMD_TELEM | 0x80:
                    t = decode_telemetry(payload)
                    if t:
                        print_row(t)
                        if csv_f:
                            csv_f.write(",".join([
                                time.strftime("%H:%M:%S"),
                                *[str(v) for v in t["cell"]],
                                str(t["pack"]), str(t["current"]),
                                f"{t['temp']:.1f}", str(t["soc"]),
                                str(t["remain"]), f"0x{t['faults']:08X}",
                                f"0x{t['fet']:02X}", f"0x{t['bal']:02X}"]) + "\n")
                            csv_f.flush()
                elif cmd == CMD_VER | 0x80:
                    print("版本:", payload.decode(errors="replace"))
                else:
                    print(f"回包 cmd=0x{cmd:02X} data={payload.hex()}")

    except KeyboardInterrupt:
        print("\n退出")
    finally:
        if csv_f:
            csv_f.close()


if __name__ == "__main__":
    main()
