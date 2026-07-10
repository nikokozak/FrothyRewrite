package main

import (
	"strings"
	"testing"
	"time"
)

type editorSessionView struct {
	session          string
	state            string
	mirror           string
	mode             string
	profile          string
	lastKind         string
	lastSource       string
	lastLine         string
	lastStatus       string
	interruptSettled bool
	errorCode        string
	candidates       []string
}

func (v *editorSessionView) applyRecord(record map[string]any) {
	if session, ok := record["session"].(string); ok {
		v.session = session
	}
	if state, ok := record["state"].(string); ok {
		v.state = state
	}
	if mirror, ok := record["mirror"].(string); ok {
		v.mirror = mirror
	}
	if kind, ok := record["kind"].(string); ok {
		v.lastKind = kind
	}

	switch record["kind"] {
	case "status":
		if mode, ok := record["mode"].(string); ok {
			v.mode = mode
		}
		if device, ok := record["device"].(map[string]any); ok {
			if profile, ok := device["profile"].(string); ok {
				v.profile = profile
			}
		}
	case "send":
		if source, ok := record["source"].(string); ok {
			v.lastSource = source
		}
		if line, ok := record["line"].(string); ok {
			v.lastLine = line
		}
	case "compile_error":
		if status, ok := record["status"].(string); ok {
			v.lastStatus = status
		}
	case "response":
		if status, ok := record["status"].(string); ok {
			v.lastStatus = status
		}
	case "interrupt":
		if settled, ok := record["settled"].(bool); ok {
			v.interruptSettled = settled
		}
		if status, ok := record["status"].(string); ok {
			v.lastStatus = status
		}
	case "session_error":
		if code, ok := record["code"].(string); ok {
			v.errorCode = code
		}
		if values, ok := record["candidates"].([]any); ok {
			v.candidates = nil
			for _, value := range values {
				if candidate, ok := value.(string); ok {
					v.candidates = append(v.candidates, candidate)
				}
			}
		}
	}
}

func TestEditorSessionViewTracksLiveRecordState(t *testing.T) {
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionApply, line: "apply 0102"},
		},
	}
	dev := &fakeDevice{responses: []string{statusResponse("host-required"), "ok\n"}}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("time is 200\n"), &out, comp, dev, time.Second, false, &interruptTracker{})
	if err != nil {
		t.Fatal(err)
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,response,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}

	var view editorSessionView
	for _, record := range records {
		view.applyRecord(record)
		switch record["kind"] {
		case "status":
			if view.state != "idle" || view.mirror != "clean" ||
				view.mode != "host-required" || view.profile != "test" {
				t.Fatalf("status view = %#v", view)
			}
		case "send":
			if view.state != "waiting" || view.mirror != "pending" ||
				view.lastSource != "time is 200" || view.lastLine != "apply 0102" {
				t.Fatalf("send view = %#v", view)
			}
		case "response":
			if view.state != "idle" || view.mirror != "clean" ||
				view.lastStatus != "ok" {
				t.Fatalf("response view = %#v", view)
			}
		case "session_end":
			if view.state != "closed" || view.mirror != "clean" || view.lastKind != "session_end" {
				t.Fatalf("session_end view = %#v", view)
			}
		}
	}
}

func TestEditorSessionViewTracksStaleMirror(t *testing.T) {
	tracker := &interruptTracker{}
	comp := &fakeCompiler{
		target: targetProfile("1234abcd"),
		results: []compileResult{
			{action: actionApply, line: "apply 0102"},
		},
	}
	dev := &fakeDevice{
		responses: []string{
			statusResponse("host-required"),
			"error: interrupted (10)\n",
		},
		onSend: func(line string) {
			if line == "apply 0102" {
				tracker.request()
			}
		},
	}
	var out strings.Builder

	err := runRecordsTestSession(t, strings.NewReader("time is 200\n"), &out, comp, dev, time.Second, false, tracker)
	if err == nil {
		t.Fatal("runRecordsTestSession accepted interrupted apply")
	}

	records := decodeRecords(t, out.String())
	if got, want := recordKinds(records), "session_start,status,send,interrupt,session_error"; got != want {
		t.Fatalf("record kinds %q, want %q", got, want)
	}

	var view editorSessionView
	for _, record := range records {
		view.applyRecord(record)
		switch record["kind"] {
		case "send":
			if view.state != "waiting" || view.mirror != "pending" || view.lastLine != "apply 0102" {
				t.Fatalf("send view = %#v", view)
			}
		case "interrupt":
			if view.state != "stale" || view.mirror != "stale" ||
				!view.interruptSettled || view.lastStatus != "error: interrupted (10)" {
				t.Fatalf("interrupt view = %#v", view)
			}
		case "session_error":
			if view.state != "stale" || view.mirror != "stale" ||
				view.errorCode != "mirror_stale" {
				t.Fatalf("session_error view = %#v", view)
			}
		}
	}
}

func TestEditorSessionViewIgnoresUnknownRecordKind(t *testing.T) {
	view := editorSessionView{}
	view.applyRecord(map[string]any{
		"session": "s1",
		"kind":    "future_record",
		"state":   "idle",
		"mirror":  "clean",
	})

	if view.session != "s1" || view.lastKind != "future_record" ||
		view.state != "idle" || view.mirror != "clean" || view.errorCode != "" {
		t.Fatalf("future record view = %#v", view)
	}
}

func TestEditorSessionViewSeesPortSelectionErrors(t *testing.T) {
	tests := []struct {
		name           string
		ports          []string
		code           string
		wantCandidates string
	}{
		{name: "none", ports: []string{"/dev/null"}, code: "no_ports"},
		{
			name:           "multiple",
			ports:          []string{"/dev/cu.usbmodem101", "/dev/cu.usbserial-0001"},
			code:           "multiple_ports",
			wantCandidates: "/dev/cu.usbmodem101,/dev/cu.usbserial-0001",
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			var out strings.Builder
			records := newRecordWriter(&out, "s1")
			if err := records.sessionStart(); err != nil {
				t.Fatal(err)
			}
			_, err := pickSessionPort("", func() ([]string, error) {
				return test.ports, nil
			}, records)
			if err == nil {
				t.Fatal("pickSessionPort accepted invalid port selection")
			}

			decoded := decodeRecords(t, out.String())
			if got, want := recordKinds(decoded), "session_start,session_error"; got != want {
				t.Fatalf("record kinds %q, want %q", got, want)
			}
			var view editorSessionView
			for _, record := range decoded {
				view.applyRecord(record)
			}
			if view.state != "error" || view.mirror != "none" ||
				view.errorCode != test.code ||
				strings.Join(view.candidates, ",") != test.wantCandidates {
				t.Fatalf("port selection view = %#v", view)
			}
		})
	}
}
