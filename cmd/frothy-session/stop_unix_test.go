//go:build darwin || linux

package main

import (
	"bytes"
	"fmt"
	"reflect"
	"strings"
	"syscall"
	"testing"
	"time"
)

type recordedSignal struct {
	pid int
	sig syscall.Signal
}

func TestSerialStopperSignalsOnlyVerifiedFrothyTargets(t *testing.T) {
	var signals []recordedSignal
	stopper := serialStopper{
		ownPID:     99,
		executable: "/tmp/frothy",
		processCommand: func(pid int) (string, error) {
			switch pid {
			case 10:
				return "/tmp/frothy", nil
			case 11:
				return "/Applications/CoolTerm", nil
			default:
				return "", fmt.Errorf("unknown pid")
			}
		},
		signal: func(pid int, sig syscall.Signal) error {
			signals = append(signals, recordedSignal{pid: pid, sig: sig})
			return nil
		},
		alive: func(int) bool { return false },
		sleep: func(time.Duration) {},
	}
	holders := []serialPortHolder{
		{pid: 10, command: "frothy", frothy: true},
		{pid: 11, command: "frothy", frothy: true},
		{pid: 12, command: "frothy", frothy: false},
		{pid: 99, command: "frothy", frothy: true},
	}

	targets := stopper.targetsFromHolders("/dev/cu.test", holders)
	if len(targets) != 1 || targets[0].pid != 10 {
		t.Fatalf("targets = %#v, want only pid 10", targets)
	}
	if _, err := stopper.stopTargets(targets, time.Millisecond); err != nil {
		t.Fatal(err)
	}
	want := []recordedSignal{{pid: 10, sig: syscall.SIGTERM}}
	if !reflect.DeepEqual(signals, want) {
		t.Fatalf("signals = %#v, want %#v", signals, want)
	}
}

func TestSerialStopperEscalatesAfterSIGTERM(t *testing.T) {
	var signals []recordedSignal
	stopper := serialStopper{
		ownPID:         99,
		executable:     "/tmp/frothy",
		processCommand: func(int) (string, error) { return "/tmp/frothy", nil },
		signal: func(pid int, sig syscall.Signal) error {
			signals = append(signals, recordedSignal{pid: pid, sig: sig})
			return nil
		},
		alive: func(int) bool { return true },
		sleep: func(time.Duration) {},
	}
	targets := []frothyStopTarget{{pid: 10, command: "/tmp/frothy", ports: []string{"/dev/cu.test"}}}

	stopped, err := stopper.stopTargets(targets, time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	if len(stopped) != 1 || !stopped[0].killed {
		t.Fatalf("stopped = %#v, want killed target", stopped)
	}
	want := []recordedSignal{
		{pid: 10, sig: syscall.SIGTERM},
		{pid: 10, sig: syscall.SIGKILL},
	}
	if !reflect.DeepEqual(signals, want) {
		t.Fatalf("signals = %#v, want %#v", signals, want)
	}
}

func TestConnectBusyNonTTYDoesNotTakeOverWithoutFlag(t *testing.T) {
	busy := serialPortBusyError{
		port: "/dev/cu.test",
		holders: []serialPortHolder{
			{pid: 10, command: "/tmp/frothy", frothy: true},
		},
		message: "port /dev/cu.test is in use by another frothy (pid 10); run 'frothy stop' to free it",
	}
	openCalls := 0
	open := func(string, int) (*serialDevice, func(), error) {
		openCalls++
		return nil, nil, busy
	}
	var signals []recordedSignal
	stopper := serialStopper{
		ownPID:         99,
		executable:     "/tmp/frothy",
		processCommand: func(int) (string, error) { return "/tmp/frothy", nil },
		signal: func(pid int, sig syscall.Signal) error {
			signals = append(signals, recordedSignal{pid: pid, sig: sig})
			return nil
		},
		alive: func(int) bool { return true },
		sleep: func(time.Duration) {},
	}

	var stdout, stderr bytes.Buffer
	code := runConnectCommandWithStopper(
		[]string{"--port", "/dev/cu.test", "--settle", "0"},
		strings.NewReader(""), &stdout, &stderr, nil, open, nil, stopper,
	)
	if code != 1 {
		t.Fatalf("exit code = %d, want 1", code)
	}
	if openCalls != 1 {
		t.Fatalf("open calls = %d, want 1", openCalls)
	}
	if len(signals) != 0 {
		t.Fatalf("signals = %#v, want none", signals)
	}
	if !strings.Contains(stderr.String(), "run 'frothy stop'") {
		t.Fatalf("stderr = %q, want busy guidance", stderr.String())
	}
}

func TestTakeoverStopsFrothyAndRetriesOpen(t *testing.T) {
	busy := serialPortBusyError{
		port: "/dev/cu.test",
		holders: []serialPortHolder{
			{pid: 10, command: "/tmp/frothy", frothy: true},
		},
		message: "busy",
	}
	var signals []recordedSignal
	stopper := serialStopper{
		ownPID:         99,
		executable:     "/tmp/frothy",
		processCommand: func(int) (string, error) { return "/tmp/frothy", nil },
		signal: func(pid int, sig syscall.Signal) error {
			signals = append(signals, recordedSignal{pid: pid, sig: sig})
			return nil
		},
		alive: func(int) bool { return false },
		sleep: func(time.Duration) {},
	}
	targets := stopper.targetsFromHolders("/dev/cu.test", busy.holders)
	openCalls := 0
	open := func() (*serialDevice, func(), error) {
		openCalls++
		if openCalls < 3 {
			return nil, nil, busy
		}
		return &serialDevice{}, func() {}, nil
	}

	dev, closeDev, err := stopper.takeoverPort("/dev/cu.test", targets, open)
	if err != nil {
		t.Fatal(err)
	}
	if dev == nil || closeDev == nil {
		t.Fatal("takeover returned nil device or cleanup")
	}
	if openCalls != 3 {
		t.Fatalf("open calls = %d, want 3", openCalls)
	}
	want := []recordedSignal{{pid: 10, sig: syscall.SIGTERM}}
	if !reflect.DeepEqual(signals, want) {
		t.Fatalf("signals = %#v, want %#v", signals, want)
	}
}

func TestRunStopReportsNoTargets(t *testing.T) {
	stopper := serialStopper{
		ownPID:    99,
		listPorts: func() ([]string, error) { return []string{"/dev/cu.test"}, nil },
		lookupHolders: func(string) []serialPortHolder {
			return []serialPortHolder{{pid: 10, command: "/Applications/CoolTerm", frothy: false}}
		},
		processCommand: func(int) (string, error) { return "/Applications/CoolTerm", nil },
		signal:         func(int, syscall.Signal) error { t.Fatal("signal must not be called"); return nil },
		alive:          func(int) bool { return false },
		sleep:          func(time.Duration) {},
	}

	var stdout, stderr bytes.Buffer
	code := runStopCommand(nil, &stdout, &stderr, stopper)
	if code != 0 {
		t.Fatalf("exit code = %d, want 0; stderr=%q", code, stderr.String())
	}
	if strings.TrimSpace(stdout.String()) != "no running frothy sessions found" {
		t.Fatalf("stdout = %q", stdout.String())
	}
}

func TestRunStopStopsDedupedFrothyTargets(t *testing.T) {
	var signals []recordedSignal
	stopper := serialStopper{
		ownPID: 99,
		listPorts: func() ([]string, error) {
			return []string{"/dev/cu.a", "/dev/cu.b"}, nil
		},
		lookupHolders: func(port string) []serialPortHolder {
			return []serialPortHolder{
				{pid: 10, command: "/tmp/frothy", frothy: true},
				{pid: 99, command: "/tmp/frothy", frothy: true},
			}
		},
		executable:     "/tmp/frothy",
		processCommand: func(int) (string, error) { return "/tmp/frothy", nil },
		signal: func(pid int, sig syscall.Signal) error {
			signals = append(signals, recordedSignal{pid: pid, sig: sig})
			return nil
		},
		alive: func(int) bool { return false },
		sleep: func(time.Duration) {},
	}

	var stdout, stderr bytes.Buffer
	code := runStopCommand(nil, &stdout, &stderr, stopper)
	if code != 0 {
		t.Fatalf("exit code = %d, want 0; stderr=%q", code, stderr.String())
	}
	wantOut := "stopped frothy connect (pid 10) on /dev/cu.a, /dev/cu.b\n"
	if stdout.String() != wantOut {
		t.Fatalf("stdout = %q, want %q", stdout.String(), wantOut)
	}
	wantSignals := []recordedSignal{{pid: 10, sig: syscall.SIGTERM}}
	if !reflect.DeepEqual(signals, wantSignals) {
		t.Fatalf("signals = %#v, want %#v", signals, wantSignals)
	}
}
