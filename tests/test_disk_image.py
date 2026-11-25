#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

"""Check the disk images written by "qdl --disk-image".

A small GPT layout is described the way ptool does it: partition table
binaries built for a disk of unknown size, plus the patches that fix them up
once the size is known. The resulting image must be a valid GPT disk holding
the programmed data.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

SECTOR = 512
ENTRY_SIZE = 128
NUM_ENTRIES = 128
ARRAY_SECTORS = NUM_ENTRIES * ENTRY_SIZE // SECTOR
ARRAY_BYTES = ARRAY_SECTORS * SECTOR
HEADER_SIZE = 92

BOOT_START = 64
BOOT_SECTORS = 64
ROOTFS_START = 128
ROOTFS_BLOCKS = 8
SPARSE_BLOCK = 4096


def run_qdl(qdl, *args, cwd=None, expect_success=True):
    # A run that goes looking for a device would wait for one forever
    try:
        result = subprocess.run([qdl, *args], cwd=cwd, capture_output=True,
                                text=True, timeout=60)
    except subprocess.TimeoutExpired as e:
        raise AssertionError(f"qdl {' '.join(args)} timed out") from e

    if expect_success and result.returncode:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        raise AssertionError("qdl command failed")

    if not expect_success and not result.returncode:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        raise AssertionError("qdl command succeeded unexpectedly")

    return result


def gpt_entry(name, first_lba, last_lba, seed):
    return struct.pack("<16s16sQQQ72s", bytes([seed]) * 16,
                       bytes([seed + 1]) * 16, first_lba, last_lba, 0,
                       name.encode("utf-16-le"))


def gpt_header(my_lba, array_lba):
    # Everything depending on the disk size is left zero for the patches
    return struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000,
                       HEADER_SIZE, 0, 0, my_lba, 0, 34, 0, b"\x42" * 16,
                       array_lba, NUM_ENTRIES, ENTRY_SIZE, 0).ljust(SECTOR, b"\0")


def sparse_image(raw, fill, fill_blocks, skip_blocks):
    """Android sparse image: raw data, a fill pattern, then a hole"""
    raw_blocks = len(raw) // SPARSE_BLOCK
    total = raw_blocks + fill_blocks + skip_blocks

    image = struct.pack("<IHHHHIIII", 0xed26ff3a, 1, 0, 28, 12, SPARSE_BLOCK,
                        total, 3, 0)
    image += struct.pack("<HHII", 0xcac1, 0, raw_blocks, 12 + len(raw)) + raw
    image += struct.pack("<HHII", 0xcac2, 0, fill_blocks, 16) + fill
    image += struct.pack("<HHII", 0xcac3, 0, skip_blocks, 12)

    return image


def program(start, sectors, filename, label, partition=0, sparse=False):
    return (f'  <program SECTOR_SIZE_IN_BYTES="{SECTOR}" file_sector_offset="0"'
            f' filename="{filename}" label="{label}"'
            f' num_partition_sectors="{sectors}"'
            f' physical_partition_number="{partition}"'
            f' sparse="{"true" if sparse else "false"}"'
            f' start_sector="{start}"/>\n')


def patch(start, offset, size, value, filename="DISK"):
    return (f'  <patch SECTOR_SIZE_IN_BYTES="{SECTOR}" byte_offset="{offset}"'
            f' filename="{filename}" physical_partition_number="0"'
            f' size_in_bytes="{size}" start_sector="{start}" value="{value}"'
            f' what="test"/>\n')


def write_build(work):
    """Create the build and return the content expected on the disk"""
    work.mkdir()

    boot = (bytes(range(256)) * (BOOT_SECTORS * SECTOR // 256))[:-4] + b"boot"
    raw = os.urandom(2 * SPARSE_BLOCK)
    fill = b"\xde\xad\xbe\xef"
    rootfs = raw + fill * (SPARSE_BLOCK // 4)

    array = (gpt_entry("boot", BOOT_START, BOOT_START + BOOT_SECTORS - 1, 1) +
             gpt_entry("rootfs", ROOTFS_START, 0, 3)).ljust(ARRAY_BYTES, b"\0")

    (work / "prog.elf").write_bytes(b"\x7fELF" + bytes(60))
    (work / "boot.img").write_bytes(boot)
    (work / "rootfs.img").write_bytes(sparse_image(raw, fill, 1, ROOTFS_BLOCKS - 3))
    (work / "gpt_main0.bin").write_bytes(bytes(SECTOR) + gpt_header(1, 2) + array)
    (work / "gpt_backup0.bin").write_bytes(array + gpt_header(0, 0))

    (work / "rawprogram0.xml").write_text(
        '<?xml version="1.0" ?>\n<data>\n' +
        program(BOOT_START, BOOT_SECTORS, "boot.img", "boot") +
        program(ROOTFS_START, ROOTFS_BLOCKS * SPARSE_BLOCK // SECTOR,
                "rootfs.img", "rootfs", sparse=True) +
        program(0, 34, "gpt_main0.bin", "PrimaryGPT") +
        program("NUM_DISK_SECTORS-33.", 33, "gpt_backup0.bin", "BackupGPT") +
        '</data>\n')

    backup = "NUM_DISK_SECTORS-1."
    (work / "patch0.xml").write_text(
        '<?xml version="1.0" ?>\n<patches>\n' +
        # Only the DISK patches apply, this one must be left out
        patch(0, 0, 4, "1234", filename="gpt_main0.bin") +
        # Grow the last partition to the end of the disk
        patch(2, ENTRY_SIZE + 40, 8, "NUM_DISK_SECTORS-34.") +
        patch("NUM_DISK_SECTORS-33.", ENTRY_SIZE + 40, 8, "NUM_DISK_SECTORS-34.") +
        # Last usable LBA
        patch(1, 48, 8, "NUM_DISK_SECTORS-34.") +
        patch(backup, 48, 8, "NUM_DISK_SECTORS-34.") +
        # Location of the other header, and of the backup one itself
        patch(1, 32, 8, "NUM_DISK_SECTORS-1.") +
        patch(backup, 24, 8, "NUM_DISK_SECTORS-1.") +
        patch(backup, 32, 8, "1") +
        patch("NUM_DISK_SECTORS-1", 72, 8, "NUM_DISK_SECTORS-33.") +
        # Checksums: partition array first, then the header over it
        patch(1, 88, 4, f"CRC32(2,{ARRAY_BYTES})") +
        patch(backup, 88, 4, f"CRC32(NUM_DISK_SECTORS-33.,{ARRAY_BYTES})") +
        patch(1, 16, 4, "0") +
        patch(1, 16, 4, f"CRC32(1,{HEADER_SIZE})") +
        patch(backup, 16, 4, "0") +
        patch(backup, 16, 4, f"CRC32(NUM_DISK_SECTORS-1.,{HEADER_SIZE})") +
        '</patches>\n')

    (work / "rawprogram1.xml").write_text(
        '<?xml version="1.0" ?>\n<data>\n' +
        program(0, BOOT_SECTORS, "boot.img", "xbl", partition=1) +
        '</data>\n')

    return {"boot": boot, "rootfs": rootfs}


def check_header(disk, lba, other_lba, array_lba, num_sectors):
    """Validate one GPT header and return the partition array it points to"""
    header = disk[lba * SECTOR:lba * SECTOR + HEADER_SIZE]
    (signature, _, size, crc, _, my_lba, alternate_lba, first_usable,
     last_usable, _, entries_lba, num_entries, entry_size,
     array_crc) = struct.unpack("<8sIIIIQQQQ16sQIII", header)

    assert signature == b"EFI PART", "bad GPT signature"
    assert size == HEADER_SIZE
    assert my_lba == lba, f"header at {lba} claims to be at {my_lba}"
    assert alternate_lba == other_lba, "wrong location of the other header"
    assert first_usable == 34
    assert last_usable == num_sectors - 34, "wrong last usable LBA"
    assert entries_lba == array_lba, "wrong partition array location"
    assert (num_entries, entry_size) == (NUM_ENTRIES, ENTRY_SIZE)

    zeroed = header[:16] + bytes(4) + header[20:]
    assert crc == zlib.crc32(zeroed), "bad GPT header checksum"

    array = disk[array_lba * SECTOR:array_lba * SECTOR + ARRAY_BYTES]
    assert array_crc == zlib.crc32(array), "bad partition array checksum"

    return array


def check_disk(image, expected, num_sectors):
    disk = image.read_bytes()
    assert len(disk) == num_sectors * SECTOR, f"image is {len(disk)} bytes"

    primary = check_header(disk, 1, num_sectors - 1, 2, num_sectors)
    backup = check_header(disk, num_sectors - 1, 1, num_sectors - 33, num_sectors)
    assert primary == backup, "partition arrays differ"

    # The file-only patch must not have touched the protective MBR
    assert disk[:4] == bytes(4), "patch of a file applied to the disk"

    first, last = struct.unpack_from("<QQ", primary, ENTRY_SIZE + 32)
    assert first == ROOTFS_START
    assert last == num_sectors - 34, "last partition not grown to the disk"

    offset = BOOT_START * SECTOR
    assert disk[offset:offset + len(expected["boot"])] == expected["boot"]

    # The sparse image is expanded: raw and fill chunks, then a hole
    offset = ROOTFS_START * SECTOR
    rootfs = expected["rootfs"]
    assert disk[offset:offset + len(rootfs)] == rootfs
    hole = (ROOTFS_BLOCKS - 3) * SPARSE_BLOCK
    assert disk[offset + len(rootfs):offset + len(rootfs) + hole] == bytes(hole)


def test_minimal_size(qdl, work, expected):
    run_qdl(qdl, "--disk-image=disk.img", "prog.elf", "rawprogram0.xml",
            "patch0.xml", cwd=work)

    # End of the last partition, followed by the backup GPT
    num_sectors = ROOTFS_START + ROOTFS_BLOCKS * SPARSE_BLOCK // SECTOR + 33
    check_disk(work / "disk.img", expected, num_sectors)


def test_explicit_size(qdl, work, expected):
    run_qdl(qdl, "--disk-image", "big.img", "--disk-size=4M", "prog.elf",
            "rawprogram0.xml", "patch0.xml", cwd=work)

    check_disk(work / "big.img", expected, 4 * 1024 * 1024 // SECTOR)


def test_stale_image_replaced(qdl, work, expected):
    (work / "disk.img").write_bytes(b"\xff" * 8 * 1024 * 1024)

    test_minimal_size(qdl, work, expected)


def test_raw_commands(qdl, work, expected):
    # Nothing states the sector size here, eMMC selects 512 bytes
    run_qdl(qdl, "--disk-image=raw.img", "--disk-size=1M", "--storage=emmc",
            "prog.elf", "write", "0/16", "boot.img",
            "read", f"0/16+{BOOT_SECTORS}", "readback.bin",
            "erase", "0/16+1",
            "read", "0/16+2", "erased.bin", cwd=work)

    assert (work / "raw.img").stat().st_size == 1024 * 1024
    assert (work / "readback.bin").read_bytes() == expected["boot"]
    assert (work / "erased.bin").read_bytes() == \
        bytes(SECTOR) + expected["boot"][SECTOR:2 * SECTOR]


def test_reject_other_partition(qdl, work):
    result = run_qdl(qdl, "--disk-image=bad.img", "prog.elf",
                     "rawprogram0.xml", "rawprogram1.xml", "patch0.xml",
                     cwd=work, expect_success=False)

    assert "partition 0 only" in result.stderr + result.stdout
    assert not (work / "bad.img").exists(), "image created despite the error"


def test_reject_partition_name(qdl, work):
    run_qdl(qdl, "--disk-image=bad.img", "prog.elf", "write", "boot",
            "boot.img", cwd=work, expect_success=False)

    assert not (work / "bad.img").exists(), "image created despite the error"


def test_reject_small_size(qdl, work):
    run_qdl(qdl, "--disk-image=bad.img", "--disk-size=64K", "prog.elf",
            "rawprogram0.xml", "patch0.xml", cwd=work, expect_success=False)
    run_qdl(qdl, "--disk-image=bad.img", "--disk-size=1000001", "prog.elf",
            "rawprogram0.xml", "patch0.xml", cwd=work, expect_success=False)

    assert not (work / "bad.img").exists(), "image created despite the error"


def test_failed_run_leaves_no_image(qdl, work):
    # The erase is not sized for, so it fails once the image is being written
    args = ["--disk-size=64K", "--storage=emmc", "prog.elf",
            "write", "0/0", "boot.img", "erase", "0/4096+1"]

    result = run_qdl(qdl, "--disk-image=bad.img", *args, cwd=work,
                     expect_success=False)
    assert "flashed" in result.stdout, "run failed before writing the image"
    assert not (work / "bad.img").exists(), "partial image left behind"
    assert not (work / "bad.img.partial").exists(), "temporary file left behind"

    # An image from an earlier run is not lost to a failing one
    (work / "kept.img").write_bytes(b"earlier image")
    run_qdl(qdl, "--disk-image=kept.img", *args, cwd=work, expect_success=False)
    assert (work / "kept.img").read_bytes() == b"earlier image"
    assert not (work / "kept.img.partial").exists(), "temporary file left behind"


def test_reject_image_as_input(qdl, work, expected):
    shutil.copy(work / "boot.img", work / "alias.img")
    names = ["alias.img"]
    if os.name != "nt":
        names.append("./alias.img")

    for name in names:
        result = run_qdl(qdl, f"--disk-image={name}", "--disk-size=1M",
                         "--storage=emmc", "prog.elf", "write", "0/0",
                         "alias.img", cwd=work, expect_success=False)
        assert "both the disk image and" in result.stderr + result.stdout

        result = run_qdl(qdl, f"--disk-image={name}", "--disk-size=1M",
                         "--storage=emmc", "prog.elf", "write", "0/0",
                         "boot.img", "read", "0/0+1", "alias.img", cwd=work,
                         expect_success=False)
        assert "both the disk image and" in result.stderr + result.stdout

    assert (work / "alias.img").read_bytes() == expected["boot"]

    # Nor any other input, nor a new file under another spelling
    for name, args in (("prog.elf", ["prog.elf", "rawprogram0.xml"]),
                       ("rawprogram0.xml", ["prog.elf", "rawprogram0.xml"]),
                       ("new.img", ["prog.elf", "write", "0/0", "boot.img",
                                    "read", "0/0+1", "./new.img"])):
        before = (work / name).read_bytes() if (work / name).exists() else None
        result = run_qdl(qdl, f"--disk-image={name}", "--disk-size=1M",
                         "--storage=emmc", *args, cwd=work,
                         expect_success=False)
        assert "both the disk image and" in result.stderr + result.stdout
        if before is None:
            assert not (work / name).exists()
        else:
            assert (work / name).read_bytes() == before


def test_reject_standalone_commands(qdl, work, expected):
    # These would otherwise drop the option and look for a device
    for command in (["reset"], ["chipinfo"], ["list"],
                    ["ramdump", "-o", "dump"]):
        for option in (["--disk-image=bad.img"], ["--disk-image", "bad.img"],
                       ["--disk-image=bad.img", "--disk-size", "1M"],
                       ["--disk-im=bad.img"], ["--disk-s", "1M"]):
            result = run_qdl(qdl, *option, *command, cwd=work,
                             expect_success=False)
            assert "cannot be used with" in result.stderr + result.stdout

    assert not (work / "bad.img").exists()

    # An image may be named like a subcommand
    run_qdl(qdl, "--disk-image", "reset", "prog.elf", "rawprogram0.xml",
            "patch0.xml", cwd=work)
    num_sectors = ROOTFS_START + ROOTFS_BLOCKS * SPARSE_BLOCK // SECTOR + 33
    check_disk(work / "reset", expected, num_sectors)


def write_flashmap(work, name, memories, files=("rawprogram0.xml", "patch0.xml")):
    flashmap = {
        "version": "1.1.0",
        "products": [{
            "name": "unit-test",
            "layouts": [{
                "name": "layout0",
                "programmer": ["prog.elf"],
                "programmable": [{
                    "memory": memory,
                    "slot": 0,
                    "files": list(files),
                } for memory in memories],
            }],
        }],
    }
    (work / name).write_text(json.dumps(flashmap))


def test_flashmap_storage(qdl, work, expected):
    write_flashmap(work, "emmc.json", ["emmc"])
    write_flashmap(work, "two.json", ["emmc", "spinor"])
    write_flashmap(work, "nand.json", ["nand"], files=["rawprogram_nand.xml"])
    (work / "rawprogram_nand.xml").write_text(
        '<?xml version="1.0" ?>\n<data>\n' +
        program(0, BOOT_SECTORS, "boot.img", "boot").replace(
            "<program ", '<program PAGES_PER_BLOCK="64" ') +
        '</data>\n')

    run_qdl(qdl, "--disk-image=map.img", "flash", "emmc.json", cwd=work)
    num_sectors = ROOTFS_START + ROOTFS_BLOCKS * SPARSE_BLOCK // SECTOR + 33
    check_disk(work / "map.img", expected, num_sectors)

    # Configuring the same storage again is no second device
    write_flashmap(work, "twice.json", ["emmc", "emmc"])
    run_qdl(qdl, "--disk-image=twice.img", "flash", "twice.json", cwd=work)
    check_disk(work / "twice.img", expected, num_sectors)

    # Two storage devices do not fit one image, whatever --storage says
    result = run_qdl(qdl, "--disk-image=bad.img", "flash", "two.json",
                     cwd=work, expect_success=False)
    assert "one storage device" in result.stderr + result.stdout

    result = run_qdl(qdl, "--disk-image=bad.img", "--storage=emmc", "flash",
                     "nand.json", cwd=work, expect_success=False)
    assert "NAND" in result.stderr + result.stdout

    assert not (work / "bad.img").exists(), "image created despite the error"


def test_partition_extents(qdl, work):
    # A sparse image smaller than its fixed-size partition, followed by an
    # empty partition left to grow: both need their room in the image
    (work / "extent.xml").write_text(
        '<?xml version="1.0" ?>\n<data>\n' +
        program(ROOTFS_START, 2 * ROOTFS_BLOCKS * SPARSE_BLOCK // SECTOR,
                "rootfs.img", "rootfs", sparse=True) +
        program(ROOTFS_START + 2 * ROOTFS_BLOCKS * SPARSE_BLOCK // SECTOR, 0,
                "", "userdata") +
        program("NUM_DISK_SECTORS-33.", 33, "gpt_backup0.bin", "BackupGPT") +
        '</data>\n')

    run_qdl(qdl, "--disk-image=extent.img", "prog.elf", "extent.xml", cwd=work)
    size = (work / "extent.img").stat().st_size
    assert size == (ROOTFS_START + 2 * ROOTFS_BLOCKS * SPARSE_BLOCK // SECTOR
                    + 1 + 33) * SECTOR, f"image is {size} bytes"


def test_sparse_sector_size(qdl, work):
    # Nothing but a hole, which still states its 512-byte sectors
    (work / "hole.img").write_bytes(
        struct.pack("<IHHHHIIII", 0xed26ff3a, 1, 0, 28, 12, SPARSE_BLOCK, 8,
                    1, 0) +
        struct.pack("<HHII", 0xcac3, 0, 8, 12))
    (work / "hole.xml").write_text(
        '<?xml version="1.0" ?>\n<data>\n' +
        program(0, 8 * SPARSE_BLOCK // SECTOR, "hole.img", "hole", sparse=True) +
        '</data>\n')

    run_qdl(qdl, "--disk-image=hole-disk.img", "prog.elf", "hole.xml", cwd=work)
    size = (work / "hole-disk.img").stat().st_size
    assert size == 8 * SPARSE_BLOCK, f"image is {size} bytes"


def test_raw_write_limit(qdl, work):
    (work / "one.bin").write_bytes(b"\x5a" * SECTOR)

    # The length of an address limits the write, it does not size the disk
    run_qdl(qdl, "--disk-image=one.img", "--disk-size=512", "--storage=emmc",
            "prog.elf", "write", "0/0+2048", "one.bin", cwd=work)
    assert (work / "one.img").read_bytes() == b"\x5a" * SECTOR


def test_literal_patch_value(qdl, work):
    (work / "literal.xml").write_text(
        '<?xml version="1.0" ?>\n<patches>\n' +
        # Larger than the number of sectors, which it has nothing to do with
        patch(3, 8, 4, "305419896") +
        patch(3, 16, 8, "18446744073709551615") +
        '</patches>\n')
    (work / "wide.xml").write_text(
        '<?xml version="1.0" ?>\n<patches>\n' +
        patch(3, 8, 2, "65536") +
        '</patches>\n')

    run_qdl(qdl, "--disk-image=literal.img", "--disk-size=64K", "prog.elf",
            "literal.xml", cwd=work)
    disk = (work / "literal.img").read_bytes()
    assert disk[3 * SECTOR + 8:3 * SECTOR + 12] == bytes.fromhex("78563412")
    assert disk[3 * SECTOR + 16:3 * SECTOR + 24] == b"\xff" * 8

    result = run_qdl(qdl, "--disk-image=bad.img", "--disk-size=64K",
                     "prog.elf", "wide.xml", cwd=work, expect_success=False)
    assert "does not fit" in result.stderr + result.stdout
    assert not (work / "bad.img").exists(), "partial image left behind"


def test_reject_size_without_image(qdl, work):
    run_qdl(qdl, "--dry-run", "--disk-size=4M", "prog.elf", "rawprogram0.xml",
            cwd=work, expect_success=False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--builddir", required=True)
    args = parser.parse_args()

    qdl = Path(args.builddir) / ("qdl.exe" if os.name == "nt" else "qdl")
    if not qdl.exists():
        raise AssertionError(f"qdl binary not found: {qdl}")

    work = Path(tempfile.mkdtemp(prefix="disk-image-",
                                dir=Path(args.builddir) / "tests"))

    try:
        build = work / "build"
        expected = write_build(build)

        test_minimal_size(str(qdl), build, expected)
        test_explicit_size(str(qdl), build, expected)
        test_stale_image_replaced(str(qdl), build, expected)
        test_raw_commands(str(qdl), build, expected)
        test_reject_other_partition(str(qdl), build)
        test_reject_partition_name(str(qdl), build)
        test_reject_small_size(str(qdl), build)
        test_failed_run_leaves_no_image(str(qdl), build)
        test_reject_image_as_input(str(qdl), build, expected)
        test_reject_standalone_commands(str(qdl), build, expected)
        test_flashmap_storage(str(qdl), build, expected)
        test_literal_patch_value(str(qdl), build)
        test_partition_extents(str(qdl), build)
        test_sparse_sector_size(str(qdl), build)
        test_raw_write_limit(str(qdl), build)
        test_reject_size_without_image(str(qdl), build)
    finally:
        shutil.rmtree(work)


if __name__ == "__main__":
    main()
