# Deadline Lab

Deadline Lab runs a finite workload in the board-targeted Cortex-M4 firmware.
It uses the AymOS EDF scheduler, SVC, PendSV, PSP, SysTick, and the version 1
trace. The host does not calculate a substitute schedule.

## Run the lab

Install the locked tools once:

```sh
make setup
```

Run both modes:

```sh
make demo
```

The command builds two ELF files. It runs each ELF in pinned Renode. It checks
the complete trace. It then creates one report directory:

```text
runs/<run-id>/
  index.html
  normal/
    metadata.json
    workload.yaml
    firmware.elf
    firmware.map
    build-metadata.txt
    trace.bin
    trace.json
    summary.json
    timeline.html
    emulator.log
  overload/
    ...same files...
```

Open `index.html` or either `timeline.html` in a browser. The HTML files have no
network or server dependency.

Each mode also keeps `command.txt`, the run harness `metadata.txt`, the full
`uart.bin`, the validator output in `uart.txt` and `uart-validation.log`, and
the Renode log. `metadata.json` hashes all of these files plus the required
files above, except itself. The run root adds a combined `summary.json`, a small
completion marker, and `index.html`. Temporary HOME and XDG directories are
removed after a successful run. They remain after a failed run for diagnosis.

## Workload

The firmware creates four user tasks:

| Task | ID | Release pattern | Relative deadline | Execution demand | Jobs |
| --- | ---: | --- | ---: | ---: | ---: |
| sampler | 1 | period 6, first release 0 | 3 | 1 tick | 3 |
| controller | 2 | period 10, first release 1 | 8 | 2 ticks | 2 |
| telemetry | 3 | period 18, first release 2 | 14 | 1 tick | 2 |
| load | 4 | one release at 0 | 12 | 1 or 8 ticks | 1 |

Normal mode gives the load task one execution tick. All eight jobs meet their
deadlines. Overload mode gives the same load task eight execution ticks. The
load task is preempted by the sampler. It then reaches its absolute deadline at
tick 12 and records the first miss. All other configuration stays the same.

Each task performs a small deterministic integer workload. The execution loop
uses the kernel's running-task accounting. Time spent preempted does not count
as load execution.

## Trace and report rules

The validator requires these facts:

- all eight configured releases occur at their exact ticks;
- each periodic task uses `os_wait_next_period()`;
- every task starts and exits through the real kernel lifecycle;
- observed running intervals match the configured execution demand;
- the trace has a positive idle interval and a committed preemption;
- normal mode has no miss;
- overload mode has one first miss for task 4 at tick 12;
- the overload trace contains the earlier emitted selection that explains why
  task 4 ran; and
- the trace closes at tick 21 with no loss and fewer than 256 records.

The timeline derives running intervals only from committed context-switch
events. It shows releases, running intervals, idle intervals, preemptions,
absolute deadlines, and misses. Selection explanations use the emitted
candidate deadlines, priorities, and reason field.

The run ID contains UTC time, process ID, and random bytes to prevent a path
collision. The random bytes do not affect the firmware workload. The workload
has no random seed. The tool rejects a runs-root symlink. It keeps at most eight
marked completed or failed Deadline Lab runs. It does not delete an unmarked,
symlinked, or active run directory.

The demo fixes the Renode virtual duration at 0.5 seconds. It fixes the Renode
host timeout at 20 seconds and the outer collector timeout at 45 seconds. It
does not inherit alternative timeout values. If the outer limit expires, the
collector terminates the run-harness process group. Metadata records the
limits.

## Build one mode

Use these commands when you only need an ELF:

```sh
make deadline-lab WORKLOAD_MODE=normal
make deadline-lab WORKLOAD_MODE=overload
```

The output paths are:

```text
build/nucleo_f401re/deadline_lab/normal/
build/nucleo_f401re/deadline_lab/overload/
```

## Measurement boundary

The reports show guest ticks, event order, and observed scheduling behavior in
Renode. They do not prove physical execution time, interrupt latency, worst-case
execution time, clock accuracy, or a hard real-time guarantee. Physical board
timing remains unverified.
