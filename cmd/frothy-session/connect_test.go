package main

import (
	"bytes"
	"errors"
	"strings"
	"testing"
)

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
