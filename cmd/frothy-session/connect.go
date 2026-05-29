package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"time"
)

type connectDeviceFactory func(port string, baud int) (sessionDevice, func(), error)

func runConnectMain() int {
	return runConnect()
}

func runConnectCommand(args []string, stderr io.Writer, list portLister, open connectDeviceFactory) int {
	fs := flag.NewFlagSet("frothy connect", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var (
		port    = fs.String("port", "", "serial port, for example /dev/cu.usbmodem101")
		baud    = fs.Int("baud", 115200, "serial baud rate")
		timeout = fs.Duration("timeout", 3*time.Second, "serial prompt timeout")
		settle  = fs.Duration("settle", 2*time.Second, "delay after opening serial")
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
		fmt.Fprintf(stderr, "connect: %v\n", err)
		return 1
	}
	if status.compiler == compilerHostRequired {
		fmt.Fprintln(stderr, "frothy connect: device advertises host-required mode; use frothy session or frothy send instead")
		return 1
	}

	fmt.Fprintln(stderr, "frothy connect: not yet implemented")
	return 1
}
