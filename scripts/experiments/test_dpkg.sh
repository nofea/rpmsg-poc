docker run --rm zephyrprojectrtos/ci:latest bash -c "
sudo apt-get update && sudo apt-get install -y gcc-aarch64-linux-gnu make cmake git wget
wget -q http://ports.ubuntu.com/pool/main/s/sysfsutils/libsysfs-dev_2.1.1-6ubuntu1_arm64.deb
wget -q http://ports.ubuntu.com/pool/main/s/sysfsutils/libsysfs2_2.1.1-6ubuntu1_arm64.deb
dpkg-deb -x libsysfs-dev_2.1.1-6ubuntu1_arm64.deb sysfs
dpkg-deb -x libsysfs2_2.1.1-6ubuntu1_arm64.deb sysfs
sudo cp -r sysfs/usr/include/sysfs /usr/aarch64-linux-gnu/include/
sudo cp -d sysfs/lib/aarch64-linux-gnu/libsysfs* /usr/aarch64-linux-gnu/lib/
sudo cp -d sysfs/usr/lib/aarch64-linux-gnu/libsysfs* /usr/aarch64-linux-gnu/lib/
git clone https://github.com/OpenAMP/libmetal.git /tmp/libmetal
cd /tmp/libmetal && git checkout v2024.05.0
mkdir build && cd build
cmake .. -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_FIND_ROOT_PATH=/usr/aarch64-linux-gnu -DCMAKE_INSTALL_PREFIX=/usr/aarch64-linux-gnu
make
"
