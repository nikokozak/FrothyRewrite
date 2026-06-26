//go:build !darwin && !linux

package main

import (
	"io"
	"os"
)

type serialPortBusyError struct {
	port    string
	holders []serialPortHolder
	message string
	err     error
}

func (e serialPortBusyError) Error() string {
	if e.message != "" {
		return e.message
	}
	return "serial port is in use"
}

func (e serialPortBusyError) Unwrap() error {
	return e.err
}

type serialPortHolder struct {
	pid     int
	command string
	frothy  bool
}

func writerIsTerminal(io.Writer) bool {
	return false
}

func readerIsTerminal(io.Reader) bool {
	return false
}

func setSerialExclusive(*os.File) error {
	return nil
}

func decorateSerialOpenError(_ string, err error) error {
	return err
}
