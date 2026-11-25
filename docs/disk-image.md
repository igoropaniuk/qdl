# Creating a disk image

Instead of flashing a device, QDL can write what it would have flashed into a
file. The result is an image of the whole storage - partition table included
and adjusted to the size of the disk - that can be copied to an SD card or
eMMC with `dd` or a similar tool, or attached to an emulator.

The `--disk-image` option selects the file to create. It implies `--dry-run`:
no device is needed and none is touched.

```bash
qdl --disk-image=disk.img --disk-size=$(sudo blockdev --getsize64 /dev/sdX) \
    prog_firehose_ddr.elf rawprogram0.xml patch0.xml
sudo dd if=disk.img of=/dev/sdX bs=4M conv=fsync status=progress
```

The backup partition table belongs at the very end of the media, which is why
the image is created with the exact size of the card here, see
[Size of the image](#size-of-the-image).

The image is produced by the simulated device of the dry run, which stores
the data of each `<program>`, applies each `<patch>` and honours each
`<erase>` the same way a Firehose programmer would on real storage. Everything
QDL can flash is therefore supported, such as sparse images, installer
packages and the `flash` subcommand:

```bash
qdl --disk-image=disk.img flash contents.xml::emmc
```

The image is built in a temporary file next to the requested one, named
after it with a `.partial` suffix, and only takes its place once the run has
succeeded. A failed run therefore leaves no image behind, and an existing file
of the same name untouched.

## Size of the image

By default the image is as small as the build allows: it ends where the last
programmed partition does, followed by what the build places at the end of the
disk, i.e. the backup partition table. A last partition that is meant to grow
gets no room beyond the file programmed into it.

Such an image suits an emulator, or media of exactly that size. Copied to
larger media its backup partition table is no longer at the end of the disk,
which partitioning tools report as an error until it is moved there, e.g.
with `sgdisk -e`.

Use `--disk-size` to create an image of the size of the target media instead.
The size is given in bytes, optionally with a `K`, `M` or `G` suffix
(powers of 1024), and must be a multiple of the sector size:

```bash
qdl --disk-image=disk.img --disk-size=16G prog_firehose_ddr.elf rawprogram0.xml patch0.xml
```

The patches then place the backup partition table at the end of those 16 GiB
and grow the last partition to fill them. The image is created as a sparse
file, so on most filesystems only the programmed data takes up space.

## Limitations

The image represents a single flat block device, so only builds that keep
everything in physical partition 0 can be converted. QDL refuses to create an
image if any operation addresses another physical partition, which rules out:

- UFS layouts spread over several LUNs
- eMMC layouts using the boot or other hardware partitions
- NVMe layouts using more than one namespace

For the same reason an installer package or *contents.xml* flashing several
storage devices must be narrowed down to one with a selector. NAND storage
and UFS provisioning are not supported either.

The image file cannot at the same time be one of the inputs of the run, such
as the programmer, an XML file or a file to flash, nor a file to read into.

Raw [read, write and erase](raw-io.md) commands work on the image, but must
use numeric addresses: partition names cannot be looked up. As these commands
carry no sector size, 512 bytes is used with `--storage=emmc` and 4096 bytes
otherwise.
