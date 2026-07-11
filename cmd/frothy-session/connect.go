package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"
	"time"
)

type connectDeviceFactory func(port string, baud int) (*serialDevice, func(), error)

type connectInteractiveFn func(dev *serialDevice, stdin io.Reader, stdout io.Writer, hist connectHistory) int

func runConnectMain() int {
	return runConnect()
}

func runConnectCommand(args []string, stdin io.Reader, stdout io.Writer, stderr io.Writer, list portLister, open connectDeviceFactory, interactive connectInteractiveFn) int {
	return runConnectCommandWithStopper(args, stdin, stdout, stderr, list, open, interactive, defaultSerialStopper())
}

func runConnectCommandWithStopper(args []string, stdin io.Reader, stdout io.Writer, stderr io.Writer, list portLister, open connectDeviceFactory, interactive connectInteractiveFn, stopper serialStopper) int {
	fs := flag.NewFlagSet("frothy connect", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var (
		port     = fs.String("port", "", "serial port, for example /dev/cu.usbmodem101")
		baud     = fs.Int("baud", 115200, "serial baud rate")
		takeover = fs.Bool("takeover", false, "stop another frothy holding the port before connecting")
		// Connect probes immediately. Boards that reset on serial open are
		// covered by the status retry window; --settle remains an escape hatch
		// for unusual adapters that need a fixed quiet window after open.
		timeout     = fs.Duration("timeout", 5*time.Second, "serial prompt timeout")
		settle      = fs.Duration("settle", 0, "delay after opening serial")
		noHistory   = fs.Bool("no-history", false, "disable history read/write")
		historyFile = fs.String("history-file", "", "override history file path (default $XDG_DATA_HOME/frothy/history)")
	)
	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("connect"), fs)
		return 0
	}
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
		busy, ok := asSerialBusyError(err)
		targets := stopper.targetsFromHolders(chosen, busy.holders)
		if !ok || len(targets) == 0 {
			fmt.Fprintf(stderr, "connect: %v\n", err)
			return 1
		}
		allowed := *takeover
		if !allowed && connectCanPromptTakeover(stdin, stdout) {
			allowed = promptConnectTakeover(stdin, stderr, chosen, targets)
		}
		if !allowed {
			fmt.Fprintf(stderr, "connect: %v\n", err)
			return 1
		}
		dev, closeDev, err = stopper.takeoverPort(chosen, targets, func() (*serialDevice, func(), error) {
			return open(chosen, *baud)
		})
		if err != nil {
			fmt.Fprintf(stderr, "connect: %v\n", err)
			return 1
		}
	}
	defer closeDev()
	time.Sleep(*settle)

	_, err = readDeviceStatus(dev, *timeout)
	if err != nil {
		fmt.Fprintf(stderr, "connect: device silent or wedged; try frothy wipe --force esp32_devkit_v1 --port %s: %v\n", chosen, err)
		return 1
	}
	if interactive == nil {
		fmt.Fprintln(stderr, "frothy connect: not yet implemented")
		return 1
	}
	hist := resolveHistoryConfig(*noHistory, *historyFile, os.Getenv)
	return interactive(dev, stdin, stdout, hist)
}

func asSerialBusyError(err error) (serialPortBusyError, bool) {
	var busy serialPortBusyError
	if errors.As(err, &busy) {
		return busy, true
	}
	return serialPortBusyError{}, false
}

func isSerialBusyError(err error) bool {
	_, ok := asSerialBusyError(err)
	return ok
}

func connectCanPromptTakeover(stdin io.Reader, stdout io.Writer) bool {
	return readerIsTerminal(stdin) && writerIsTerminal(stdout)
}

func promptConnectTakeover(stdin io.Reader, stderr io.Writer, port string, targets []frothyStopTarget) bool {
	fmt.Fprintf(stderr, "port %s is held by another frothy (%s) - stop it and take over? [y/N] ",
		port, formatStopTargetPIDs(targets))
	answer, err := readAnswerLine(stdin)
	if err != nil {
		return false
	}
	answer = strings.ToLower(strings.TrimSpace(answer))
	return answer == "y" || answer == "yes"
}

func readAnswerLine(in io.Reader) (string, error) {
	var line []byte
	var one [1]byte
	for {
		n, err := in.Read(one[:])
		if n > 0 {
			switch one[0] {
			case '\n', '\r':
				return string(line), nil
			default:
				line = append(line, one[0])
			}
		}
		if err != nil {
			return string(line), err
		}
	}
}
