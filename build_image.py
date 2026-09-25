#!/usr/bin/env python3
import os
import struct
import sys
from pathlib import Path

SECTOR = 512
IMAGE_SECTORS = 2048
IMAGE_SIZE = SECTOR * IMAGE_SECTORS
KERNEL_LOAD_SECTORS = 508
FS_MAGIC = 0x474F4653
FS_MAX_FILES = 32
FS_NAME_LEN = 16
FS_ENTRY_SIZE = 32
FS_BLOCK_SECTORS = 16
FS_BLOCK_BYTES = SECTOR * FS_BLOCK_SECTORS
FS_LEGACY_SUPER = 200
FS_LEGACY_TABLE = 201
FS_LEGACY_DATA = 203
FS_SUPER = 512
FS_TABLE = 513
FS_DATA = 515
DIRECTORY_MARK = 0xD1


def read_entries(image, super_lba, table_lba, data_start):
    super_offset = super_lba * SECTOR
    magic, count, next_free = struct.unpack_from("<III", image, super_offset)
    if magic != FS_MAGIC:
        return None
    table_offset = table_lba * SECTOR
    raw_table = image[table_offset : table_offset + FS_MAX_FILES * FS_ENTRY_SIZE]
    if len(raw_table) != FS_MAX_FILES * FS_ENTRY_SIZE:
        raise ValueError("Die Dateitabelle ist unvollstaendig.")

    entries = []
    for index in range(FS_MAX_FILES):
        raw = raw_table[index * FS_ENTRY_SIZE : (index + 1) * FS_ENTRY_SIZE]
        name_raw = raw[:FS_NAME_LEN]
        size, start_lba = struct.unpack_from("<II", raw, FS_NAME_LEN)
        used = raw[24]
        reserved = raw[25:32]
        if not used:
            continue
        if b"\0" not in name_raw:
            raise ValueError("Dateisystem enthaelt einen ungueltigen Namen.")
        name_bytes = name_raw.split(b"\0", 1)[0]
        if not name_bytes or len(name_bytes) >= FS_NAME_LEN:
            raise ValueError("Dateisystem enthaelt einen ungueltigen Namen.")
        name = name_bytes.decode("latin1")
        parts = name.split("/")
        if any(not part or part in (".", "..") for part in parts):
            raise ValueError("Dateisystem enthaelt einen ungueltigen Pfad.")
        if any(ord(char) < 32 or char in "\\:" for char in name):
            raise ValueError("Dateisystem enthaelt ungueltige Pfadzeichen.")
        is_directory = reserved[0] == DIRECTORY_MARK
        if start_lba < data_start or start_lba + FS_BLOCK_SECTORS > IMAGE_SECTORS:
            raise ValueError("Dateisystem verweist ausserhalb des Datentraegers.")
        if (is_directory and size != 0) or (not is_directory and size > FS_BLOCK_BYTES):
            raise ValueError("Dateisystemeintrag hat eine ungueltige Groesse.")
        entries.append(
            {
                "index": index,
                "name": name,
                "name_raw": name_raw,
                "size": size,
                "start_lba": start_lba,
                "is_directory": is_directory,
                "raw": raw,
            }
        )

    names = set()
    for entry in entries:
        if entry["name"] in names:
            raise ValueError("Dateisystem enthaelt doppelte Namen.")
        names.add(entry["name"])
        parent = entry["name"].rpartition("/")[0]
        if parent and not any(
            candidate["name"] == parent and candidate["is_directory"]
            for candidate in entries
        ):
            raise ValueError("Dateisystem enthaelt ein fehlendes Elternverzeichnis.")
    for i, entry in enumerate(entries):
        for other in entries[i + 1 :]:
            if (
                entry["start_lba"] < other["start_lba"] + FS_BLOCK_SECTORS
                and entry["start_lba"] + FS_BLOCK_SECTORS > other["start_lba"]
            ):
                raise ValueError("Dateisystemeintraege belegen ueberlappende Bloecke.")
    if count != len(entries):
        raise ValueError("Dateisystemzaehler stimmt nicht mit der Tabelle ueberein.")
    return entries


def write_superblock(image, reserved, entries):
    next_free = max(
        (entry["start_lba"] + FS_BLOCK_SECTORS for entry in entries),
        default=0,
    )
    offset = FS_SUPER * SECTOR
    image[offset : offset + SECTOR] = struct.pack(
        "<III", FS_MAGIC, len(entries), next_free
    ) + reserved[: SECTOR - 12].ljust(SECTOR - 12, b"\0")


def migrate_legacy(image, source_image, entries):
    source_ranges = [
        (entry["start_lba"], entry["start_lba"] + FS_BLOCK_SECTORS)
        for entry in entries
    ]
    destinations = []
    for lba in range(FS_DATA, IMAGE_SECTORS - FS_BLOCK_SECTORS + 1, FS_BLOCK_SECTORS):
        if any(lba < end and lba + FS_BLOCK_SECTORS > start for start, end in source_ranges):
            continue
        if any(lba < end and lba + FS_BLOCK_SECTORS > start for start, end in destinations):
            continue
        destinations.append((lba, lba + FS_BLOCK_SECTORS))
        if len(destinations) == len(entries):
            break
    if len(destinations) != len(entries):
        raise ValueError("Nicht genug sicherer Platz fuer die FS-Migration.")

    table = bytearray(FS_MAX_FILES * FS_ENTRY_SIZE)
    for entry, (destination, _) in zip(entries, destinations):
        if not entry["is_directory"]:
            src = entry["start_lba"] * SECTOR
            dst = destination * SECTOR
            image[dst : dst + FS_BLOCK_BYTES] = source_image[src : src + FS_BLOCK_BYTES]
        updated = bytearray(entry["raw"])
        struct.pack_into("<I", updated, FS_NAME_LEN + 4, destination)
        start = entry["index"] * FS_ENTRY_SIZE
        table[start : start + FS_ENTRY_SIZE] = updated
        entry["start_lba"] = destination

    table_offset = FS_TABLE * SECTOR
    image[table_offset : table_offset + len(table)] = table
    legacy_reserved = source_image[
        FS_LEGACY_SUPER * SECTOR + 12 : (FS_LEGACY_SUPER + 1) * SECTOR
    ]
    write_superblock(image, legacy_reserved, entries)
    print(f"Altes Dateisystem sicher nach LBA {FS_SUPER} migriert ({len(entries)} Eintraege).")


def main():
    if len(sys.argv) != 5:
        raise SystemExit("Usage: build_image.py boot.bin stage2.bin kernel.bin os.img")
    boot_path, stage_path, kernel_path, output_path = map(Path, sys.argv[1:])
    boot = boot_path.read_bytes()
    stage = stage_path.read_bytes()
    kernel = kernel_path.read_bytes()
    if len(boot) != SECTOR:
        raise ValueError(f"Bootsektor muss {SECTOR} Bytes gross sein.")
    if len(stage) != 2 * SECTOR:
        raise ValueError(f"Stage2 muss {2 * SECTOR} Bytes gross sein.")
    if len(kernel) > KERNEL_LOAD_SECTORS * SECTOR:
        raise ValueError("Kernel ist groesser als das Bootloader-Ladefenster (508 Sektoren).")

    old_image = output_path.read_bytes() if output_path.exists() else b""
    image = bytearray(IMAGE_SIZE)
    image[:SECTOR] = boot
    image[SECTOR : 3 * SECTOR] = stage
    image[3 * SECTOR : 3 * SECTOR + len(kernel)] = kernel

    if len(old_image) >= IMAGE_SIZE:
        old_image = old_image[:IMAGE_SIZE]
        entries = read_entries(old_image, FS_SUPER, FS_TABLE, FS_DATA)
        if entries is not None:
            image[FS_SUPER * SECTOR :] = old_image[FS_SUPER * SECTOR :]
            print(f"Dateisystem aus LBA {FS_SUPER} erhalten ({len(entries)} Eintraege).")
        else:
            entries = read_entries(
                old_image, FS_LEGACY_SUPER, FS_LEGACY_TABLE, FS_LEGACY_DATA
            )
            if entries is not None:
                migrate_legacy(image, old_image, entries)
            elif (
                struct.unpack_from("<I", old_image, FS_SUPER * SECTOR)[0] == FS_MAGIC
                or struct.unpack_from("<I", old_image, FS_LEGACY_SUPER * SECTOR)[0] == FS_MAGIC
            ):
                raise ValueError("Vorhandenes Dateisystem ist ungueltig; Image bleibt unveraendert.")

    temp_path = output_path.with_name(output_path.name + ".tmp")
    try:
        temp_path.write_bytes(image)
        os.replace(temp_path, output_path)
    finally:
        if temp_path.exists():
            temp_path.unlink()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"Image-Build abgebrochen: {error}", file=sys.stderr)
        raise SystemExit(1)
