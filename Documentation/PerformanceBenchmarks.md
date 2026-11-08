# Native and guest performance measurements

Performance comparisons should use the same workload, input data, and sample
count on both systems. Guest-side virtual GPU timings are not interchangeable
with macOS frame-presentation timing.

## CPU

`benchmark_cpu.py` runs deterministic SHA-256 and integer workloads with the
Python standard library. Run it sequentially on each system using the same
Python and OpenSSL versions:

```sh
python3 scripts/benchmark_cpu.py --samples 30 --warmups 5 > cpu.json
```

Review raw samples and checksums, not only the median. Python performance
depends on the OS scheduler and core selection as well as virtualization.
Single-process timings cannot establish multicore throughput.

## Vulkan GPU

`benchmark_vulkan.c` executes two verified workloads against a hardware Vulkan
GPU: an integer-compute shader over 1,048,576 words (64 rounds per word) and
a 32 MiB buffer copy. It refuses to substitute a software Vulkan device.

Build the **same shader binary once**, then use it with both executables:

```sh
glslangValidator -V --target-env vulkan1.1 -S comp \
  scripts/benchmark_vulkan.comp -o benchmark_vulkan.spv
```

Compile the C source with Vulkan headers on each operating system. On macOS,
link against the MoltenVK framework shipped with the tested UTM build:

```sh
frameworks="/path/to/UTM.app/Contents/Frameworks"
headers="/path/to/vulkan-headers/include"
clang -O2 -std=c11 -I "$headers" scripts/benchmark_vulkan.c \
  -F "$frameworks" -framework MoltenVK \
  -Wl,-rpath,"$frameworks" -o benchmark_vulkan
```

In the Linux guest, link the identical source against the Vulkan loader:

```sh
headers="/path/to/vulkan-headers/include"
loader="/path/to/vulkan-loader/lib"
cc -O2 -std=c11 -I "$headers" scripts/benchmark_vulkan.c \
  -L "$loader" -Wl,-rpath,"$loader" -lvulkan -lm -o benchmark_vulkan
```

Run the benchmark without other GPU stress tests running:

```sh
./benchmark_vulkan ./benchmark_vulkan.spv --samples 30 --warmups 5 \
  > vulkan.json
```

Every compute output word and all 32 MiB of the copied buffer are validated.
Each reported duration has raw samples plus median, P95, and P99. Both
systems record `wall_submit_wait`, which starts just before `vkQueueSubmit`
and ends after `vkQueueWaitIdle`. It includes driver and virtualization
submission overhead, **not** pipeline creation or shader compilation.

Device timestamp results are included only when sane. A driver can advertise
timestamp queries yet return nonmonotonic or otherwise invalid values;
in that case `gpu_timestamps` is `null` rather than a misleading duration.

## Interpreting results

Keep native macOS, upstream UTM, and forked UTM results separate. Record OS
versions, GPU drivers, guest vCPU/RAM allocations, display resolution, power
state, thermal state, and concurrent activity. Prefer repeated alternating
runs and compare distributions rather than treating one ratio as proof.

The buffer-copy test uses host-visible Vulkan memory; it is **not** a substitute
for texture-upload profiling. Actual desktop performance requires independent
Wayland presentation-feedback measurements, including 3840×2160, with missed
refreshes and tail latency reported explicitly. A Vulkan compute benchmark
cannot measure input-to-photon latency or confirm sustained display FPS.


## Summarize a matched run

The comparison helper checks both runs' workload checksums and method metadata
before printing a table. Vulkan inputs must also have the same compiled shader
hash. It does not compare invalid or unsupported GPU-only timestamp data.

```sh
python3 scripts/compare_benchmarks.py \
  native-cpu.json guest-cpu.json native-vulkan.json guest-vulkan.json
```

Keep raw JSON files for later reruns; the table is a summary, not a substitute
for latency distributions or controlled thermal testing.
