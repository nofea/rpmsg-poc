#!/bin/bash
set -e

echo "=== AM64x RPMsg POC Build & Run Script (Userspace Pivot) ==="

echo "1. Initialize Zephyr Workspace (if not done)"
if [ ! -d "zephyrproject" ]; then
    west init zephyrproject
    cd zephyrproject
    west update
    west zephyr-export
    cd ..
fi

echo "2. Build Zephyr Application for Cortex-R5F"
cd zephyrproject
west build -p always -b qemu_cortex_r5 ../zephyr_app -d ../zephyr_app/build
cd ..

echo "3. Build A53 Userspace OpenAMP Application"
aarch64-linux-gnu-gcc -O2 -o am64_rpmsg_userspace am64_rpmsg_userspace.c \
    -I/usr/aarch64-linux-gnu/include \
    -L/usr/aarch64-linux-gnu/lib \
    -lopen_amp -lmetal

echo "4. The Userspace Application has been built successfully."
echo "Note: Before running Renode, ensure the 'am64_rpmsg_userspace' binary is injected"
echo "into the simulated Linux rootfs so it can be executed from the A53 console."

echo "5. Running Renode Simulation"
# Start renode headless and run the script
renode -e "s @run_poc.resc; start" --disable-x11
