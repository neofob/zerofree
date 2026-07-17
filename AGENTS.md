# Agent Instructions for zerofree

## Project Overview
`zerofree` is a C program for Linux designed to zero out unused blocks in a filesystem to make it more sparse. It targets modern x86-64 CPUs and leverages SIMD instructions (AVX2, AVX-512) for performance.

## Build and Development
- **Standard**: C11 (`-std=c11`)
- **Target Architecture**: Modern x86-64 with AVX-512 support.
- **Compiler**: `gcc`
- **Dependencies**: `libext2fs`

### Common Commands
- **Build all targets**: `make`
- **Clean build artifacts**: `make clean`
- **Generate tags**: `make tags`

## Coding Conventions
- Follow modern C11 standards.
- Use SIMD intrinsics for performance-critical paths (as seen in `zerofree.c`).
- Ensure memory safety and handle Linux system call errors rigorously.
- Remove trailing whitespace in the source code.

## Verification
Since there is no automated test suite in the `Makefile`, manually verify changes by:
1. Compiling with `make`.
2. Testing against sample disk images (e.g., `test_disk.img`).
