package main

import (
	"flag"
	"fmt"
	"io"
	"os"
)

// SPEC D9 + acceptance #6 expect `frothy fetch` to resolve git deps
// into .frothy/cache/<hash>/ on cold caches. The full implementation
// (network + cache layout + lockfile-shaped reproducibility) hasn't
// landed yet; this stub exists so the CLI surface matches the SPEC
// and so the build verb's "git deps require frothy fetch" error
// points at a real verb.

func runFetchMain() int {
	return runFetchCommand(os.Args[1:], os.Stdout, os.Stderr)
}

func runFetchCommand(args []string, stdout io.Writer, stderr io.Writer) int {
	fs := flag.NewFlagSet("frothy fetch", flag.ContinueOnError)
	fs.SetOutput(stderr)
	_ = fs.String("project", ".", "project directory containing frothy.toml")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	fmt.Fprintln(stderr, "frothy fetch: git deps not yet wired in this tranche")
	return 1
}
