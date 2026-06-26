//go:build !darwin && !linux

package main

import (
	"fmt"
	"io"
	"os"
)

type serialStopper struct{}

type frothyStopTarget struct {
	pid     int
	command string
	ports   []string
}

func runStopMain() int {
	fmt.Fprintln(os.Stderr, "frothy stop is not supported on this platform yet")
	return 1
}

func runStopCommand(_ []string, _ io.Writer, stderr io.Writer, _ serialStopper) int {
	fmt.Fprintln(stderr, "frothy stop is not supported on this platform yet")
	return 1
}

func defaultSerialStopper() serialStopper {
	return serialStopper{}
}

func (serialStopper) targetsFromHolders(string, []serialPortHolder) []frothyStopTarget {
	return nil
}

func (serialStopper) takeoverPort(string, []frothyStopTarget, func() (*serialDevice, func(), error)) (*serialDevice, func(), error) {
	return nil, nil, fmt.Errorf("serial takeover is not supported on this platform")
}

func formatStopTargetPIDs([]frothyStopTarget) string {
	return "pid unknown"
}
