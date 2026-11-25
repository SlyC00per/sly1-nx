# Runs on a Switch with nobody at it

The homebrew menu takes a build over the network (nxlink) only after Y is pressed, once a run.
The receiver removes that: a small program that waits for a build, starts it, and is started
again by the build when its run ends. One start by hand, then any number of runs.

## Using it

Once, with the homebrew menu waiting (Y):

    nxlink -a ADDRESS build-switch/sly1_receiver.nro

It copies itself to `sdmc:/switch/sly1_receiver/`, where the menu lists it as "sly1-nx
receiver" from then on, and shows the address it waits at. + leaves it.

Then, from the Mac, as often as wanted:

    python3 tools/switch_run.py build-switch/sly1.nro LOG SECONDS "ARGS"
    python3 tools/bench.py run NAME --receiver

`switch_run.py` sends the build with `SLY1_NEXT` (the receiver's path: the program to start
when the run ends) and `SLY1_DEADLINE` (the run ends itself, status 4, after SECONDS), and
writes what the build prints to LOG. A new receiver is put there the same way, through the one
waiting: `switch_run.py build-switch/sly1_receiver.nro /dev/null 5`.

The Switch on its charger, auto-sleep off.

## What a run's end has to do (port/host/main.cpp, HandOver)

The loader starts the next program in the same process, with the same heap. Everything the
build holds of the system has to be given back first, in order, by the first thread:

1. The mixer ends and closes the audio output itself.
2. The GS thread stops between two items (`ParkGsThread`); the renderer destroys deko3d's
   command buffer, swapchain, queue, memory blocks and device (`gpu::Shutdown`), which closes
   the GPU's services.
3. Every other thread ends itself (`EndOtherThreads`, below); one that does not within 3 s is
   named in the log and stopped for good.
4. Each thread's stack mapping is taken away, by the size the kernel names for it, and its
   handle closed.
5. The link to the Mac is closed, and the first thread returns to the loader.

A fault ends the run the same way (status 3), and so does a start that fails.

## Threads that end themselves (port/platform/thread.cpp)

A thread cannot be ended from outside, and one only stopped stays in the process. The three
calls a thread waits in are wrapped at the link (`-Wl,--wrap=`): `svcSleepThread`,
`svcWaitSynchronization` and `condvarWaitTimeout` (every `std::condition_variable` and
semaphore). Once the end is asked for, a thread that reaches one of them or wakes from one ends
there; waits longer than 50 ms are cut into slices; the condition variables threads are
waiting on at that moment are signalled. The first thread never ends this way.

## What went wrong on the way (2026-11-24 and 25, all on hardware)

| What was done | What happened |
| --- | --- |
| the build's threads left running | the receiver faulted at once: "the software closed because an error occurred" |
| the threads stopped, nothing given back | the receiver ran; the second build brought the whole system down |
| the GPU's session closed from outside (`nvExit`) with the render thread stopped | hung there |
| stacks unmapped by `stack_sz`, or by libnx's `threadClose` | the mappings stayed; the next build faulted in `nvInitialize` on memory still lent out |
| threads stopped, never ended | the tenth build in the process could not make its threads |
| every condition variable ever waited on signalled | the kernel writes 0 to one it signals: half a pointer on the first thread's stack zeroed |
| a thread's record read at the end | a detached thread's is freed when it ends: the handle and stack are copied when it is made |

`HeapReport` (the "heapmap" lines) says the parts of the heap that are not plain memory: one
at the start (the loader's) and one at the end is a clean run. `SLY1_HEAP_MAP=1` lists them.

## Measured

2026-11-25, one process: 26 builds in a row, among them one past its deadline, one with a fault
made on purpose (`SLY1_FAULT_AFTER`), a normal run after each, and the five-level benchmark
(57.0% at 60; render thread 9.30 ms, GS thread 9.50 ms, a helper's job 378 us: as through
nxlink the same night, 57.1%, 9.30, 9.44 and 376, so the wrapped waits cost nothing that
shows). After the last fix every run ended "13 of 13 threads ended, 0 stopped" with no fault.

## When a run left the process unusable

A run that ends without giving the GPU back leaves it, and other memory, in the process that the
next run starts in. One started with the GPU would fault, or bring the whole system down (2026-11-25);
a run refuses to start in a process marked so (`ProcessTainted`, `sdmc:/switch/sly1/tainted.txt`) and
the way out is to close the receiver from HOME and start it again. A run with the GPU never opened
works in it all the same and measures the CPU side on the Switch's own cores:

    SLY1_NO_GPU=1 SLY1_GS_COUNT=1 SLY1_GS_COUNT_BATCHES=1

(`switch_matrix.py --common` takes them). No picture, and no GPU time: the frame's own limit in
such a run is the vertical blank and the threads. The program's heap starts at the largest clean
stretch of the loader's (`__libnx_initheap`), so a piece of it a run before left lent out is never
handed to it: the fault on that memory is what made the first attempts fail.

