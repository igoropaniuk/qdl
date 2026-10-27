#!/bin/bash
# pr347 nbdkit getstorageinfo probe: probe.sh <storage> <dir> <programmer> "<luns>"
STORAGE=$1; DIR=$2; PROG=$3; LUNS=$4
apt-get install -y -qq --no-install-recommends nbdkit nbdkit-plugin-dev libnbd-bin >/dev/null 2>&1 || true
if ! command -v nbdkit >/dev/null || ! command -v nbdinfo >/dev/null || ! pkg-config --exists nbdkit; then
	echo "nbdkit/nbdinfo/headers unavailable"; lava-test-case pr347-nbdkit-deps --result fail; exit 0
fi
lava-test-case pr347-nbdkit-deps --result pass
nbdkit --version
if ! (cd /qdl && meson setup build-nbd -Dnbdkit=enabled >/dev/null && meson compile -C build-nbd >/dev/null); then
	lava-test-case pr347-nbdkit-build --result fail; exit 0
fi
lava-test-case pr347-nbdkit-build --result pass
PLUGIN=/qdl/build-nbd/nbdkit-qdl-plugin.so
case "$PROG" in *.xml) PROG=prog_firehose_ddr.elf ;; esac
echo "probe storage=$STORAGE programmer=$DIR/$PROG luns=$LUNS"
first=1
for lun in $LUNS; do
	if [ $first = 0 ]; then
		sleep 5; lava-device-command qdl_enter || echo "qdl_enter failed"
		for i in $(seq 1 60); do lsusb -d 05c6:9008 >/dev/null && break; sleep 2; done
	fi
	first=0
	lsusb -d 05c6:9008
	RC=0
	out=$(cd "$DIR" && timeout 180 nbdkit -U - "$PLUGIN" programmer="$DIR/$PROG" storage="$STORAGE" lun="$lun" debug=1 --run 'nbdinfo --size "$uri"' 2>&1) || RC=$?
	echo "$out" > /tmp/nbd-lun$lun.log
	echo "--- getstorageinfo exchange, LUN $lun:"
	echo "$out" | grep -E 'getstorageinfo|num_partition_sectors=|SECTOR_SIZE_IN_BYTES=|num_physical_partitions|total_blocks|storage geometry|failed to|ERR|error' | grep -v 'program ' | cut -c1-400 | head -20
	size=$(echo "$out" | grep -E '^[0-9]+$' | tail -1)
	FMT=unknown
	echo "$out" | grep -q 'total_blocks' && FMT=JSON
	echo "$out" | grep -q 'value="num_partition_sectors=' && FMT=KV
	echo "PR347_PROBE lun=$lun rc=$RC size=${size:-none} format=$FMT"
	if [ $RC = 0 ] && [ -n "$size" ] && [ "$size" -gt 0 ]; then
		lava-test-case pr347-nbdkit-lun$lun --result pass --measurement "$size" --units bytes
	else
		lava-test-case pr347-nbdkit-lun$lun --result fail
		echo "$out" | tail -15 | cut -c1-300
	fi
	lava-test-case pr347-format-lun$lun-$FMT --result pass
done
exit 0
