package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"
)

type installDeviceFactory func(port string, baud int) (sessionDevice, func(), error)

const (
	installBaud    = 115200
	installTimeout = 3 * time.Second
	installSettle  = 2 * time.Second
)

func defaultInstallDeviceFactory(port string, baud int) (sessionDevice, func(), error) {
	dev, err := openSerial(port, baud)
	if err != nil {
		return nil, nil, err
	}
	return dev, dev.close, nil
}

func runInstallMain() int {
	return runInstallCommand(os.Args[1:], os.Stdout, os.Stderr, defaultPortLister, defaultInstallDeviceFactory,
		installBaud, installTimeout, installSettle)
}

func runInstallCommand(args []string, stdout io.Writer, stderr io.Writer, list portLister, openDev installDeviceFactory,
	baud int, timeout time.Duration, settle time.Duration) int {
	fs := flag.NewFlagSet("frothy install", flag.ContinueOnError)
	fs.SetOutput(stderr)
	port := fs.String("port", "", "serial port, for example /dev/cu.usbserial-0001")
	projectDir := fs.String("project", ".", "project directory containing frothy.toml")

	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("install"), fs)
		return 0
	}

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintln(stderr, "install: takes no positional arguments")
		return 2
	}

	absProject, err := filepath.Abs(*projectDir)
	if err != nil {
		fmt.Fprintf(stderr, "install: %v\n", err)
		return 1
	}

	proj, err := readProjectManifest(absProject)
	if err != nil {
		fmt.Fprintf(stderr, "install: %v\n", err)
		return 1
	}

	libraryPath := filepath.Join(buildOutputDir(absProject, proj.Board), "library.fr")
	if _, err := os.Stat(libraryPath); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			fmt.Fprintf(stderr, "error: library.fr not found at %s; run frothy build first\n", libraryPath)
		} else {
			fmt.Fprintf(stderr, "install: %v\n", err)
		}
		return 1
	}

	lines, err := readFileLines(libraryPath)
	if err != nil {
		fmt.Fprintf(stderr, "install: %v\n", err)
		return 1
	}

	chosen, err := pickPort(*port, list)
	if err != nil {
		fmt.Fprintf(stderr, "install: %v\n", err)
		return 2
	}

	dev, closeDev, err := openDev(chosen, baud)
	if err != nil {
		fmt.Fprintf(stderr, "error: cannot open %s: %v\n", chosen, err)
		return 1
	}
	defer closeDev()

	if settle > 0 {
		time.Sleep(settle)
	}

	if err := dev.syncPrompt(timeout); err != nil {
		fmt.Fprintf(stderr, "install: %v\n", err)
		return 1
	}

	response, err := dev.sendLine("install-library", timeout, nil)
	if err != nil {
		if errors.Is(err, errPromptTimeout) {
			fmt.Fprintf(stderr, "error: device did not acknowledge install-library within %s\n", timeout)
		} else {
			fmt.Fprintf(stderr, "install: %v\n", err)
		}
		return 1
	}
	if !responseOK(response) {
		fmt.Fprintf(stderr, "error: device returned %s\n", responseStatus(response))
		return 1
	}
	printDeviceResponse(stderr, responseNoticeText(response))

	for _, line := range lines {
		response, err := dev.sendLine(line, timeout, nil)
		if err != nil {
			fmt.Fprintf(stderr, "install: %v\n", err)
			return 1
		}
		if !responseOK(response) {
			fmt.Fprintf(stderr, "error: device returned %s\n", responseStatus(response))
			return 1
		}
		printDeviceResponse(stderr, responseNoticeText(response))
	}
	return 0
}
