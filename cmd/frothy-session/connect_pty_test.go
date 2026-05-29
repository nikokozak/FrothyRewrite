//go:build darwin || linux

package main

import (
	"bytes"
	"io"
	"strings"
	"sync"
	"testing"
	"time"

	"frothyrewrite/internal/testpty"
)

// User types "foo" without Enter; device emits a notice line; after the
// idle window the input line is erased, the notice prints, and the
// prompt + buffered "foo" are redrawn at the right cursor column.
func TestConnectRedrawAfterDeviceOutputMidTyping(t *testing.T) {
	master, slave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	defer slave.Close()

	dev := &serialDevice{
		file:   slave,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()

	stdinR, stdinW := io.Pipe()
	defer stdinR.Close()

	var stdoutMu sync.Mutex
	var stdout bytes.Buffer
	syncedStdout := &lockedWriter{w: &stdout, mu: &stdoutMu}

	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractive(dev, stdinR, syncedStdout)
	}()

	if _, err := stdinW.Write([]byte("foo")); err != nil {
		t.Fatal(err)
	}
	waitForStdoutContains(t, &stdoutMu, &stdout, promptPrimary+"foo", time.Second)

	if _, err := master.Write([]byte("notice\r\n")); err != nil {
		t.Fatal(err)
	}
	waitForStdoutContains(t, &stdoutMu, &stdout, "notice", time.Second)

	// Wait until the redrawn prompt+buf appears after the notice.
	waitForStdoutMatches(t, &stdoutMu, &stdout, func(s string) bool {
		i := strings.Index(s, "notice")
		if i < 0 {
			return false
		}
		return strings.Contains(s[i:], promptPrimary+"foo")
	}, time.Second)

	if _, err := stdinW.Write([]byte{0x04}); err != nil {
		t.Fatal(err)
	}
	_ = stdinW.Close()

	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code = %d, want 0", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("runConnectInteractive did not return")
	}

	stdoutMu.Lock()
	out := stdout.String()
	stdoutMu.Unlock()

	// The pending input line must be erased before "notice" prints.
	noticeIdx := strings.Index(out, "notice")
	if noticeIdx < 0 {
		t.Fatalf("notice missing: %q", out)
	}
	if !strings.Contains(out[:noticeIdx], "\r\x1b[K") {
		t.Fatalf("input line not erased before notice: %q", out)
	}
	// "foo" must appear twice: once when echoed, once when redrawn.
	if strings.Count(out, "foo") < 2 {
		t.Fatalf("expected 'foo' twice (echo + redraw), got: %q", out)
	}
}

// User types "foo" without Enter; device emits a notice line into the
// pending buffer; the user submits before the idle window fires. The
// notice must print between the submit newline and the next prompt; no
// device bytes are dropped.
func TestConnectSubmitFlushesPendingDeviceOutput(t *testing.T) {
	master, slave, _, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	defer slave.Close()

	dev := &serialDevice{
		file:   slave,
		readCh: make(chan byte, 256),
		errCh:  make(chan error, 1),
	}
	go dev.readLoop()

	stdinR, stdinW := io.Pipe()
	defer stdinR.Close()

	var stdoutMu sync.Mutex
	var stdout bytes.Buffer
	syncedStdout := &lockedWriter{w: &stdout, mu: &stdoutMu}

	done := make(chan int, 1)
	go func() {
		done <- runConnectInteractive(dev, stdinR, syncedStdout)
	}()

	if _, err := stdinW.Write([]byte("foo")); err != nil {
		t.Fatal(err)
	}
	waitForStdoutContains(t, &stdoutMu, &stdout, promptPrimary+"foo", time.Second)

	if _, err := master.Write([]byte("notice\r\n")); err != nil {
		t.Fatal(err)
	}
	// Beat the 50ms idle flush. 10ms is enough for the PTY → readLoop →
	// pump pipeline to deposit the notice into pending; the remaining
	// 40ms covers the submit before the idle timer fires.
	time.Sleep(10 * time.Millisecond)

	if _, err := stdinW.Write([]byte("\r")); err != nil {
		t.Fatal(err)
	}

	waitForStdoutMatches(t, &stdoutMu, &stdout, func(s string) bool {
		marker := promptPrimary + "foo\n"
		i := strings.Index(s, marker)
		if i < 0 {
			return false
		}
		rest := s[i+len(marker):]
		n := strings.Index(rest, "notice")
		if n < 0 {
			return false
		}
		p := strings.Index(rest, promptPrimary)
		return p > n
	}, time.Second)

	if _, err := stdinW.Write([]byte{0x04}); err != nil {
		t.Fatal(err)
	}
	_ = stdinW.Close()

	select {
	case code := <-done:
		if code != 0 {
			t.Fatalf("exit code = %d, want 0", code)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("runConnectInteractive did not return")
	}
}

type lockedWriter struct {
	w  io.Writer
	mu *sync.Mutex
}

func (l *lockedWriter) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.w.Write(p)
}

func waitForStdoutContains(t *testing.T, mu *sync.Mutex, buf *bytes.Buffer, want string, limit time.Duration) {
	t.Helper()
	waitForStdoutMatches(t, mu, buf, func(s string) bool { return strings.Contains(s, want) }, limit)
}

func waitForStdoutMatches(t *testing.T, mu *sync.Mutex, buf *bytes.Buffer, ok func(string) bool, limit time.Duration) {
	t.Helper()
	deadline := time.Now().Add(limit)
	for time.Now().Before(deadline) {
		mu.Lock()
		s := buf.String()
		mu.Unlock()
		if ok(s) {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	mu.Lock()
	s := buf.String()
	mu.Unlock()
	t.Fatalf("timed out waiting for stdout match; got: %q", s)
}
