#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# TEST ONLY (not for upstream): drive the nbdkit plugin against the HIL
# board so firehose_getsize() / getstorageinfo parsing is exercised.
set -u

echo "=== nbdkit getstorageinfo probe on $(uname -s) ==="
if ! command -v nbdkit >/dev/null || ! command -v nbdinfo >/dev/null ||
   ! pkg-config --exists nbdkit 2>/dev/null; then
	echo "nbdkit, nbdinfo or nbdkit headers missing; trying passwordless install"
	if [ "$(uname -s)" = Linux ] && sudo -n true 2>/dev/null; then
		sudo -n apt-get install -y nbdkit nbdkit-plugin-dev libnbd-bin
	fi
fi
if ! command -v nbdkit >/dev/null || ! command -v nbdinfo >/dev/null ||
   ! pkg-config --exists nbdkit 2>/dev/null; then
	echo "NBDKIT-PROBE: SKIP (nbdkit not available)"
	exit 0
fi

meson setup build-nbd -Dnbdkit=enabled >/dev/null && meson compile -C build-nbd >/dev/null || {
	echo "NBDKIT-PROBE: FAIL (plugin build)"; exit 1; }
PLUGIN="$PWD/build-nbd/nbdkit-qdl-plugin.so"

BUILD="${QDL_HIL_BUILD:?}"
[ -d "$BUILD" ] || BUILD="$(dirname "$BUILD")"
PROG="${QDL_HIL_PROGRAMMER:-$(ls "$BUILD"/prog_firehose_*.elf | head -1)}"
STORAGE="${QDL_HIL_STORAGE:?}"
echo "programmer=$PROG storage=$STORAGE"

# The plugin has no serial option and opens the first EDL device it finds.
# Hide every other Qualcomm EDL device while probing so a second board on
# the same host is never touched; restore them on exit.
HIDDEN=()
restore() {
	for a in "${HIDDEN[@]}"; do echo 1 | sudo -n tee "$a" >/dev/null; done
}
trap restore EXIT
hide_others() {
	local d serial
	for d in /sys/bus/usb/devices/*; do
		[ "$(cat "$d/idVendor" 2>/dev/null)" = 05c6 ] || continue
		[ "$(cat "$d/idProduct" 2>/dev/null)" = 9008 ] || continue
		serial=$(cat "$d/serial" 2>/dev/null)
		case "$serial" in *"${QDL_HIL_SERIAL:?}"*) continue ;; esac
		[ "$(cat "$d/authorized")" = 1 ] || continue
		echo "hiding EDL device $d serial=$serial"
		echo 0 | sudo -n tee "$d/authorized" >/dev/null && HIDDEN+=("$d/authorized")
	done
}

rc=0
for lun in ${NBD_LUNS:-0 1}; do
	echo "--- LUN $lun"
	if [ -n "${QDL_HIL_ENTER_EDL_CMD:-}" ]; then
		bash -c "$QDL_HIL_ENTER_EDL_CMD" || { echo "enter EDL failed"; rc=1; continue; }
	fi
	for i in $(seq 1 60); do
		./build/qdl list 2>/dev/null | grep -q "${QDL_HIL_SERIAL:?}" && break
		sleep 1
	done
	sleep 5
	if [ "$(uname -s)" = Linux ]; then hide_others; fi
	./build/qdl list
	if ./build/qdl list | grep -qv "${QDL_HIL_SERIAL}"; then
		echo "NBDKIT-PROBE: LUN $lun FAIL (another EDL device still visible)"; rc=1; continue
	fi
	out=$(timeout 180 nbdkit -U - "$PLUGIN" programmer="$PROG" storage="$STORAGE" \
		lun="$lun" debug=1 --run 'nbdinfo --size "$uri"' 2>&1)
	r=$?
	echo "$out" | grep -E "getstorageinfo|RX:.*(num_partition_sectors|SECTOR_SIZE_IN_BYTES|num_physical|total_blocks|storage_info)|geometry|failed to query" | cut -c1-300
	size=$(echo "$out" | grep -E '^[0-9]+$' | tail -1)
	if [ $r -eq 0 ] && [ -n "$size" ] && [ "$size" -gt 0 ]; then
		echo "NBDKIT-PROBE: LUN $lun PASS size=$size bytes"
	else
		echo "NBDKIT-PROBE: LUN $lun FAIL (rc=$r)"
		echo "$out" | tail -40
		rc=1
	fi
done
exit $rc
