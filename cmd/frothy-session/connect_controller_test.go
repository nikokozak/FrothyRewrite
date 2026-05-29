//go:build darwin || linux

package main

import (
	"bytes"
	"testing"
)

// Submit with pending device output flushes the notice before the new
// prompt and sends the buffered line to the device. The previous PTY
// version of this proof leaned on a 10ms sleep; here every step is a
// synchronous call to the controller, so the order is what the code
// does, not what the host scheduler does first.
func TestConnectControllerSubmitFlushesPending(t *testing.T) {
	var out bytes.Buffer
	var sent [][]byte
	c := newConnectController(&out, func(b []byte) error {
		sent = append(sent, append([]byte(nil), b...))
		return nil
	})

	c.writePrompt()
	if exit, _ := c.onInput(inputPrintable{Bytes: []byte("foo")}); exit {
		t.Fatal("early exit on printable")
	}
	if exit, _ := c.onDevice(deviceBytes{Bytes: []byte("notice\r\n")}); exit {
		t.Fatal("early exit on device bytes")
	}
	if !c.idleArmed {
		t.Fatal("idle should be armed while pending bytes wait behind the input line")
	}
	if exit, _ := c.onInput(inputSubmit{}); exit {
		t.Fatal("early exit on submit")
	}

	want := promptPrimary + "foo\nnotice\r\n" + promptPrimary
	if got := out.String(); got != want {
		t.Fatalf("stdout = %q, want %q", got, want)
	}
	if len(sent) != 1 || string(sent[0]) != "foo\n" {
		t.Fatalf("sent = %q, want [\"foo\\n\"]", sent)
	}
	if c.idleArmed {
		t.Fatal("idle should be cleared after submit consumed pending")
	}
}

// Device output that arrives while the line buffer is empty prints
// straight through and does not arm the idle flush.
func TestConnectControllerDeviceBytesWithEmptyBuf(t *testing.T) {
	var out bytes.Buffer
	c := newConnectController(&out, func([]byte) error { return nil })
	c.writePrompt()

	if exit, _ := c.onDevice(deviceBytes{Bytes: []byte("hello\r\n")}); exit {
		t.Fatal("early exit on device bytes")
	}
	if c.idleArmed {
		t.Fatal("idle should stay disarmed when buffer is empty")
	}
	if got := out.String(); got != promptPrimary+"hello\r\n" {
		t.Fatalf("stdout = %q", got)
	}
}

// Idle fire flushes the pending notice and redraws the prompt + buffer.
func TestConnectControllerIdleFireRedraws(t *testing.T) {
	var out bytes.Buffer
	c := newConnectController(&out, func([]byte) error { return nil })
	c.writePrompt()
	c.onInput(inputPrintable{Bytes: []byte("foo")})
	c.onDevice(deviceBytes{Bytes: []byte("notice\r\n")})
	c.onIdleFire()

	want := promptPrimary + "foo" + "\r\x1b[K" + "notice\r\n" + promptPrimary + "foo"
	if got := out.String(); got != want {
		t.Fatalf("stdout = %q, want %q", got, want)
	}
}
