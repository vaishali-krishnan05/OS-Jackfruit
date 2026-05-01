# OS-Jackfruit
### Multi-Container Runtime with Kernel-Space Memory Monitor

A lightweight Linux container runtime written in C, featuring a long-running supervisor process, per-container namespace isolation, a kernel loadable module for memory monitoring, and CFS scheduler experiments.

---

## Project Structure

```
OS-Jackfruit/
├── boilerplate/
│   ├── engine.c           # User-space supervisor & CLI
│   ├── monitor.c          # Kernel module (memory monitor via ioctl)
│   ├── monitor_ioctl.h    # Shared ioctl command definitions
│   ├── cpu_hog.c          # CPU-bound workload (16-thread contention)
│   ├── io_pulse.c         # I/O-bound workload
│   ├── memory_hog.c       # Memory-consuming workload
│   ├── Makefile           # Build targets for user-space + kernel module
│   └── environment-check.sh
├── Screenshots/
├── .github/workflows/     # CI smoke check
├── project-guide.md
└── README.md
```

---

## Setup & Build

### Prerequisites
- Ubuntu 22.04 or 24.04 (bare metal or VirtualBox VM)
- Secure Boot must be OFF — required for loading the kernel module
- WSL is not supported

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Environment Check
```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

### Prepare Root Filesystem (Alpine)
```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

> Do not commit `rootfs-base/` or `rootfs-*` directories.

### Build
```bash
cd boilerplate
make
```

For CI (no kernel module):
```bash
make -C boilerplate ci
```

---

## Usage

### Starting the Supervisor
```bash
sudo ./engine
```

### CLI Commands
```
create <id> <rootfs_path>     Create a new container
start <id>                    Start a container
stop <id>                     Stop a running container
list                          List all containers and their status
logs <id>                     View container logs
stats <id>                    Show memory stats via kernel module
destroy <id>                  Destroy a stopped container
exit                          Shut down the supervisor
```

> Note: Exited container IDs persist in the supervisor's state until it is restarted. Always use `destroy` before attempting to reuse an ID.

---

## Implementation Details

### Container Isolation
Each container gets its own set of Linux namespaces:
- PID namespace — isolated process tree
- Mount namespace — private filesystem view via `pivot_root`
- UTS namespace — per-container hostname
- Network namespace (partial) — scoped network view

### Kernel Module (monitor.c)
- Loadable kernel module exposing memory stats via `ioctl`
- Uses a kernel timer to periodically sample per-container RSS
- PID namespace fix: resolved a critical bug where `find_vpid()` in timer callbacks resolved PIDs in the wrong namespace. Fixed using `find_get_pid()` + `kill_pid()` to operate on global PIDs correctly.
- Kernel 6.x compatibility: updated deprecated `del_timer_sync` to `timer_delete_sync` and added `CONTAINER_ID_MAX` guards.

### Supervisor (engine.c)
- Long-running daemon managing container lifecycle
- Communicates with the kernel module via `ioctl` on `/dev/monitor`
- Logs events with timestamps to per-container log files

---

## Scheduler Experiments

Experiments were run to observe CFS (Completely Fair Scheduler) behavior across different workload types.

### Methodology
Used a `ps`-in-loop approach to sample `vruntime` and CPU time, replacing `pidstat` which silently fails on kernel 6.17.

### Experiment 1 — CPU-bound vs I/O-bound
- `cpu_hog` (16 threads, 6-core i5) and `io_pulse` run concurrently
- Observation: CFS vruntime converges for CPU-bound threads; I/O-bound processes frequently block and yield, accumulating less vruntime and being prioritized on wake-up

### Experiment 2 — Nice Value Differential
- Two `cpu_hog` instances: one at `nice 0`, one at `nice +10`
- Observation: The lower-nice process receives roughly 3x more CPU time, confirming CFS weight-proportional scheduling via the `sched_prio_to_weight` table

### Key Takeaway
CFS does not enforce strict time slices. It tracks virtual runtime and always schedules the task with the smallest vruntime, making it work-conserving and fair over time.

---

## Notable Bugs Fixed

| Bug | Root Cause | Fix |
|---|---|---|
| Monitor ioctl returning stale/zero data | `find_vpid()` resolving in wrong PID namespace inside timer callback | Switched to `find_get_pid()` + `kill_pid()` with global PID |
| Kernel module build failure on 6.x | `del_timer_sync` removed in kernel 6.17 | Replaced with `timer_delete_sync` |
| engine CLI silent crash | Stripped client-side code missing argument validation | Reconstructed with full bounds checking and `CONTAINER_ID_MAX` guard |
| `pidstat` not reporting stats | Binary silently unsupported on kernel 6.17 | Replaced with `ps -o pid,cputime,etimes` loop |

---

## Screenshots

See the `Screenshots/` directory for:
- Container lifecycle output (`create`, `start`, `stop`, `list`)
- Memory stats from kernel module via `stats <id>`
- CFS vruntime plots from scheduler experiments

---

## Design Decisions

- Alpine minirootfs was chosen as the container filesystem for its minimal size (~3 MB) and suitability for namespace-isolated environments.
- ioctl over procfs was chosen for kernel-to-user communication to allow structured, typed data exchange and avoid string parsing overhead.
- 16-thread `cpu_hog` was designed specifically to saturate a 6-core i5 and force CFS contention, making scheduler behavior measurable.
- `ps`-loop sampling replaced `pidstat` after confirming the latter silently fails on kernel 6.17 without returning an error.

---

## CI

GitHub Actions runs a smoke check on every push:
- Compiles user-space binaries (`engine`, `cpu_hog`, `io_pulse`, `memory_hog`)
- Verifies `./engine` with no arguments prints usage and exits non-zero
- Does not test kernel module loading (requires bare-metal or VM)

---

## Requirements Met

- Multi-container lifecycle management (create / start / stop / destroy)
- PID, mount, UTS namespace isolation
- Long-running supervisor with persistent state
- Kernel LKM with ioctl-based memory monitoring
- Per-container logging
- CFS scheduler experiments with analysis
- CI smoke check via GitHub Actions

---

## Authors

**Vaishali Krishnan- PES1UG24AM313**
**Samyuktha Vadrevu- PES1UG24AM316**
Btech Computer Science & Engineering (AI/ML), PES University
