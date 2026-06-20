package main

import (
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
)

// bootstrapScriptPath is the script-relative location TB's contract names.
// The bootstrap verb requires the script to exist in the repo root and fails
// clearly when it does not (TB SPEC Closed-decision #7).
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

	if _, err := os.Stat(bootstrapScriptPath); err != nil {
		fmt.Fprintf(stderr, "bootstrap: %s not found; run from the Frothy repo root\n", bootstrapScriptPath)
		return 2
	}

	var scriptArgs []string
	if *force {
		scriptArgs = append(scriptArgs, "--force")
	}
	return run(bootstrapScriptPath, scriptArgs, stdout, stderr)
}
