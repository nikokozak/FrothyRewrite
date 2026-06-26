//go:build darwin || linux

package main

import (
	"reflect"
	"testing"
)

func collectPumpEvents(chunks ...[]byte) []deviceEvent {
	readCh := make(chan byte, 64)
	errCh := make(chan error, 1)
	events := make(chan deviceEvent, 64)
	stop := make(chan struct{})
	defer close(stop)

	go runSerialEventPump(readCh, errCh, events, stop)
	for _, chunk := range chunks {
		for _, b := range chunk {
			readCh <- b
		}
	}
	close(readCh)

	var got []deviceEvent
	for ev := range events {
		got = append(got, ev)
	}
	return got
}

func TestSerialEventPumpSwallowsLineStartPrompt(t *testing.T) {
	got := collectPumpEvents([]byte("3\r\nok\r\n> "))
	want := []deviceEvent{
		deviceBytes{Bytes: []byte("3\r\nok\r\n")},
		devicePrompt{},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("events = %#v, want %#v", got, want)
	}
}

func TestSerialEventPumpSwallowsPromptSplitAcrossChunks(t *testing.T) {
	got := collectPumpEvents([]byte("ok\r\n>"), []byte(" "))
	want := []deviceEvent{
		deviceBytes{Bytes: []byte("ok\r\n")},
		devicePrompt{},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("events = %#v, want %#v", got, want)
	}
}

func TestSerialEventPumpSwallowsPromptBeforeMoreOutput(t *testing.T) {
	got := collectPumpEvents([]byte("ok\r\n> event\r\n"))
	want := []deviceEvent{
		deviceBytes{Bytes: []byte("ok\r\n")},
		devicePrompt{},
		deviceBytes{Bytes: []byte("event\r\n")},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("events = %#v, want %#v", got, want)
	}
}

func TestSerialEventPumpKeepsGreaterThanSpaceInsideOutput(t *testing.T) {
	got := collectPumpEvents([]byte("value > \r\n"))
	want := []deviceEvent{
		deviceBytes{Bytes: []byte("value > \r\n")},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("events = %#v, want %#v", got, want)
	}
}
