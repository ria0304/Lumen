#!/bin/sh
# Working wrapper for interactive QEMU runs.
# Forwards all arguments to `make run` (e.g. ./run.sh QEMUFLAGS="-m 256").
exec make run "$@"
