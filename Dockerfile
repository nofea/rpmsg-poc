FROM zephyrprojectrtos/ci:latest

# Install Renode (Portable to avoid dependency issues on newer Ubuntu versions)
RUN sudo apt-get update && \
    sudo apt-get install -y wget curl cmake && \
    wget https://github.com/renode/renode/releases/download/v1.15.3/renode-1.15.3.linux-portable.tar.gz && \
    sudo mkdir -p /opt/renode && \
    sudo tar -xzf renode-1.15.3.linux-portable.tar.gz -C /opt/renode --strip-components=1 && \
    sudo ln -s /opt/renode/renode /usr/local/bin/renode && \
    rm renode-1.15.3.linux-portable.tar.gz

# We need the AArch64 GCC for the userspace app
RUN sudo apt-get install -y gcc-aarch64-linux-gnu make

WORKDIR /workspace

# Download and install arm64 libsysfs manually into the cross-compiler sysroot
RUN wget -q http://ports.ubuntu.com/pool/main/s/sysfsutils/libsysfs-dev_2.1.1-6build1_arm64.deb && \
    wget -q http://ports.ubuntu.com/pool/main/s/sysfsutils/libsysfs2_2.1.1-6build1_arm64.deb && \
    dpkg-deb -x libsysfs-dev_2.1.1-6build1_arm64.deb sysfs && \
    dpkg-deb -x libsysfs2_2.1.1-6build1_arm64.deb sysfs && \
    sudo cp -r sysfs/usr/include/sysfs /usr/aarch64-linux-gnu/include/ && \
    sudo cp -d sysfs/lib/aarch64-linux-gnu/libsysfs* /usr/aarch64-linux-gnu/lib/ 2>/dev/null || true && \
    sudo cp -d sysfs/usr/lib/aarch64-linux-gnu/libsysfs* /usr/aarch64-linux-gnu/lib/ 2>/dev/null || true && \
    rm -rf sysfs *.deb

# Cross-compile libmetal for AArch64
RUN git clone https://github.com/OpenAMP/libmetal.git /tmp/libmetal && \
    cd /tmp/libmetal && git checkout v2024.05.0 && \
    mkdir build && cd build && \
    cmake .. -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
             -DCMAKE_SYSTEM_NAME=Linux \
             -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
             -DCMAKE_FIND_ROOT_PATH=/usr/aarch64-linux-gnu \
             -DCMAKE_INSTALL_PREFIX=/usr/aarch64-linux-gnu && \
    make && sudo make install

# Cross-compile open-amp for AArch64
RUN git clone https://github.com/OpenAMP/open-amp.git /tmp/open-amp && \
    cd /tmp/open-amp && git checkout v2024.05.0 && \
    mkdir build && cd build && \
    cmake .. -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
             -DCMAKE_SYSTEM_NAME=Linux \
             -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
             -DCMAKE_FIND_ROOT_PATH=/usr/aarch64-linux-gnu \
             -DCMAKE_INSTALL_PREFIX=/usr/aarch64-linux-gnu \
             -DWITH_LIBMETAL_FIND=OFF && \
    make && sudo make install
