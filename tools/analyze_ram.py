"""Read the linked ESP32 ELF and model history capacity without changing firmware.

Run with PlatformIO's Python (includes pyelftools):
  C:/Users/Pedro/.platformio/penv/Scripts/python.exe tools/analyze_ram.py

This is a static capacity analysis, NOT a runtime free-heap measurement.
The region model is for the classic ESP32 / ESP-IDF 5.5 layout in this project.
"""

import argparse
import hashlib
import math
import re
import struct
from pathlib import Path

from elftools.elf.elffile import ELFFile


def read_at(elf, address, size):
    for section in elf.iter_sections():
        start = section["sh_addr"]
        if start <= address and address + size <= start + section["sh_size"]:
            offset = address - start
            return section.data()[offset:offset + size]
    raise ValueError(f"No section contains {address:#x} + {size}")


def available_regions(regions, reservations):
    pieces = []
    for start, size, kind, _alias, startup in regions:
        # ESP32 memory_layout.c: 0 = DRAM, 1 = D/IRAM. Both support 8-bit malloc.
        if kind not in (0, 1):
            continue
        ranges = [(start, start + size)]
        for low, high in reservations:
            if high <= low:
                continue
            remainder = []
            for begin, end in ranges:
                if low >= end or high <= begin:
                    remainder.append((begin, end))
                else:
                    if begin < low:
                        remainder.append((begin, low))
                    if high < end:
                        remainder.append((high, end))
            ranges = remainder
        pieces.extend((begin, end, kind, startup) for begin, end in ranges)
    merged = []
    for begin, end, kind, startup in sorted(pieces):
        if merged and merged[-1][1] == begin and merged[-1][2:] == (kind, startup):
            merged[-1] = (merged[-1][0], end, kind, startup)
        else:
            merged.append((begin, end, kind, startup))
    return merged


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", nargs="?", type=Path,
                        default=Path(".pio/build/esp32dev/firmware.elf"))
    args = parser.parse_args()
    with args.elf.open("rb") as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s for s in elf.get_section_by_name(".symtab").iter_symbols()}

        def value(name):
            return symbols[name]["st_value"]

        def data(name):
            symbol = symbols[name]
            return read_at(elf, symbol["st_value"], symbol["st_size"])

        if elf["e_machine"] != "EM_XTENSA" or elf.elfclass != 32:
            raise ValueError("Expected a 32-bit Xtensa ESP32 ELF")
        regions = list(struct.iter_unpack("<IIIII", data("soc_memory_regions")))
        assert len(regions) == struct.unpack("<I", data("soc_memory_region_count"))[0]
        reservations = {
            name: struct.unpack("<II", data(name))
            for name in symbols if name.startswith("reserved_region_")
        }
        static_bytes = sum(elf.get_section_by_name(name)["sh_size"]
                           for name in (".dram0.data", ".dram0.bss"))
        iram_bytes = value("_iram_end") - value("_iram_start")
        history_bytes = symbols["history"]["st_size"]
        # DWARF gives the struct/array element size, avoiding an assumed ABI size.
        sample_sizes = []
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                if (die.tag == "DW_TAG_typedef" and
                        die.attributes.get("DW_AT_name") and
                        die.attributes["DW_AT_name"].value == b"history_sample_t"):
                    sample_sizes.append(die.get_DIE_from_attribute("DW_AT_type")
                                        .attributes["DW_AT_byte_size"].value)
        assert sample_sizes and len(set(sample_sizes)) == 1
        sample_bytes = sample_sizes[0]
        original = available_regions(regions, reservations.values())
        dram_start, heap_start = reservations["reserved_region_dram_data"]
        # Match memory.ld in this build; this is a link limit, not total DRAM.
        map_path = args.elf.with_name("esp32-sht30.map")
        match = re.search(r"^dram0_0_seg\s+0x[0-9a-f]+\s+0x([0-9a-f]+)",
                          map_path.read_text(encoding="utf-8"), re.MULTILINE)
        if not match:
            raise ValueError("Missing DRAM linker region in map file")
        link_limit = int(match[1], 16)
        print("ESP32 temperature/humidity monitor: static RAM investigation")
        print(f"ELF: {args.elf}")
        print(f"ELF SHA256: {hashlib.sha256(args.elf.read_bytes()).hexdigest()}")
        print("Runtime heap has NOT been measured. All sizes below are bytes or KiB (1024 bytes).")
        print(f"Static DRAM: {static_bytes:,} bytes ({static_bytes / 1024:.2f} KiB)")
        print(f"IRAM code/vectors/alignment: {iram_bytes:,} bytes ({iram_bytes / 1024:.2f} KiB)")
        print(f"History record: {sample_bytes} bytes; history array: {history_bytes:,} bytes")
        print(f"Other static DRAM: {static_bytes - history_bytes:,} bytes")
        print(f"Static DRAM linker region: {link_limit:,} bytes")
        print("\nEligible 8-bit heap regions BEFORE allocator metadata, startup allocations,")
        print("task stacks, Wi-Fi, TCP/IP, HTTP, ADC and other runtime allocations:")
        for start, end, kind, startup in original:
            print(f"  {start:#010x}..{end:#010x}: {end-start:7,d} bytes"
                  f" ({'D/IRAM' if kind else 'DRAM'}, startup_stack={bool(startup)})")
        print(f"  Total theoretical region space: {sum(b-a for a,b,_,_ in original):,} bytes")
        print("\n24-hour retention with existing 12-byte records and full history snapshot:")
        print("Interval  Points  Array KiB  Array+copy KiB  Static KiB  Region test")
        for seconds in (60, 30, 20, 16, 15, 10, 9, 8, 5, 2):
            count = math.ceil(86400 / seconds)
            size = count * sample_bytes
            delta = size - history_bytes
            candidate = dict(reservations)
            # Existing data is 8-byte aligned; account for the resized .bss tail.
            candidate_end = (heap_start + delta + 7) & ~7
            candidate["reserved_region_dram_data"] = (dram_start, candidate_end)
            free_regions = available_regions(regions, candidate.values())
            largest = max((b-a for a,b,_,_ in free_regions), default=0)
            if candidate_end - dram_start > link_limit:
                status = "FAIL: static DRAM link limit"
            elif size > largest:
                status = "FAIL: snapshot cannot fit any region"
            else:
                status = "Fits geometry ONLY; needs runtime measurement"
            print(f"{seconds:6}s {count:7} {size/1024:10.2f} {2*size/1024:15.2f}"
                  f" {(static_bytes+delta)/1024:11.2f}  {status}")
        print("\nLimits: region sizes are upper bounds, not free heap. Passing this table")
        print("does not establish safety. Wi-Fi traffic, sockets and fragmentation matter.")
        print("Rows assume true requested sample spacing; the current ~2-second sensor")
        print("loop rounds odd intervals up in practice, and execution adds some drift.")


if __name__ == "__main__":
    main()
