# libriscv fuzzing system

## Building

To build all the available fuzzers run:
```
./fuzzer.sh
```

All the fuzzers are built in the build folder:
```
la -ls build/*fuzzer*
```
There are 3 fuzzers of each kind. A 32-bit, a 64--bit and a 128-bit fuzzer. Normally we would try to fuzz as much as possible, however the fuzzer is ineffective when it fuzzes different machines and loaders at the same time.

## Fuzzing

Example starting a fuzzer:
```
./build/vmfuzzer32 -N4 -handle_fpe=0
```
You may want to make sure coredumps are enabled, through ASAN_OPTIONS:
```
export ASAN_OPTIONS=disable_coredump=0::unmap_shadow_on_exit=1::handle_segv=0::handle_sigfpe=0
```

[libfuzzer](https://llvm.org/docs/LibFuzzer.html) is being employed, which is a part of LLVM-Clang.

## Fuzzing the ELF loader in Docker

If the native build doesn't work on your machine, `docker.sh` builds the ELF
loader fuzzers (`elffuzzer32` / `elffuzzer64`) inside a container with the
right clang-18 toolchain and runs one of them in normal libFuzzer mode:

```
./docker.sh                          # elffuzzer64 (default)
./docker.sh elffuzzer32              # 32-bit ELF loader
./docker.sh elffuzzer64 -- -max_len=8192   # forward extra libFuzzer args
```

The image seeds the corpus with a real RISC-V ELF binary (`led_hello.elf` for
rv32, `hello` for rv64) so the fuzzer gets past ELF header validation and
reaches the loader/decoder code. Note that each iteration loads and simulates a
full ELF under ASan at `-O0`, so it runs at a low exec/s — let it fuzz for a
while.

The corpus and any crash artifacts are persisted on the host under
`fuzz/corpus-<target>/`, so progress survives container restarts.
