# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Proof of concept for an OpenAMP/RPMsg link between a Cortex-A53 (Linux, master) and a Cortex-R5F (Zephyr, remote), aimed at the TI AM6442 SoC and simulated entirely in **Renode**. Renode has no AM64x model, so a **ZynqMP platform stands in** for it, with a Python peripheral mocking the TI mailbox. The design spec that drives this work is [antigravity_agent_spec_am64x_rpmsg_poc.md](antigravity_agent_spec_am64x_rpmsg_poc.md); read it before making structural changes. Its acceptance criterion: the Linux console shows the userspace app sending `"ping"` and the Zephyr UART prints `OpenAMP: Received message: "ping"`.

The A53 side was moved from a kernel module to a **userspace app** (libmetal + libopen_amp over `generic-uio`). Don't reintroduce a kernel driver (`am64_rpmsg_client.c` was deleted on purpose).

## Build & run

Everything is meant to run inside the Docker image built from [Dockerfile](Dockerfile). It is based on `zephyrprojectrtos/ci`, adds Renode 1.15.3 at `/opt/renode`, and cross-compiles libsysfs, libmetal and open-amp (`v2024.05.0`) into the `/usr/aarch64-linux-gnu` sysroot.

```bash
docker build -t rpmsg-poc .
docker run --rm -it -v "$PWD":/workspace rpmsg-poc ./build_and_run.sh
```

[build_and_run.sh](build_and_run.sh) runs these steps, which you can also run on their own:

```bash
# Zephyr R5F firmware (run from zephyrproject/; west workspace, Zephyr 4.5.0-rc1)
cd zephyrproject && west build -p always -b qemu_cortex_r5 ../zephyr_app -d ../zephyr_app/build

# A53 userspace app (needs the cross-compiled libs from the Docker image)
aarch64-linux-gnu-gcc -O2 -o am64_rpmsg_userspace am64_rpmsg_userspace.c -lopen_amp -lmetal

# Renode, headless
renode -e "s @run_poc.resc; start" --disable-x11
```

`zephyrproject/` is the west workspace and is owned by root because it was created inside the container. Don't edit it. `test_dpkg.sh` and `test_multiarch.sh` are throwaway experiments for getting arm64 libsysfs into the container; they are not tests. The repo has no unit tests or linter.

## Architecture

The parts only work if they agree on one memory map, which is spread across several files. **Change all of them together:**

| Item | Address | Defined in |
|---|---|---|
| Shared SRAM (vrings + buffers), 1 MB | `0xA5000000` | `am64_zynqmp.repl`, `am64_rpmsg_overlay.dts`, `zephyr_app/app.overlay`, `zephyr_app/src/rsc_table.c` |
| VRING0 / VRING1 (4K aligned, 256 entries) | `0xA5000000` / `0xA5004000` | `rsc_table.c`, `am64_rpmsg_overlay.dts` (reserved-memory) |
| Resource table | `0xA0100000` | `am64_zynqmp.repl` |
| Mock mailbox (UIO on Linux) | `0x2A000000`, IRQ 100 → `rpu0` | `am64_zynqmp.repl`, `am64_rpmsg_overlay.dts` |

- **Renode platform** ([am64_zynqmp.repl](am64_zynqmp.repl)): extends Renode's stock `platforms/cpus/zynqmp.repl` with the shared SRAM, the resource-table RAM and the mailbox peripheral.
- **Mailbox mock** ([ti_mailbox_mock.py](ti_mailbox_mock.py)): a Renode `PythonPeripheral` script, not a normal Python module. It uses Renode's `request`/`self` globals. Writing TX (offset `0x0`) raises the R5F IRQ. Reading or writing RX (`0x4`) clears it.
- **Simulation script** ([run_poc.resc](run_poc.resc)): boots Antmicro's prebuilt ZynqMP Linux kernel and rootfs (downloaded from URLs) with bootargs `uio_pdrv_genirq.of_id="generic-uio"` so UIO binds to the overlay nodes.
- **Linux side**: [am64_rpmsg_overlay.dts](am64_rpmsg_overlay.dts) exposes the SRAM and mailbox as `generic-uio` nodes. [am64_rpmsg_userspace.c](am64_rpmsg_userspace.c) opens them with `metal_device_open("platform", "a5000000.uio_sram" / "2a000000.mailbox")`.
- **Zephyr side** ([zephyr_app/](zephyr_app/)): `rsc_table.c` places the resource table in the `.resource_table` section. `app.overlay` points `zephyr,ipc_shm` at the shared SRAM and `zephyr,ipc` at `rpu0_ipi`. `prj.conf` enables the IPC service with the RPMsg backend (remote mode) and `XLNX_IPI`.

## Known gaps (as of the initial scaffolding)

The POC is not wired end to end yet. Check these before assuming something works:
- `am64_rpmsg_userspace.c` skips the vring/remoteproc setup and calls `ns_bind_cb(NULL, ...)` directly, so `rpmsg_create_ept` gets a NULL `rdev`.
- Zephyr `main.c` never initializes IPC or OpenAMP. It only sleeps, so `endpoint_cb` is never registered.
- The build targets the `qemu_cortex_r5` board, but `app.overlay` refers to `rpu0_ipi`, a ZynqMP node.
- `run_poc.resc` doesn't load the Zephyr ELF onto `rpu0`, the DT overlay/DTB, or the userspace binary yet (the spec asks for all three). Its `reset` macro is a placeholder.
