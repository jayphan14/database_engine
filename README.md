# database_engine

A small SQL `SELECT` parser written in C++17.

## Requirements

- `g++` with C++17 support (or `clang++` — adjust `CXX` in the `Makefile`)
- `make`

## Build and run

Build the `dbms` binary:

```sh
make dbms
```

Run it (parses a handful of example queries in `main.cpp` and prints each AST):

```sh
./dbms
```

Or do both in one step:

```sh
make run
```

## Run tests locally

Tests use [doctest](https://github.com/doctest/doctest) (vendored at `tests/vendor/doctest.h`, no install needed):

```sh
make test
```

This builds and runs `build/run_tests`. Pass doctest flags by invoking the binary directly, e.g.:

```sh
build/run_tests --help
build/run_tests --test-case="SELECT *"
```

## Clean

```sh
make clean
```

Removes `build/` and the `dbms` binary.
