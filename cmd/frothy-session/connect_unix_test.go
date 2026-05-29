//go:build darwin || linux

package main

import (
	"bufio"
	"bytes"
	"fmt"
	"os"
	"strings"
	"testing"

	"frothyrewrite/internal/testpty"
)

func TestConnectRejectsHostRequiredOverPTY(t *testing.T) {
	master, slave, slavePath, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	defer slave.Close()

	deviceErrs := serveHostRequiredFakeDevice(master)

	var stderr bytes.Buffer
	args := []string{
		"--port=" + slavePath,
		"--settle=0",
		"--timeout=5s",
	}
	code := runConnectCommand(args, &stderr, nil, defaultConnectDeviceFactory)

	if err := waitForProcessFakeDevice(deviceErrs); err != nil {
		t.Fatal(err)
	}

	if code != 1 {
		t.Fatalf("exit code = %d, want 1; stderr=%q", code, stderr.String())
	}
	out := stderr.String()
	if !strings.Contains(out, "host-required") {
		t.Fatalf("stderr missing 'host-required': %q", out)
	}
	if !strings.Contains(out, "frothy session") && !strings.Contains(out, "frothy send") {
		t.Fatalf("stderr missing redirect to session/send: %q", out)
	}
}

func serveHostRequiredFakeDevice(master *os.File) <-chan error {
	errs := make(chan error, 1)
	go func() {
		defer close(errs)
		if _, err := master.Write([]byte("> ")); err != nil {
			errs <- fmt.Errorf("fake serial prompt: %w", err)
			return
		}
		reader := bufio.NewReader(master)
		for {
			line, err := reader.ReadString('\n')
			if err != nil {
				if fakeDeviceClosed(err) {
					return
				}
				errs <- fmt.Errorf("fake serial read: %w", err)
				return
			}
			trimmed := strings.TrimSpace(line)
			if trimmed == "" {
				continue
			}
			if trimmed != "status" {
				errs <- fmt.Errorf("fake serial unexpected line %q", trimmed)
				return
			}
			if _, err := master.Write([]byte(statusResponse("host-required") + "> ")); err != nil {
				errs <- fmt.Errorf("fake serial status: %w", err)
				return
			}
			return
		}
	}()
	return errs
}
