# rpmsg-poc

Proof of concept for an OpenAMP/RPMsg link between an application core and a real-time core, aimed at the **TI AM6442** SoC:

- **Cortex-A53**: Linux, with the RPMsg host running in **userspace** (libmetal + OpenAMP over UIO)
- **Cortex-R5F**: Zephyr RTOS, as the RPMsg remote

Everything runs in the [Renode](https://renode.io) simulator. Renode has no AM64x model, so a **Xilinx ZynqMP** platform stands in for it, which also has Cortex-A53 and Cortex-R5F cores. The ZynqMP **IPI** (inter-processor interrupt) block stands in for the AM64x hardware mailbox.

The design spec is [docs/antigravity_agent_spec_am64x_rpmsg_poc.md](docs/antigravity_agent_spec_am64x_rpmsg_poc.md).

## What the POC does

1. Zephyr boots on the R5F, publishes its resource table to a fixed shared-memory address and waits.
2. Linux boots on the A53. The userspace app `am64_rpmsg_userspace` maps the resource table, the shared SRAM and the IPI registers through UIO. It sets up the vrings and buffers and signals the R5F that they're ready.
3. Zephyr announces the `rpmsg-client-sample` endpoint. The app binds to it and sends `"ping"`.
4. Zephyr prints `OpenAMP: Received message: "ping"` and replies `"pong"`, which the app prints.

Expected console output:

```
# Linux (uart1)
am64_rpmsg_userspace: sent "ping"
am64_rpmsg_userspace: received "pong"

# Zephyr (uart0)
OpenAMP: Received message: "ping"
OpenAMP: Sent reply: "pong"
```

## Requirements

- Docker or Podman
- About 30 GB of disk for the image, which is based on `zephyrprojectrtos/ci`
- Network access for the first build (Zephyr sources, plus Antmicro's prebuilt ZynqMP firmware, kernel and rootfs)

The container image provides everything else: Renode, the Zephyr SDK, the AArch64 Linux cross compiler and static builds of libmetal and OpenAMP.

## Quick start

```bash
docker build -t rpmsg-poc .
docker run --rm -it -v "$PWD":/workspace rpmsg-poc ./scripts/build_and_run.sh
```

With Podman, use `podman` in place of `docker`. On SELinux hosts, add `:Z` to the volume (`-v "$PWD":/workspace:Z`).

This builds everything and runs the automated acceptance test ([renode/rpmsg_poc.robot](renode/rpmsg_poc.robot)). The first run takes a while because it fetches the Zephyr workspace. After that, the test itself takes under a minute. Results land in `build/test-results/` (`log.html` has the full UART transcripts).

### Modes

`scripts/build_and_run.sh [mode]`:

| Mode | What it does |
|---|---|
| `test` (default) | Build, then run the headless acceptance test |
| `interactive` | Build, then open a Renode console with the simulation running |
| `build` | Build only |

### Running it by hand

```bash
docker run --rm -it -p 3456:3456 -v "$PWD":/workspace rpmsg-poc ./scripts/build_and_run.sh interactive
```

The Renode monitor runs in that terminal, and its log shows both UARTs: `uart1` is Linux and `uart0` is Zephyr. The Linux console is also served on TCP port 3456. Once the log shows `Machine started`, connect to it from a second terminal:

```bash
telnet localhost 3456        # or: nc localhost 3456
```

At `buildroot login:`, log in as `root` (no password) and run:

```sh
/root/am64_rpmsg_userspace
```

The Zephyr side of the exchange appears as `uart0` lines in the Renode log. Type `quit` in the Renode monitor to exit.

Notes:
- Type commands rather than pasting them. The simulated UART drops characters when a whole line arrives in one burst.
- The app does one exchange per simulation boot, so restart the simulation to run it again.

## How it is built

`scripts/build_and_run.sh` runs these steps:

1. Creates a Zephyr west workspace in `zephyrproject/` (Zephyr `v4.5.0-rc1`, only the modules needed).
2. Builds the R5F firmware from `zephyr_app/` for the `qemu_cortex_r5` board, which is the ZynqMP RPU.
3. Cross-compiles `linux_app/am64_rpmsg_userspace.c` as a static AArch64 binary.
4. Merges `linux_app/am64_rpmsg_overlay.dts` into Antmicro's ZynqMP OpenAMP device tree with `fdtoverlay`, producing `build/am64_rpmsg_poc.dtb`.
5. Copies Antmicro's OpenAMP rootfs to `build/rootfs.ext2`, adds the app and a boot script that loads `uio_pdrv_genirq`.
6. Runs Renode with `renode/run_poc.resc`.

## Memory map

| Item | Address |
|---|---|
| R5F (Zephyr) RAM, 1 MB | `0xA0000000` |
| Resource table, 4 KB | `0xA0100000` |
| Shared SRAM, 1 MB (VRING0 at `+0x0`, VRING1 at `+0x4000`, buffers from `+0x8000`) | `0xA5000000` |
| A53 IPI channel 7 (UIO on Linux) | `0xFF340000`, SPI 29 |
| R5F IPI channel 1 | `0xFF310000`, SPI 33 |

These values are spread across the Renode platform, both device trees and both applications, so change them together. [CLAUDE.md](CLAUDE.md) lists exactly where each one is defined.

## Repository layout

```
docs/        design spec
linux_app/   A53 userspace RPMsg host and Linux device tree overlay
zephyr_app/  R5F Zephyr firmware (OpenAMP remote, resource table, IPI mailbox)
renode/      platform description, simulation script, acceptance test
scripts/     build_and_run.sh; experiments/ holds throwaway scripts
Dockerfile   build/run environment
```

## Limitations and notes

- **Simulated stand-in, not AM64x hardware.** The ZynqMP IPI replaces the TI mailbox, and the addresses are chosen for the ZynqMP platform. Porting to a real AM6442 means swapping the doorbell (TI mailbox) and the memory map.
- **No remoteproc loading.** Renode loads the R5F firmware directly, and Zephyr copies its resource table to a fixed address that Linux maps.
- **One exchange per boot.** The Zephyr side doesn't handle a virtio reset.
- **Renode IPI quirk.** `renode/run_poc.resc` installs a hook because Renode's `ZynqMP_IPI` model doesn't implement write-1-to-clear on its status registers. Without it, the R5F gets stuck in an interrupt storm.
- **External images.** The Linux kernel, firmware and rootfs are Antmicro's prebuilt ZynqMP OpenAMP demo images, downloaded from `dl.antmicro.com`.
