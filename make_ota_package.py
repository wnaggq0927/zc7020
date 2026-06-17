#!/usr/bin/env python3
"""Create a minimal protected OTA package for the OTDR board."""

import argparse
import binascii
import datetime as _datetime
import pathlib
import struct
import sys


MAGIC = 0x5041544F
VERSION = 2
TARGET_ID = 0x4F544452
OTA_KEY = 0xA5C35A96
HEADER_FMT = "<IIIIIIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)


def key_byte(index: int, target_id: int) -> int:
    x = (OTA_KEY ^ target_id ^ ((index * 1103515245 + 12345) & 0xFFFFFFFF)) & 0xFFFFFFFF
    x ^= x >> 16
    x ^= x >> 8
    return x & 0xFF


def encrypt(data: bytes, target_id: int) -> bytes:
    out = bytearray(len(data))
    for i, value in enumerate(data):
        out[i] = value ^ key_byte(i, target_id)
    return bytes(out)


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def default_firmware_version() -> int:
    return int(_datetime.datetime.now().strftime("%Y%m%d%H"))


def build_package(boot_bin: bytes, target_id: int, firmware_version: int,
                  package_format: int) -> bytes:
    if not 0 <= firmware_version <= 0xFFFFFFFF:
        raise ValueError("firmware_version must fit uint32")
    if package_format not in (1, 2):
        raise ValueError("package_format must be 1 or 2")

    encrypted = encrypt(boot_bin, target_id)
    plain_crc = crc32(boot_bin)
    metadata_word = firmware_version if package_format >= 2 else 0

    header_without_crc = struct.pack(
        HEADER_FMT,
        MAGIC,
        package_format,
        target_id,
        len(boot_bin),
        len(encrypted),
        plain_crc,
        0,
        metadata_word,
    )
    header_crc = crc32(header_without_crc)
    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        package_format,
        target_id,
        len(boot_bin),
        len(encrypted),
        plain_crc,
        header_crc,
        metadata_word,
    )
    return header + encrypted


def main() -> None:
    parser = argparse.ArgumentParser(description="Pack BOOT.bin into an OTDR OTA .pkg file.")
    parser.add_argument("input", type=pathlib.Path, nargs="?", help="Input BOOT.bin path")
    parser.add_argument("-o", "--output", type=pathlib.Path, help="Output .pkg path")
    parser.add_argument("--target-id", type=lambda s: int(s, 0), default=TARGET_ID,
                        help="Target ID, default 0x4F544452")
    parser.add_argument("--fw-version", type=lambda s: int(s, 0), default=None,
                        help="Firmware version integer, default YYYYMMDDHH")
    parser.add_argument("--pkg-format", type=int, choices=(1, 2), default=2,
                        help="OTA package format. Use 1 for boards that do not support v2 yet.")
    args = parser.parse_args()

    input_path = args.input
    output = args.output
    firmware_version = args.fw_version

    if input_path is None:
        input_path, output, firmware_version = select_paths_with_ui(output, firmware_version)
        if input_path is None:
            print("Canceled.")
            return
    elif firmware_version is None:
        firmware_version = default_firmware_version()

    boot_bin = input_path.read_bytes()
    if output is None:
        output = input_path.with_suffix(input_path.suffix + ".pkg")

    package = build_package(boot_bin, args.target_id, firmware_version, args.pkg_format)
    output.write_bytes(package)

    print(f"input:  {input_path}")
    print(f"output: {output}")
    print(f"plain_size={len(boot_bin)} encrypted_size={len(package) - HEADER_SIZE}")
    print(f"plain_crc32=0x{crc32(boot_bin):08X}")
    print(f"target_id=0x{args.target_id:08X}")
    print(f"package_format={args.pkg_format}")
    print(f"firmware_version={firmware_version}")


def select_paths_with_ui(output_arg: pathlib.Path | None,
                         firmware_version_arg: int | None
                         ) -> tuple[pathlib.Path | None, pathlib.Path | None, int | None]:
    try:
        import tkinter as tk
        from tkinter import filedialog, simpledialog
    except Exception as exc:
        print(f"GUI is unavailable: {exc}", file=sys.stderr)
        return None, None, None

    root = tk.Tk()
    root.withdraw()

    input_name = filedialog.askopenfilename(
        title="Select BOOT.BIN",
        filetypes=[("BOOT image", "*.bin *.BIN"), ("All files", "*.*")],
    )
    if not input_name:
        root.destroy()
        return None, None, None

    input_path = pathlib.Path(input_name)
    output = output_arg
    if output is None:
        default_name = input_path.name + ".pkg"
        output_name = filedialog.asksaveasfilename(
            title="Save OTA package",
            initialdir=str(input_path.parent),
            initialfile=default_name,
            defaultextension=".pkg",
            filetypes=[("OTA package", "*.pkg"), ("All files", "*.*")],
        )
        if not output_name:
            root.destroy()
            return None, None, None
        output = pathlib.Path(output_name)

    firmware_version = firmware_version_arg
    if firmware_version is None:
        default_version = str(default_firmware_version())
        version_text = simpledialog.askstring(
            "Firmware version",
            "Input firmware version integer:",
            initialvalue=default_version,
            parent=root,
        )
        if not version_text:
            root.destroy()
            return None, None, None
        firmware_version = int(version_text, 0)

    root.destroy()
    return input_path, output, firmware_version


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        try:
            import tkinter as tk
            from tkinter import messagebox

            root = tk.Tk()
            root.withdraw()
            messagebox.showerror("OTA package failed", str(exc))
            root.destroy()
        except Exception:
            pass
        raise
