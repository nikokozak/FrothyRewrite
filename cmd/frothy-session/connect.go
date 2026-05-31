package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"time"
)

type connectDeviceFactory func(port string, baud int) (*serialDevice, func(), error)

type connectInteractiveFn func(dev *serialDevice, stdin io.Reader, stdout io.Writer, hist connectHistory) int

func runConnectMain() int {
	return runConnect()
}

func runConnectCommand(args []string, stdin io.Reader, stdout io.Writer, stderr io.Writer, list portLister, open connectDeviceFactory, interactive connectInteractiveFn) int {
	fs := flag.NewFlagSet("frothy connect", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var (
		port        = fs.String("port", "", "serial port, for example /dev/cu.usbmodem101")
		baud        = fs.Int("baud", 115200, "serial baud rate")
		timeout     = fs.Duration("timeout", 3*time.Second, "serial prompt timeout")
		settle      = fs.Duration("settle", 2*time.Second, "delay after opening serial")
		noHistory   = fs.Bool("no-history", false, "disable history read/write")
		historyFile = fs.String("history-file", "", "override history file path (default $XDG_DATA_HOME/frothy/history)")
	)
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintln(stderr, "connect: takes no positional arguments")
		return 2
	}

	chosen, err := pickPort(*port, list)
	if err != nil {
		fmt.Fprintf(stderr, "connect: %v\n", err)
		return 2
	}
	dev, closeDev, err := open(chosen, *baud)
	if err != nil {
		fmt.Fprintf(stderr, "connect: %v\n", err)
		return 1
	}
	defer closeDev()
	time.Sleep(*settle)

	status, err := readDeviceStatus(dev, *timeout)
	if err != nil {
		fmt.Fprintf(stderr, "connect: device silent or wedged; try frothy wipe --force esp32_devkit_v1 --port %s: %v\n", chosen, err)
		return 1
	}
	if status.compiler == compilerHostRequired {
		fmt.Fprintln(stderr, "frothy connect: device advertises host-required mode; use frothy session or frothy send instead")
		return 1
	}

	if interactive == nil {
		fmt.Fprintln(stderr, "frothy connect: not yet implemented")
		return 1
	}
	hist := resolveHistoryConfig(*noHistory, *historyFile, os.Getenv)
	return interactive(dev, stdin, stdout, hist)
}
