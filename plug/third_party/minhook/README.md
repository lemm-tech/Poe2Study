# MinHook

Upstream: https://github.com/TsudaKageyu/minhook

Version: v1.3.4, commit `c3fcafdc10146beb5919319d0683e44e3c30d537`.

The unmodified `include/` and `src/` files are compiled directly into Monitor.dll with the project's compiler. No precompiled MinHook library or additional runtime DLL is required. The current build uses `buffer.c`, `hook.c`, `trampoline.c`, and `hde/hde64.c` for Windows x64.

`LICENSE.txt` contains the upstream MinHook and Hacker Disassembler Engine licenses. Include it with binary distributions. `poe2-plug-LICENSE.txt` retains the license of the reference project used when studying the integration.
