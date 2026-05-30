//go:build darwin || linux

package main

import (
	"io"
	"os"
	"strings"
	"syscall"
	"testing"
	"time"

	"frothyrewrite/internal/testpty"
)

// SIGTERM through the wrapper exits with code 143 (128 + SIGTERM = 15)
// and restores the stdin terminal to its initial termios state.
func TestRunConnectInteractiveTermiosSIGTERMRestoresAndExits143(t *testing.T) {
	dev, devClose := newTestSerialPTY(t)
	defer devClose()

	termMaster, termSlave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	defer termMaster.Close()
	defer termSlave.Close()

	initial, err := captureTermiosState(termSlave)
	if err != nil {
		t.Fatal(err)
	}

	sigCh := make(chan os.Signal, 1)
	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractiveTermios(dev, termSlave, io.Discard, connectHistory{}, sigCh)
	}()

	waitForTermiosDiff(t, termSlave, initial, time.Second)

	sigCh <- syscall.SIGTERM

	select {
	case code := <-done:
		if code != 143 {
			t.Fatalf("exit code = %d, want 143", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("SIGTERM did not exit")
	}

	restored, err := captureTermiosState(termSlave)
	if err != nil {
		t.Fatal(err)
	}
	if restored != initial {
		t.Fatalf("termios not restored: got %q, want %q", restored, initial)
	}
}

// Ctrl-D on an empty buffer exits cleanly (code 0) and restores the stdin
// terminal to its initial termios state.
func TestRunConnectInteractiveTermiosNormalExitRestores(t *testing.T) {
	dev, devClose := newTestSerialPTY(t)
	defer devClose()

	termMaster, termSlave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	defer termMaster.Close()
	defer termSlave.Close()

	initial, err := captureTermiosState(termSlave)
	if err != nil {
		t.Fatal(err)
	}

	sigCh := make(chan os.Signal, 1)
	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractiveTermios(dev, termSlave, io.Discard, connectHistory{}, sigCh)
	}()

	waitForTermiosDiff(t, termSlave, initial, time.Second)

	if _, err := termMaster.Write([]byte{0x04}); err != nil {
		t.Fatal(err)
	}

	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code = %d, want 0", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Ctrl-D did not exit")
	}

	restored, err := captureTermiosState(termSlave)
	if err != nil {
		t.Fatal(err)
	}
	if restored != initial {
		t.Fatalf("termios not restored: got %q, want %q", restored, initial)
	}
}

func newTestSerialPTY(t *testing.T) (*serialDevice, func()) {
	t.Helper()
	master, slave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		_ = master.Close()
		_ = slave.Close()
		t.Fatal(err)
	}
	dev := &serialDevice{
		file:   slave,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()
	return dev, func() {
		_ = master.Close()
		_ = slave.Close()
	}
}

func captureTermiosState(file *os.File) (string, error) {
	out, err := runStty(file, []string{"-g"})
	if err != nil {
		return "", err
	}
	return strings.TrimRight(out, "\r\n"), nil
}

func waitForTermiosDiff(t *testing.T, file *os.File, initial string, limit time.Duration) {
	t.Helper()
	deadline := time.Now().Add(limit)
	for time.Now().Before(deadline) {
		cur, err := captureTermiosState(file)
		if err == nil && cur != initial {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for raw mode to take effect")
}
