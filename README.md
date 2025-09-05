# Project 1: WASM Interpreter

This repo contains starter code (in C++) for building up a wasm interpreter. 
You are not required to use C++ or any part of this starter code for the project, but it 
will simplify many tasks for you.

Create a [private fork of the public repo](https://gist.github.com/0xjac/85097472043b697ab57ba1b1c7530274) 
when working on your solution

The project description and specifications of Project 1 can be found at the website [here](https://www.cs.cmu.edu/~btitzer/cs17-770/fall2024/proj1.html).

The core [WebAssembly spec](https://webassembly.github.io/spec/core/intro/index.html) is a valuable reference resource for implementation.


## Starter Code Structure
`main.cpp` serves as the entry-point that invokes the parser and interpreter.
We have provided you a parser that ingests a Wasm binary and produces a `WasmModule` instance that encapsulates the 
static state of the module (see [here](inc/ir.h)).
Your goal is to then construct dynamic state and the interpreter loop required to execute the binary.

A [list of legal opcodes](src/opcode_table.c) is specified that your VM is required to interpret.
We will not test you on any binaries outside of the subset enabled in the table.

Helper methods for LEB parsing are provided to you as well, see [inc/common.h](inc/common.h)

## Project Management

### Prerequisites
* Install the [WebAssembly Binary Toolkit](https://github.com/WebAssembly/wabt.git) from source or using `apt` on Linux.
Ensure that the relevant binary directory is added to PATH.
In particular, `wat2wasm` and `wasm2wat` will be very useful for you to inspect and modify test cases in text format.
* `cmake` and `make` are required to build the interpreter. 
* `clang` is required to build C test samples

### Build/Clean
To build, run `make`.

To clean, run `make clean`

While C/Make files may be modified,
**please ensure we can still build and clean your project with the targets above**


### Tests
We have provided some sample tests along with a build script to build.
**For this assignment, you are required to produce at least 25 problems of a non-trivial complexity (i.e. more than a single instruction).**

To build tests, run `make build-tests`.

To run tests, run `make run-tests`. This will execute the grading script and give you a summary.

## Notes
* The entrypoint of the Wasm binary can be always be assumed to be an exported function named "main".
* Feel free to modify the parser or `WasmModule` encoding if you wish
* When printing f64 outputs, print them with precision of *6-digits after the decimal point*
* Print all expected outputs (including *!trap*) to `stdout` only, and make sure `stderr` is empty for grading
* You may add extra command line options if you wish, but we will only use '-a' for testing
