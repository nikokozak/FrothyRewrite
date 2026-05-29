package main

import (
	"bytes"
	"errors"
	"strings"
	"testing"
)

func TestConnectRejectsHostRequiredMode(t *testing.T) {
	dev := &fakeDevice{responses: []string{statusResponse("host-required")}}
	open := func(port string, baud int) (sessionDevice, func(), error) {
		return dev, func() {}, nil
	}
	var stderr bytes.Buffer
	args := []string{"--port=/dev/cu.fake", "--settle=0"}
	code := runConnectCommand(args, &stderr, nil, open)
	if code != 1 {
		t.Fatalf("exit code = %d, want 1", code)
	}
	out := stderr.String()
	if !strings.Contains(out, "host-required") {
		t.Fatalf("stderr missing 'host-required': %q", out)
	}
	if !strings.Contains(out, "frothy session") && !strings.Contains(out, "frothy send") {
		t.Fatalf("stderr missing redirect to session/send: %q", out)
	}
}

func TestConnectRejectsPositionalArgs(t *testing.T) {
	open := func(string, int) (sessionDevice, func(), error) {
		return nil, nil, errors.New("should not be called")
	}
	var stderr bytes.Buffer
	code := runConnectCommand([]string{"extra"}, &stderr, nil, open)
	if code != 2 {
		t.Fatalf("exit code = %d, want 2", code)
	}
	if !strings.Contains(stderr.String(), "positional") {
		t.Fatalf("stderr missing positional error: %q", stderr.String())
	}
}
