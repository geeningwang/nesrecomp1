# Build environment

Same constraint as the nes1 project: the development Mac kills freshly compiled executables
("Killed: 9"), so **nothing compiled is run on the Mac**. What the Mac can do:

* run all Python tools (3.9+),
* *compile without running* for foreign targets with Apple clang, which includes the ARM Thumb and WebAssembly
  back ends: `clang --target=thumbv7em-none-eabihf -mcpu=cortex-m7 -Os -ffreestanding -c x.c` and
  `clang --target=wasm32 -Os -ffreestanding -c x.c`; object sizes via
  `/Library/Developer/CommandLineTools/usr/bin/llvm-size`. This is how the STM32 / web code-size spikes are measured.

Everything that must run happens on VMs:

| VM | Role | Access |
|----|------|--------|
| `nes1-win-vm` (Windows Server 2022, existing) | MSVC builds, FCEUX oracle, cc65 for ROM rebuilds (`C:\work\nr\cc65\ca65.exe`, `ld65.exe`, taken from the disassembly repo), portable Python `C:\work\python\python.exe` | SSH as `nesuser`, see nes1 `doc/build_environment.md` |
| `nesrecomp1-linux-vm` (Ubuntu 22.04, e2-standard-4, created) | GCC 11 / Clang builds, sanitizers, `arm-none-eabi-gcc` 10.3; later Emscripten, Renode, Xvfb, Playwright | SSH as the OS Login user `admin_jingnw_altostrat_com` straight to the external IP (`ssh -i ~/.ssh/google_compute_engine admin_jingnw_altostrat_com@<ip>`); `gcloud compute ssh` goes through IAP and fails with a 502 here |

## Foundation tests

```
python -m unittest discover -s tests -p "test_*.py" -v
cmake -S . -B build -A x64 -DPython3_EXECUTABLE=C:\work\python\python.exe
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The smoke target compiles operation macros as C and calls them from a C++17 adapter with C
linkage. MSVC treats warnings as errors except C4127: constant flag masks are intentional in
these generated-operation macros. This is a linkage/arithmetic test, not full runtime parity.

## Rebuilding ROMs on the Windows VM

```
cd C:\work\nr
C:\work\python\python.exe tools\build_rom.py --all --ca65 C:\work\nr\cc65\ca65.exe --ld65 C:\work\nr\cc65\ld65.exe
```
(the repo is copied there with `tar`; the disassembly's `_misc` folders are not needed for the build).
The outputs are copied back to the Mac for the Python analysis (`out/` is git-ignored).

## Linux VM

Startup script (installs once, then touches `/var/lib/nesrecomp1-ready`):

```
apt-get install -y build-essential cmake ninja-build python3 python3-pip git clang gcc-arm-none-eabi \
  libsdl2-dev xvfb unzip curl
```
cc65 is not installed there: ROMs are rebuilt on the Windows VM and copied over. Emscripten (emsdk), Renode and
Playwright are installed on top when their milestones start.

Two things that cost time and are worth knowing:

- **ASan/UBSan crash at start-up on this kernel** (random `Segmentation fault` with no report, about one run in
  four) because the kernel's `vm.mmap_rnd_bits` is larger than the sanitizer runtime supports. Run
  `sudo sysctl -w vm.mmap_rnd_bits=28` before sanitizer runs (0 failures in 120 runs afterwards) and set
  `ASAN_OPTIONS=detect_leaks=0`.
- GCC is stricter than MSVC and Clang about the C macros of `runtime/include/nesrt_ops.h`; it rejected the
  prototype's `ADC`/`SBC` macros (`-Wmisleading-indentation`) the first time anything was built with GCC.

The Linux build-and-compare procedure that produced the numbers in `doc/runtime.md` is: build `nrt_test` with GCC,
Clang and Clang+ASan/UBSan (`cmake -G Ninja -DNES1_DIR=<nes1 checkout>`), run it on the 23 suite ROMs and one
replay with `--frames 3000 --digest --dump-frames all`, and `cmp` each `digest.txt` against the file produced by
the MSVC build on the Windows VM.
