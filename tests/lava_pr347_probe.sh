#!/bin/bash
# pr347 nbdkit getstorageinfo probe: probe.sh <storage> <dir> <programmer> "<luns>"
STORAGE=$1; DIR=$2; PROG=$3; LUNS=$4
SER=$(echo "$LAVA_BOARD_ID" | sed -n 's/.*SN:\([0-9A-Fa-f]*\).*/\1/p'); [ -n "$SER" ] || SER=$LAVA_BOARD_ID
# The nbdkit plugin has no serial option: it opens the FIRST EDL device it
# sees. Only run it when this container can see exactly one Qualcomm USB
# device, exactly one EDL device is listed, and its serial is this board's.
isolated() {
	echo "--- isolation check (expected serial '$SER')"
	lsusb 2>&1 | sed 's/^/usb: /'
	# 05c6:9302 is the board's EPM power-measurement probe, not a SoC: it
	# cannot enter EDL, so it does not count. Any other Qualcomm device does.
	QC=$(lsusb 2>/dev/null | grep -i ' 05c6:' | grep -v -i ' 05c6:9302 ' | wc -l)
	E=$(lsusb -d 05c6:9008 2>/dev/null | wc -l)
	LIST=$(/qdl/build/qdl list 2>&1)
	echo "$LIST" | sed 's/^/qdl-list: /'
	N=$(echo "$LIST" | grep -c -i '^05c6:' || true)
	M=$(echo "$LIST" | awk -F'\t' -v s="$SER" 'tolower($1) ~ /^05c6:/ && tolower($2) == tolower(s)' | wc -l)
	echo "ISOLATION qualcomm_soc_usb=$QC edl_usb=$E edl_listed=$N serial_match=$M"
	[ -n "$SER" ] && [ "$QC" = 1 ] && [ "$E" = 1 ] && [ "$N" = 1 ] && [ "$M" = 1 ]
}
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
	if ! isolated; then
		echo "PR347_PROBE lun=$lun SKIPPED (device not isolated)"
		lava-test-case pr347-nbdkit-lun$lun --result skip
		break
	fi
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
