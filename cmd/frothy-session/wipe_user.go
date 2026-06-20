package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"time"
)

type wipeUserDeviceFactory func(port string, baud int) (sessionDevice, func(), error)

const (
	wipeUserBaud    = 115200
	wipeUserTimeout = 3 * time.Second
	wipeUserSettle  = 2 * time.Second
)

func defaultWipeUserDeviceFactory(port string, baud int) (sessionDevice, func(), error) {
	dev, err := openSerial(port, baud)
	if err != nil {
		return nil, nil, err
	}
	return dev, dev.close, nil
}

func runWipeUserMain() int {
	return runWipeUserCommand(os.Args[1:], os.Stdout, os.Stderr, defaultPortLister, defaultWipeUserDeviceFactory,
		wipeUserBaud, wipeUserTimeout, wipeUserSettle)
}

func runWipeUserCommand(args []string, stdout io.Writer, stderr io.Writer, list portLister, openDev wipeUserDeviceFactory,
	baud int, timeout time.Duration, settle time.Duration) int {
	fs := flag.NewFlagSet("frothy wipe-user", flag.ContinueOnError)
	fs.SetOutput(stderr)
	port := fs.String("port", "", "serial port, for example /dev/cu.usbserial-0001")

	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("wipe-user"), fs)
		return 0
	}
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintln(stderr, "wipe-user: takes no positional arguments")
		return 2
	}

	chosen, err := pickPort(*port, list)
	if err != nil {
		fmt.Fprintf(stderr, "wipe-user: %v\n", err)
		return 2
	}

	dev, closeDev, err := openDev(chosen, baud)
	if err != nil {
		fmt.Fprintf(stderr, "wipe-user: cannot open %s: %v\n", chosen, err)
		return 1
	}
	defer closeDev()

	if settle > 0 {
		time.Sleep(settle)
	}

	if err := dev.syncPrompt(timeout); err != nil {
		fmt.Fprintf(stderr, "wipe-user: %v\n", err)
		return 1
	}

	response, err := dev.sendLine("wipe-user", timeout, nil)
	if err != nil {
		fmt.Fprintf(stderr, "wipe-user: %v\n", err)
		return 1
	}
	if !responseOK(response) {
		fmt.Fprintf(stderr, "wipe-user: device returned %s\n", responseStatus(response))
		return 1
	}
	return 0
}
