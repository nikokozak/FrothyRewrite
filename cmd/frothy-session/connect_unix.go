//go:build darwin || linux

package main

import (
	"fmt"
	"os"
)

// runConnect is the Unix entry. The pump, line editor, history, and
// reset recovery land in follow-up slices; this scaffold exists so
// the platform split is real before any of that code arrives.
func runConnect() int {
	fmt.Fprintln(os.Stderr, "frothy connect: scaffold; serial REPL arriving in a follow-up slice")
	return 1
}
