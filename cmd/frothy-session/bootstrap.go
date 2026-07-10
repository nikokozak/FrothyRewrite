package main

import (
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
)

// bootstrapScriptPath is relative to the Frothy source root.
const bootstrapScriptPath = "tools/setup-esp-idf.sh"

func runBootstrapMain() int {
	return runBootstrapCommand(os.Args[1:], os.Stdout, os.Stderr, defaultBootstrapRunner)
}

// defaultBootstrapRunner executes the setup script with the caller's argv.
// Tests inject a stub.
type bootstrapRunner func(scriptPath string, args []string, stdout io.Writer, stderr io.Writer) int

func defaultBootstrapRunner(scriptPath string, args []string, stdout io.Writer, stderr io.Writer) int {
	cmd := exec.Command(scriptPath, args...)
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	cmd.Stdin = os.Stdin
	if err := cmd.Run(); err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			return ee.ExitCode()
		}
		fmt.Fprintf(stderr, "bootstrap: %v\n", err)
		return 1
	}
	return 0
}

func runBootstrapCommand(args []string, stdout io.Writer, stderr io.Writer, run bootstrapRunner) int {
	fs := flag.NewFlagSet("frothy bootstrap", flag.ContinueOnError)
	fs.SetOutput(stderr)
	force := fs.Bool("force", false, "reinstall ESP-IDF from scratch, replacing any existing install")

	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("bootstrap"), fs)
		return 0
	}
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintln(stderr, "bootstrap: takes no positional arguments")
		return 2
	}

	sourceRoot, err := resolveFrothySourceRoot(".")
	if err != nil {
		fmt.Fprintf(stderr, "bootstrap: %v\n", err)
		return 2
	}
	scriptPath := filepath.Join(sourceRoot, bootstrapScriptPath)
	if _, err := os.Stat(scriptPath); err != nil {
		fmt.Fprintf(stderr, "bootstrap: %s not found\n", scriptPath)
		return 2
	}

	var scriptArgs []string
	if *force {
		scriptArgs = append(scriptArgs, "--force")
	}
	return run(scriptPath, scriptArgs, stdout, stderr)
}
