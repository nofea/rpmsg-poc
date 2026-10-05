#!/bin/bash
# Build the R5F firmware, the A53 userspace app, the Linux DTB and rootfs, then
# run the POC in Renode. Meant to run inside the rpmsg-poc container:
#
#   docker run --rm -it -v "$PWD":/workspace rpmsg-poc ./scripts/build_and_run.sh [mode]
#
# mode: test        (default) headless acceptance test, renode/rpmsg_poc.robot
#       interactive  Renode console with the simulation running; the Linux
#                    console (uart1) is served on TCP port 3456 and the Zephyr
#                    shell (uart0) on 3457 (publish them with -p 3456:3456
#                    -p 3457:3457), log in as root and run /root/am64_rpmsg_userspace
#       build        build only
set -euo pipefail
cd "$(dirname "$0")/.."

MODE=${1:-test}
LINUX_CONSOLE_PORT=3456
ZEPHYR_CONSOLE_PORT=3457
ZEPHYR_VERSION=v4.5.0-rc1
BUILD=build
DOWNLOADS=$BUILD/downloads

# Antmicro's ZynqMP OpenAMP demo images; the rootfs ships uio_pdrv_genirq.ko
# for the kernel used in renode/run_poc.resc
ANTMICRO=https://dl.antmicro.com/projects/renode
BASE_DTB=zynqmp--sm-k26-revA-openamp.dtb-s_39685-7bc435ab006028eab6e7da35bff8f7ce891cfce9
BASE_ROOTFS=zynqmp--linux-rootfs-openamp.ext2-s_62914560-56183cdaeef8af742ea31ee9044b733ccf49c0d1

mkdir -p "$DOWNLOADS"

echo "=== 1. Zephyr workspace ($ZEPHYR_VERSION)"
if [ ! -d zephyrproject/.west ]; then
    west init -m https://github.com/zephyrproject-rtos/zephyr --mr "$ZEPHYR_VERSION" zephyrproject
    (cd zephyrproject &&
        west config manifest.project-filter -- "-.*,+open-amp,+libmetal,+cmsis,+cmsis_6" &&
        west update --narrow -o=--depth=1)
fi

echo "=== 2. Zephyr firmware for the R5F"
(cd zephyrproject && west build -p always -b qemu_cortex_r5 ../zephyr_app -d ../zephyr_app/build)

echo "=== 3. A53 userspace app"
aarch64-linux-gnu-gcc -O2 -Wall -static -DVIRTIO_DRIVER_SUPPORT=1 \
    -Icommon -o linux_app/am64_rpmsg_userspace linux_app/am64_rpmsg_userspace.c \
    -lopen_amp -lmetal -lsysfs -lpthread -lrt

echo "=== 4. Linux device tree"
for f in "$BASE_DTB" "$BASE_ROOTFS"; do
    [ -f "$DOWNLOADS/$f" ] || wget -q -O "$DOWNLOADS/$f" "$ANTMICRO/$f"
done
dtc -q -@ -I dts -O dtb -o "$BUILD/am64_rpmsg_overlay.dtbo" linux_app/am64_rpmsg_overlay.dts
fdtoverlay -i "$DOWNLOADS/$BASE_DTB" -o "$BUILD/am64_rpmsg_poc.dtb" "$BUILD/am64_rpmsg_overlay.dtbo"

echo "=== 5. Linux rootfs (inject the app and load uio_pdrv_genirq at boot)"
cp "$DOWNLOADS/$BASE_ROOTFS" "$BUILD/rootfs.ext2"
cat > "$BUILD/S95am64-rpmsg" <<'EOF'
#!/bin/sh
# AM64x RPMsg POC: bind uio_pdrv_genirq to the generic-uio nodes
[ "$1" = "start" ] && modprobe uio_pdrv_genirq of_id=generic-uio
exit 0
EOF
debugfs -w "$BUILD/rootfs.ext2" -f - >/dev/null <<EOF
write linux_app/am64_rpmsg_userspace /root/am64_rpmsg_userspace
sif /root/am64_rpmsg_userspace mode 0100755
sif /root/am64_rpmsg_userspace uid 0
sif /root/am64_rpmsg_userspace gid 0
write $BUILD/S95am64-rpmsg /etc/init.d/S95am64-rpmsg
sif /etc/init.d/S95am64-rpmsg mode 0100755
sif /etc/init.d/S95am64-rpmsg uid 0
sif /etc/init.d/S95am64-rpmsg gid 0
rm /etc/init.d/S99-enable-uart0.sh
EOF
e2fsck -fn "$BUILD/rootfs.ext2" >/dev/null

case "$MODE" in
build)
    echo "=== Build complete"
    ;;
interactive)
    echo "=== 6. Renode (interactive); Linux console on TCP port $LINUX_CONSOLE_PORT, Zephyr shell on $ZEPHYR_CONSOLE_PORT"
    renode --console -e "include @renode/run_poc.resc; \
        emulation CreateServerSocketTerminal $LINUX_CONSOLE_PORT \"linux_console\" false; \
        connector Connect sysbus.uart1 linux_console; \
        emulation CreateServerSocketTerminal $ZEPHYR_CONSOLE_PORT \"zephyr_console\" false; \
        connector Connect sysbus.uart0 zephyr_console; start"
    ;;
test)
    echo "=== 6. Renode acceptance test"
    renode-test --results-dir "$BUILD/test-results" renode/rpmsg_poc.robot
    ;;
*)
    echo "unknown mode: $MODE" >&2
    exit 1
    ;;
esac
