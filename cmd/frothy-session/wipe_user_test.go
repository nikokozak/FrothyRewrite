package main

import (
	"bytes"
	"errors"
	"io"
	"strings"
	"testing"
	"time"
)

func fakeWipeUserFactory(dev *fakeDevice) wipeUserDeviceFactory {
	return func(port string, baud int) (sessionDevice, func(), error) {
		return dev, func() {}, nil
	}
}

func singlePortLister(port string) portLister {
	return func() ([]string, error) {
		return []string{port}, nil
	}
}

func TestRunWipeUserSendsCommandAndExitsZero(t *testing.T) {
	dev := &fakeDevice{responses: []string{"ok\n"}}
	var stdout bytes.Buffer
	var stderr bytes.Buffer

	code := runWipeUserCommand(
		[]string{"--port", "/dev/cu.usbserial-0001"},
		&stdout, &stderr, singlePortLister("/dev/cu.usbserial-0001"), fakeWipeUserFactory(dev),
		115200, time.Second, 0,
	)
	if code != 0 {
		t.Fatalf("expected exit 0, got %d (stderr=%q)", code, stderr.String())
	}
	if got, want := stdout.String(), "wiped user definitions on /dev/cu.usbserial-0001\n"; got != want {
		t.Fatalf("stdout = %q, want %q", got, want)
	}
	if len(dev.sent) != 1 || dev.sent[0] != "wipe-user" {
		t.Fatalf("expected single wipe-user line sent, got %v", dev.sent)
	}
	if dev.syncs != 1 {
		t.Fatalf("expected one sync, got %d", dev.syncs)
	}
}

func TestRunWipeUserSurfacesDeviceErrorLine(t *testing.T) {
	dev := &fakeDevice{responses: []string{"error: unsupported (9)\n"}}
	var stderr bytes.Buffer

	code := runWipeUserCommand(
		[]string{"--port", "/dev/cu.usbserial-0001"},
		io.Discard, &stderr, singlePortLister("/dev/cu.usbserial-0001"), fakeWipeUserFactory(dev),
		115200, time.Second, 0,
	)
	if code == 0 {
		t.Fatalf("expected non-zero exit, got 0 (stderr=%q)", stderr.String())
	}
	if !strings.Contains(stderr.String(), "error: unsupported") {
		t.Fatalf("expected stderr to surface device error line, got %q", stderr.String())
	}
}

func TestRunWipeUserRejectsPositionalArgs(t *testing.T) {
	dev := &fakeDevice{}
	var stderr bytes.Buffer

	code := runWipeUserCommand(
		[]string{"esp32_devkit_v1", "--port", "/dev/cu.usbserial-0001"},
		io.Discard, &stderr, singlePortLister("/dev/cu.usbserial-0001"), fakeWipeUserFactory(dev),
		115200, time.Second, 0,
	)
	if code != 2 {
		t.Fatalf("expected exit 2 for positional args, got %d", code)
	}
	if len(dev.sent) != 0 {
		t.Fatalf("expected no device traffic, got %v", dev.sent)
	}
}

func TestRunWipeUserReportsOpenFailure(t *testing.T) {
	openErr := errors.New("permission denied")
	factory := func(port string, baud int) (sessionDevice, func(), error) {
		return nil, nil, openErr
	}
	var stderr bytes.Buffer

	code := runWipeUserCommand(
		[]string{"--port", "/dev/cu.usbserial-0001"},
		io.Discard, &stderr, singlePortLister("/dev/cu.usbserial-0001"), factory,
		115200, time.Second, 0,
	)
	if code != 1 {
		t.Fatalf("expected exit 1, got %d", code)
	}
	if !strings.Contains(stderr.String(), "permission denied") {
		t.Fatalf("expected stderr to surface open error, got %q", stderr.String())
	}
}
