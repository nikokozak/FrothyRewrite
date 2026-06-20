package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

const (
	initFrothyToml = `name   = "blink"
target = "esp32_devkit_v1"
`
	initMainFr    = `boot is fn [ led.blink: 3, 100 ]
`
	initGitignore = `.frothy/
`
)

func runInitMain() int {
	wd, err := os.Getwd()
	if err != nil {
		fmt.Fprintf(os.Stderr, "init: %v\n", err)
		return 1
	}
	return runInitCommand(os.Args[1:], wd, os.Stdout, os.Stderr)
}

func runInitCommand(args []string, dir string, stdout io.Writer, stderr io.Writer) int {
	fs := flag.NewFlagSet("frothy init", flag.ContinueOnError)
	fs.SetOutput(stderr)
	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("init"), fs)
		return 0
	}
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintln(stderr, "init: takes no positional arguments")
		return 2
	}

	// Refuse before any write so a half-initialized project is impossible.
	for _, name := range []string{"frothy.toml", "main.fr", ".gitignore", "libs"} {
		if _, err := os.Lstat(filepath.Join(dir, name)); err == nil {
			fmt.Fprintf(stderr, "init: %s already exists\n", name)
			return 1
		} else if !errors.Is(err, os.ErrNotExist) {
			fmt.Fprintf(stderr, "init: %v\n", err)
			return 1
		}
	}

	files := []struct {
		name string
		body string
	}{
		{"frothy.toml", initFrothyToml},
		{"main.fr", initMainFr},
		{".gitignore", initGitignore},
	}
	for _, f := range files {
		if err := os.WriteFile(filepath.Join(dir, f.name), []byte(f.body), 0o644); err != nil {
			fmt.Fprintf(stderr, "init: %v\n", err)
			return 1
		}
	}
	if err := os.Mkdir(filepath.Join(dir, "libs"), 0o755); err != nil {
		fmt.Fprintf(stderr, "init: %v\n", err)
		return 1
	}

	fmt.Fprintln(stdout, "initialized frothy project (frothy.toml, main.fr, .gitignore, libs/)")
	return 0
}
