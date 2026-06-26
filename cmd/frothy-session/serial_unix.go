//go:build darwin || linux

package main

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"unsafe"
)

type terminalWindowSize struct {
	row    uint16
	col    uint16
	xpixel uint16
	ypixel uint16
}

func writerIsTerminal(w io.Writer) bool {
	f, ok := w.(*os.File)
	return ok && fileIsTerminal(f)
}

func readerIsTerminal(r io.Reader) bool {
	f, ok := r.(*os.File)
	return ok && fileIsTerminal(f)
}

func fileIsTerminal(file *os.File) bool {
	if file == nil {
		return false
	}
	var size terminalWindowSize
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, file.Fd(), uintptr(syscall.TIOCGWINSZ), uintptr(unsafe.Pointer(&size)))
	return errno == 0
}

func setSerialExclusive(file *os.File) error {
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, file.Fd(), uintptr(syscall.TIOCEXCL), 0)
	if errno != 0 {
		return errno
	}
	return nil
}

type serialPortBusyError struct {
	port    string
	holders []serialPortHolder
	message string
	err     error
}

func (e serialPortBusyError) Error() string {
	if e.message == "" && e.port != "" {
		return formatSerialPortBusyMessage(e.port, e.holders)
	}
	if e.message == "" {
		return "serial port is in use"
	}
	return e.message
}

func (e serialPortBusyError) Unwrap() error {
	return e.err
}

func decorateSerialOpenError(port string, err error) error {
	if !errors.Is(err, syscall.EBUSY) {
		return err
	}
	holders := serialPortHolders(port)
	return serialPortBusyError{
		port:    port,
		holders: holders,
		message: formatSerialPortBusyMessage(port, holders),
		err:     err,
	}
}

type serialPortHolder struct {
	pid     int
	command string
	frothy  bool
}

func serialPortHolders(port string) []serialPortHolder {
	out, err := exec.Command("lsof", "-F", "pc", port).Output()
	if err != nil || len(out) == 0 {
		return nil
	}
	holders := parseLsofHolders(string(out))
	if len(holders) == 0 {
		return nil
	}

	executable, _ := os.Executable()
	for i := range holders {
		if command, err := processCommand(holders[i].pid); err == nil && command != "" {
			holders[i].command = command
		}
		holders[i].frothy = isFrothyHolder(holders[i].command, executable)
	}
	return holders
}

func parseLsofHolders(text string) []serialPortHolder {
	var holders []serialPortHolder
	for _, line := range strings.Split(text, "\n") {
		if line == "" {
			continue
		}
		switch line[0] {
		case 'p':
			pid, err := strconv.Atoi(strings.TrimSpace(line[1:]))
			if err == nil {
				holders = append(holders, serialPortHolder{pid: pid})
			}
		case 'c':
			if len(holders) > 0 {
				holders[len(holders)-1].command = strings.TrimSpace(line[1:])
			}
		}
	}
	return holders
}

func processCommand(pid int) (string, error) {
	out, err := exec.Command("ps", "-p", strconv.Itoa(pid), "-o", "comm=").Output()
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(string(out)), nil
}

func isFrothyHolder(command string, executable string) bool {
	command = strings.TrimSpace(command)
	if command == "" {
		return false
	}
	if executable != "" && filepath.Clean(command) == filepath.Clean(executable) {
		return true
	}
	base := filepath.Base(command)
	return base == "frothy" || base == "frothy-session"
}

func formatSerialPortBusyMessage(port string, holders []serialPortHolder) string {
	for _, holder := range holders {
		if holder.frothy && holder.pid > 0 {
			return fmt.Sprintf("port %s is in use by another frothy (pid %d); run 'frothy stop' to free it", port, holder.pid)
		}
	}
	for _, holder := range holders {
		if holder.pid <= 0 || holder.command == "" {
			continue
		}
		return fmt.Sprintf("port %s is held by %s (pid %d), which is not Frothy; close it and retry", port, holder.command, holder.pid)
	}
	return fmt.Sprintf("port %s is in use", port)
}
