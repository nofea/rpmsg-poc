# Antigravity Agent Orchestration Spec: AM64x RPMsg Stack in Renode (Userspace Pivot)

## 1. Project Objective

Implement and verify an Asymmetric Multiprocessing (AMP) RPMsg stack using the OpenAMP framework. The target hardware is the TI AM6442 SoC (simulated via ZynqMP platform stand-in). The simulation must run entirely within **Renode**.

**Stack Requirements:**
* **Master Core:** Cortex-A53 running a minimal generic Linux OS.
* **Remote Core:** Cortex-R5F running Zephyr RTOS.
* **IPC Mechanism:** OpenAMP (vrings + shared SRAM) operating in **Linux Userspace** via `libmetal` and `libopen_amp`.
* **Hardware Trigger:** Mocked TI hardware mailbox bound to the generic Linux UIO driver.

## 2. Agent Workflow & Execution Phases

### Phase 1: Workspace & Toolchain Refactoring

**Agent Task:** Modify the existing scaffolding to replace the A53 kernel module with a userspace application.

1. **Zephyr RTOS (R5F):**
   * Maintain the existing Zephyr workspace (`zephyr_app/`).
   * Ensure `main.c` listens for the RPMsg payload and `rsc_table.c` maps the memory accurately.

2. **Linux (A53) Userspace App:**
   * Delete `am64_rpmsg_client.c`.
   * Create `am64_rpmsg_userspace.c`. This must be a standard C application that uses `libmetal` and `libopen_amp` to initialize the vrings from shared memory and send a `"ping"` message to the remote core.
   * Update `am64_rpmsg_overlay.dts`. Instead of a custom kernel driver, define the shared SRAM and the mocked mailbox registers as nodes compatible with `"generic-uio"` (using the `uio_pdrv_genirq` driver).

3. **Docker Environment Update:**
   * Modify the `Dockerfile` to download/cross-compile `libmetal` and `libopen_amp` for the AArch64 target so the userspace application can link against them during the build phase.

### Phase 2: Memory Mapping (Unchanged)

**Agent Task:** Ensure both cores agree on the physical addresses.
* **Shared SRAM Base Address:** `0xA5000000`
* **Size:** `0x100000` (1MB)
* **Resource Table Offset:** `0xA0100000`
* **VRING0 & VRING1:** 4K aligned.

### Phase 3: Renode Simulation Configuration

**Agent Task:** Update the simulation scripts to accommodate the userspace execution.

1. **`am64_zynqmp.repl` & `ti_mailbox_mock.py`:**
   * Keep the existing `.repl` memory map and Python mailbox mock. The mock still triggers the R5F interrupt upon a TX register write.

2. **`run_poc.resc` (Startup Script):**
   * Load a pre-compiled generic ARM64 Linux kernel (`Image`) and a minimal rootfs (e.g., ext2/cpio).
   * Ensure the bootargs string includes `uio_pdrv_genirq.of_id="generic-uio"` to force the UIO driver to bind to the device tree overlay nodes.
   * Ensure the script loads the custom DTB and the Zephyr ELF into memory.

### Phase 4: Autonomous Build & Verification

**Agent Task:** Execute the build and simulation in the Docker container.

1. **Build:** 
   * Compile the Zephyr RTOS binary.
   * Cross-compile `am64_rpmsg_userspace.c` (linking `-lmetal` and `-lopen_amp`).

2. **Simulate:** 
   * Launch Renode headless via `build_and_run.sh`.
   * Inside the simulated Linux environment, run the compiled userspace binary (e.g., `./am64_rpmsg_userspace`).

3. **Validate (Acceptance Criteria):**
   * Monitor the UART outputs.
   * *Success:* The Linux console shows the userspace app sending the ping, and the Zephyr console prints `OpenAMP: Received message: "ping"`.

## 3. Constraints & Fallbacks
* **UIO Mapping:** The A53 userspace code must explicitly use `metal_device_open()` to map the UIO device exposed by the Linux kernel (typically `/dev/uio0`).
* **Rootfs Injection:** To avoid building a full Linux image, instruct Renode to inject the compiled `am64_rpmsg_userspace` binary directly into the virtual guest's root filesystem (using Renode's `sysbus LoadELF` or guest agent transfer mechanisms) before execution.