FROM zephyrprojectrtos/ci:latest

# The base image already ships Renode (/opt/renode, on PATH), the Zephyr SDK,
# west, dtc and e2fsprogs (debugfs). We only add the AArch64 Linux userspace
# toolchain and the OpenAMP libraries for the A53 side.

RUN apt-get update && \
    apt-get install -y --no-install-recommends gcc-aarch64-linux-gnu libc6-dev-arm64-cross && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /tmp/build

# CMake toolchain file for cross-compiling into the AArch64 sysroot
RUN printf '%s\n' \
      'set(CMAKE_SYSTEM_NAME Linux)' \
      'set(CMAKE_SYSTEM_PROCESSOR aarch64)' \
      'set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)' \
      'set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)' \
      'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)' \
      'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)' \
      'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)' \
      > /tmp/build/aarch64-linux.cmake

# libsysfs (libmetal's Linux backend depends on it) for arm64, unpacked into the sysroot
RUN wget -q http://ports.ubuntu.com/pool/main/s/sysfsutils/libsysfs-dev_2.1.1-6build1_arm64.deb && \
    wget -q http://ports.ubuntu.com/pool/main/s/sysfsutils/libsysfs2_2.1.1-6build1_arm64.deb && \
    dpkg-deb -x libsysfs-dev_2.1.1-6build1_arm64.deb sysfs && \
    dpkg-deb -x libsysfs2_2.1.1-6build1_arm64.deb sysfs && \
    cp -r sysfs/usr/include/sysfs /usr/aarch64-linux-gnu/include/ && \
    cp -d sysfs/usr/lib/aarch64-linux-gnu/libsysfs* /usr/aarch64-linux-gnu/lib/ && \
    rm -rf sysfs *.deb

# libmetal and open-amp, built as static libraries so the userspace app can be
# linked statically (the guest rootfs has none of these libraries)
RUN git clone -q --depth 1 -b v2024.05.0 https://github.com/OpenAMP/libmetal.git && \
    cmake -S libmetal -B libmetal/build -DCMAKE_TOOLCHAIN_FILE=/tmp/build/aarch64-linux.cmake \
          -DCMAKE_INSTALL_PREFIX=/usr/aarch64-linux-gnu \
          -DWITH_SHARED_LIB=OFF -DWITH_TESTS=OFF -DWITH_EXAMPLES=OFF -DWITH_DOC=OFF && \
    cmake --build libmetal/build -j && cmake --install libmetal/build

RUN git clone -q --depth 1 -b v2024.05.0 https://github.com/OpenAMP/open-amp.git && \
    cmake -S open-amp -B open-amp/build -DCMAKE_TOOLCHAIN_FILE=/tmp/build/aarch64-linux.cmake \
          -DCMAKE_INSTALL_PREFIX=/usr/aarch64-linux-gnu \
          -DWITH_SHARED_LIB=OFF -DWITH_PROXY=OFF -DWITH_APPS=OFF && \
    cmake --build open-amp/build -j && cmake --install open-amp/build && \
    rm -rf /tmp/build

WORKDIR /workspace
