//go:build windows

package main

import (
	"fmt"
	"os"
)

// runConnect on Windows reports the documented unsupported error.
// The terminal, signal, and /dev port discovery work is POSIX-only.
func runConnect() int {
	fmt.Fprintln(os.Stderr, "frothy connect is not supported on Windows yet")
	return 1
}
