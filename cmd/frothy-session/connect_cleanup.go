//go:build darwin || linux

package main

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"strings"
)

// Bracketed paste ANSI toggles. Decision 9 lists them under cleanup;
// decision 4 lists them as part of the line-editor contract.
const (
	bracketedPasteOn  = "\x1b[?2004h"
	bracketedPasteOff = "\x1b[?2004l"
)

// terminalState carries the termios snapshot captured by stty -g so the
// caller can put the same fd back the way it found it.
type terminalState struct {
	file  *os.File
	saved string
}

func enterRawMode(file *os.File) (*terminalState, error) {
	saved, err := runStty(file, []string{"-g"})
	if err != nil {
		return nil, fmt.Errorf("save terminal state: %w", err)
	}
	if _, err := runStty(file, []string{"raw", "-echo"}); err != nil {
		return nil, fmt.Errorf("enter raw mode: %w", err)
	}
	return &terminalState{file: file, saved: strings.TrimSpace(saved)}, nil
}

func (s *terminalState) restore() error {
	if s == nil || s.saved == "" {
		return nil
	}
	_, err := runStty(s.file, []string{s.saved})
	return err
}

func runStty(file *os.File, args []string) (string, error) {
	cmd := exec.Command("stty", args...)
	var stdout, stderr bytes.Buffer
	cmd.Stdin = file
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		text := strings.TrimSpace(stderr.String())
		if text == "" {
			return "", err
		}
		return "", fmt.Errorf("%w: %s", err, text)
	}
	return stdout.String(), nil
}
