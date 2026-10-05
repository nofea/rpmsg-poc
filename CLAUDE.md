# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Proof of concept for an OpenAMP/RPMsg link between a Cortex-A53 (Linux, RPMsg host) and a Cortex-R5F (Zephyr, remote), aimed at the TI AM6442 SoC and simulated entirely in **Renode**. Renode has no AM64x model, so a **ZynqMP platform stands in** for it, and the stock **ZynqMP IPI** stands in for the TI mailbox. The design spec is [docs/agent_spec_am64x_rpmsg_poc.md](docs/agent_spec_am64x_rpmsg_poc.md); read it before making structural changes. Its acceptance criterion: the Linux console shows the userspace app sending `"ping"` and the Zephyr UART prints `OpenAMP: Received message: "ping"`. The POC also replies `"pong"` so both directions are exercised. Phase 5 of the spec adds custom text messages in both directions: the Linux app's CLI and interactive modes, and a Zephyr shell `rpmsg send` command. Phase 6 adds bulk transfers of up to 8 MB: the payload goes through a dedicated shared-memory region and only a small descriptor goes over RPMsg.

The A53 side is a **userspace app** (libmetal + libopen_amp over `generic-uio`). Don't reintroduce a kernel driver (`am64_rpmsg_client.c` was deleted on purpose). The Python mailbox mock was replaced by the ZynqMP IPI on purpose: `PythonPeripheral` can't drive IRQ lines.

## Layout

- `linux_app/`: A53 userspace app and DT overlay
- `common/`: `rpmsg_bulk.h`, the bulk protocol shared by both apps
- `zephyr_app/`: R5F Zephyr firmware (`dts/bindings/` holds an app-local binding for the mailbox consumer node)
- `renode/`: platform, simulation script, acceptance test (Renode is run from the repo root, so paths in these files are root-relative)
- `scripts/`: `build_and_run.sh`; `scripts/experiments/` holds throwaway scripts
- `docs/`: design spec
- `build/` (generated): Linux DTB, rootfs, downloads, test results

## Build & run

Everything runs inside the image built from [Dockerfile](Dockerfile). It is based on `zephyrprojectrtos/ci`, which already ships Renode 1.16 (`/opt/renode`), the Zephyr SDK, dtc and debugfs. The Dockerfile adds the AArch64 glibc cross compiler and builds libsysfs, libmetal and open-amp (`v2024.05.0`) as static libraries into the `/usr/aarch64-linux-gnu` sysroot. Podman works the same as Docker (add `:Z` to the volume on SELinux hosts).

```bash
docker build -t rpmsg-poc .
docker run --rm -it -v "$PWD":/workspace rpmsg-poc ./scripts/build_and_run.sh              # build + acceptance test
docker run --rm -it -p 3456:3456 -p 3457:3457 -v "$PWD":/workspace rpmsg-poc ./scripts/build_and_run.sh interactive  # Linux console on telnet 127.0.0.1 3456, Zephyr shell on 3457
docker run --rm -it -v "$PWD":/workspace rpmsg-poc ./scripts/build_and_run.sh build        # build only
```

[scripts/build_and_run.sh](scripts/build_and_run.sh) creates the west workspace in `zephyrproject/` (Zephyr `v4.5.0-rc1`, only the open-amp, libmetal and cmsis modules), builds the Zephyr ELF for `qemu_cortex_r5`, statically links the userspace app, merges the DT overlay into Antmicro's OpenAMP DTB with `fdtoverlay`, and copies Antmicro's OpenAMP rootfs into `build/rootfs.ext2` with `debugfs`. The rootfs gets the app at `/root/am64_rpmsg_userspace` and an init script that runs `modprobe uio_pdrv_genirq of_id=generic-uio`.

The acceptance test is [renode/rpmsg_poc.robot](renode/rpmsg_poc.robot) (`renode-test renode/rpmsg_poc.robot`). It has three test cases, each on a fresh boot (`Test Setup  Reset Emulation`). The first runs `am64_rpmsg_userspace ping` (CLI mode). The second runs an interactive session: a Linux→Zephyr message, `rpmsg send` from the Zephyr shell, ping/pong, then `quit`. The third sends a 1 MB file with `/bulk` and a 2 MB pattern with `rpmsg bulk 2M`. Together they take about 100 s. The app does one handshake per boot: rerunning it without restarting the simulation won't work, because the R5F doesn't handle a vdev reset.

`zephyrproject/` is the west workspace. Don't edit it. `scripts/experiments/test_dpkg.sh` and `test_multiarch.sh` are throwaway experiments from getting arm64 libsysfs into the container; they are not tests.

## Architecture

The parts only work if they agree on one memory map and IPI assignment, which are spread across several files. **Change all of them together:**

| Item | Address | Defined in |
|---|---|---|
| R5F (Zephyr) RAM, 1 MB, outside Linux's DDR | `0xA0000000` | `renode/am64_zynqmp.repl`, `zephyr_app/app.overlay` |
| Resource table, 4 KB | `0xA0100000` | `renode/am64_zynqmp.repl`, `zephyr_app/app.overlay`, `linux_app/am64_rpmsg_overlay.dts`, `linux_app/am64_rpmsg_userspace.c` (UIO name) |
| Shared SRAM, 1 MB | `0xA5000000` | `renode/am64_zynqmp.repl`, `zephyr_app/app.overlay`, `linux_app/am64_rpmsg_overlay.dts`, `linux_app/am64_rpmsg_userspace.c` (UIO name) |
| VRING0 / VRING1 (4K aligned, 256 descs), buffers | `+0x0` / `+0x4000`, buffers from `+0x8000` | `zephyr_app/src/rsc_table.c`, `linux_app/am64_rpmsg_userspace.c` (`SHM_BUF_OFFSET`) |
| APU IPI channel 7 (Linux side, UIO) | `0xFF340000`, bit 24, SPI 29 | `linux_app/am64_rpmsg_overlay.dts`, `zephyr_app/app.overlay` (`xlnx,ipi-id = <24>`) |
| Bulk region, 16 MB (A53→R5F area `+0x0`, R5F→A53 area `+0x800000`, 8 MB each) | `0xA8000000` | `renode/am64_zynqmp.repl`, `zephyr_app/app.overlay`, `linux_app/am64_rpmsg_overlay.dts`, `linux_app/am64_rpmsg_userspace.c` (UIO name), `common/rpmsg_bulk.h` |
| RPU0 IPI channel 1 (Zephyr side) | `0xFF310000`, bit 8 (`0x100`), SPI 33 | `zephyr_app/app.overlay`, `linux_app/am64_rpmsg_userspace.c` (`IPI_RPU0_MASK`) |

- **Renode platform** ([renode/am64_zynqmp.repl](renode/am64_zynqmp.repl)): Renode's stock `platforms/cpus/zynqmp.repl` plus three `MappedMemory` regions (R5F RAM, resource table, shared SRAM). The IPI is the stock `sysbus.ipi`.
- **Simulation script** ([renode/run_poc.resc](renode/run_poc.resc)): follows Antmicro's `scripts/single-node/zynqmp_openamp.resc` (ATF, U-Boot and Linux 6.6.10 Image, downloaded from dl.antmicro.com). It adds our DTB and rootfs and loads the Zephyr ELF on `rpu0`. Linux console is uart1, Zephyr console is uart0. It also installs a `SetHookBeforePeripheralWrite` hook on `ipi`. Renode's `ZynqMP_IPI` doesn't implement write-1-to-clear on ISR registers, and Zephyr's driver writes `0xFFFFFFFF` to ISR at init, which without the hook causes an interrupt storm on the first kick. Keep the hook.
- **Handshake**: Zephyr copies its resource table to `0xA0100000` at boot (nothing loads it through remoteproc, so the table isn't read from the ELF) and polls the vdev status. The Linux app waits for the table, creates the vdev as `VIRTIO_DEV_DRIVER` (sets `DRIVER_OK`), and waits for the name-service announcement of `rpmsg-client-sample`. It then binds an endpoint and sends a **zero-length connect message**. The A53 never announces its own endpoint, so Zephyr only learns the A53's address (`ept->dest_addr`) from the first message it receives. Zero-length messages are therefore reserved and never carry user data. After that, the app runs CLI mode (each argv is one message, exits after 2 s idle) or interactive mode (stdin lines, `quit`/EOF). Payloads are plain text, at most 496 bytes (`MAX_PAYLOAD` on both sides). Each side kicks the other through the IPI and processes all vrings (`RSC_NOTIFY_ID_ANY`) on each kick.
- **Linux side**: the overlay disables the kernel's `zynqmp_ipi1` mailbox and `rf5ss` remoteproc nodes (they would claim IPI channel 7) and uart0, and adds `generic-uio` nodes. libmetal opens them as `a0100000.rsc_table`, `a5000000.shm` and `ff340000.ipi`. The rootfs ships `uio_pdrv_genirq.ko` matching the kernel. The base DTB/rootfs URLs are pinned in `build_and_run.sh`. Antmicro has republished these files under new hashes before, so recheck the URLs if a download 404s.
- **Zephyr side**: `qemu_cortex_r5` is the ZynqMP RPU board. `app.overlay` moves SRAM to `0xA0000000` and declares the IPI with the generic `xlnx,mbox-versal-ipi-mailbox` MBOX driver (its register layout matches ZynqMP). It uses bufferless signalling only. `main.c` uses raw OpenAMP (`VIRTIO_DEV_DEVICE`), not the IPC service. It auto-replies only to an exact `ping` and registers the shell command `rpmsg send <text...>` (shell on uart0), which calls `rpmsg_send` from the shell thread while `main` processes kicks, and `rsc_table.c` holds the resource table (fixed vring addresses, `VIRTIO_RPMSG_F_NS`).
- **Bulk channel** ([common/rpmsg_bulk.h](common/rpmsg_bulk.h)): Zephyr announces a second endpoint, `rpmsg-bulk`, and the A53 binds and connects it like the text endpoint. The sender writes the payload into its own area of the bulk region and sends a 20-byte `struct rpmsg_bulk_msg` (`XFER`: id, offset, len, CRC32). The receiver checks the CRC in place (the R5F has only 1 MB of RAM, so it never copies) and answers `RELEASE`. One transfer per direction is in flight at a time. Linux: `--bulk <file>` (CLI) or `/bulk <file>` (interactive). Zephyr: `rpmsg bulk <n>[K|M]` sends a test pattern. The UIO mapping is uncached device memory, so the Linux app only touches it through `metal_io_block_read/write`. On real AM64x hardware, the R5F's MPU must map the region non-cacheable, or the code must do cache maintenance. Renode doesn't model caches.
- The Zephyr module ships open-amp `v2026.04`, while the A53 app links `v2024.05.0`. The wire format (resource table, vrings, RPMsg header) is compatible between them.
