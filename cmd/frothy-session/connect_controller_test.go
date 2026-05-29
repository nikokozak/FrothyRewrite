//go:build darwin || linux

package main

import (
	"bytes"
	"io"
	"reflect"
	"testing"
)

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

func TestConnectControllerSubmitRecordsHistory(t *testing.T) {
	var entries []string
	c := newConnectController(io.Discard, func([]byte) error { return nil })
	c.addHistory = func(line string) { entries = appendHistory(entries, line) }
	c.writePrompt()

	for _, seq := range []struct{ keys, submit string }{
		{"foo", "foo"},
		{"foo", "foo"}, // dedup against prior
		{"", ""},       // empty submit skipped
		{"bar", "bar"},
	} {
		if seq.keys != "" {
			c.onInput(inputPrintable{Bytes: []byte(seq.keys)})
		}
		c.onInput(inputSubmit{})
	}

	want := []string{"foo", "bar"}
	if !reflect.DeepEqual(entries, want) {
		t.Fatalf("entries = %q, want %q", entries, want)
	}
}

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
