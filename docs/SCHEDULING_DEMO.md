# Scheduling demonstration

This demonstration runs a finite workload in the NUCLEO-F401RE firmware. It
uses the AymOS EDF scheduler, SVC, PendSV, PSP, SysTick, and version 1
structured trace. All scheduling decisions come from the firmware.

## Run the demonstration

Install the pinned tools once:

```sh
make setup
```

Run both workload modes:

```sh
make demo
```

The command builds and runs two ELF files. It validates each complete trace and
then creates this report structure:

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

Open `index.html` or either `timeline.html` in a browser. The HTML files do not
need a network connection or a server.

Each mode also stores the exact command, UART capture, validation output, and
Renode run metadata. `metadata.json` records a SHA-256 hash for each report
input and output. The root `summary.json` combines the metrics from both modes.

## Workload

The firmware creates four application tasks:

| Task | ID | Release pattern | Relative deadline | Work demand | Jobs |
| --- | ---: | --- | ---: | ---: | ---: |
| sampler | 1 | period 6, first release 0 | 3 | 1 tick | 3 |
| controller | 2 | period 10, first release 1 | 8 | 2 ticks | 2 |
| telemetry | 3 | period 18, first release 2 | 14 | 1 tick | 2 |
| load | 4 | one release at 0 | 12 | 1 or 8 ticks | 1 |

Normal mode gives the load task one work tick. All eight jobs meet their
deadlines. Overload mode gives the same task eight work ticks. The sampler
preempts it, and the load task records the first deadline miss at tick 12. All
other configuration stays the same.

Each task performs deterministic integer work. The kernel accounts for work
only while that task is running. Time spent preempted does not count as task
work.

## Trace and report checks

The validator requires these facts:

- all eight jobs release at their configured ticks;
- periodic tasks use `os_wait_next_period()`;
- tasks use the kernel entry and exit path;
- running intervals match the configured work demand;
- the trace contains idle execution and a committed preemption;
- normal mode has no deadline miss;
- overload mode has one first miss for task 4 at tick 12;
- emitted EDF candidate data explains the selection before that miss; and
- the trace ends at tick 21 without a lost record.

The timeline derives running intervals from committed context-switch events.
It shows releases, execution, idle time, preemptions, absolute deadlines, and
deadline misses. Selection explanations use the candidate deadlines,
priorities, and reason that the kernel emitted.

## Build one mode

Use one of these commands when you only need a firmware image:

```sh
make workload WORKLOAD_MODE=normal
make workload WORKLOAD_MODE=overload
```

The images are under `build/nucleo_f401re/deadline_lab/<mode>/`. The
`deadline_lab` name is an internal build identifier kept for command
compatibility.

## Result boundary

The report shows firmware event order, scheduler state, and guest ticks in
Renode. Use measurements from the target board when you need physical time,
interrupt latency, or worst-case execution time.
