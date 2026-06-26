//go:build darwin || linux

package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
	"time"
)

const (
	frothyStopGrace     = 1200 * time.Millisecond
	frothyStopPoll      = 100 * time.Millisecond
	frothyTakeoverGrace = 1500 * time.Millisecond
	frothyTakeoverKill  = 500 * time.Millisecond
)

type serialStopper struct {
	ownPID         int
	executable     string
	listPorts      func() ([]string, error)
	lookupHolders  func(string) []serialPortHolder
	processCommand func(int) (string, error)
	signal         func(int, syscall.Signal) error
	alive          func(int) bool
	sleep          func(time.Duration)
}

type frothyStopTarget struct {
	pid     int
	command string
	ports   []string
}

type frothyStoppedProcess struct {
	target frothyStopTarget
	killed bool
}

func runStopMain() int {
	return runStopCommand(os.Args[1:], os.Stdout, os.Stderr, defaultSerialStopper())
}

func runStopCommand(args []string, stdout io.Writer, stderr io.Writer, stopper serialStopper) int {
	fs := flag.NewFlagSet("frothy stop", flag.ContinueOnError)
	fs.SetOutput(stderr)
	if helpRequested(args) {
		printVerbHelp(stdout, helpFor("stop"), fs)
		return 0
	}
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() != 0 {
		fmt.Fprintln(stderr, "stop: takes no positional arguments")
		return 2
	}

	stopped, err := stopper.stopRunningSerialSessions()
	if err != nil {
		fmt.Fprintf(stderr, "stop: %v\n", err)
		return 1
	}
	if len(stopped) == 0 {
		fmt.Fprintln(stdout, "no running frothy sessions found")
		return 0
	}
	for _, proc := range stopped {
		fmt.Fprintf(stdout, "stopped frothy connect (pid %d) on %s\n",
			proc.target.pid, strings.Join(proc.target.ports, ", "))
	}
	return 0
}

func defaultSerialStopper() serialStopper {
	executable, _ := os.Executable()
	return serialStopper{
		ownPID:         os.Getpid(),
		executable:     executable,
		listPorts:      serialStopPorts,
		lookupHolders:  serialPortHolders,
		processCommand: processCommand,
		signal:         signalProcess,
		alive:          processAlive,
		sleep:          time.Sleep,
	}
}

func (s serialStopper) withDefaults() serialStopper {
	d := defaultSerialStopper()
	if s.ownPID == 0 {
		s.ownPID = d.ownPID
	}
	if s.executable == "" {
		s.executable = d.executable
	}
	if s.listPorts == nil {
		s.listPorts = d.listPorts
	}
	if s.lookupHolders == nil {
		s.lookupHolders = d.lookupHolders
	}
	if s.processCommand == nil {
		s.processCommand = d.processCommand
	}
	if s.signal == nil {
		s.signal = d.signal
	}
	if s.alive == nil {
		s.alive = d.alive
	}
	if s.sleep == nil {
		s.sleep = d.sleep
	}
	return s
}

func serialStopPorts() ([]string, error) {
	patterns := []string{"/dev/cu.*", "/dev/ttyUSB*", "/dev/ttyACM*"}
	seen := make(map[string]bool)
	var ports []string
	for _, pattern := range patterns {
		matches, err := filepath.Glob(pattern)
		if err != nil {
			return nil, err
		}
		for _, match := range matches {
			if seen[match] {
				continue
			}
			seen[match] = true
			ports = append(ports, match)
		}
	}
	sort.Strings(ports)
	return ports, nil
}

func signalProcess(pid int, sig syscall.Signal) error {
	if err := syscall.Kill(pid, sig); err != nil && !errors.Is(err, syscall.ESRCH) {
		return err
	}
	return nil
}

func processAlive(pid int) bool {
	if err := syscall.Kill(pid, 0); err != nil {
		return errors.Is(err, syscall.EPERM)
	}
	return true
}

func (s serialStopper) verifyFrothyProcess(pid int) (string, bool) {
	s = s.withDefaults()
	command, err := s.processCommand(pid)
	if err != nil || command == "" {
		return "", false
	}
	if !isFrothyHolder(command, s.executable) {
		return "", false
	}
	return command, true
}

func (s serialStopper) targetsFromHolders(port string, holders []serialPortHolder) []frothyStopTarget {
	s = s.withDefaults()
	targets := make(map[int]*frothyStopTarget)
	for _, holder := range holders {
		if holder.pid <= 0 || holder.pid == s.ownPID || !holder.frothy {
			continue
		}
		command, ok := s.verifyFrothyProcess(holder.pid)
		if !ok {
			continue
		}
		target := targets[holder.pid]
		if target == nil {
			target = &frothyStopTarget{pid: holder.pid, command: command}
			targets[holder.pid] = target
		}
		if !stringSliceContains(target.ports, port) {
			target.ports = append(target.ports, port)
		}
	}
	return sortedStopTargets(targets)
}

func (s serialStopper) targetsForPorts(ports []string) []frothyStopTarget {
	s = s.withDefaults()
	targets := make(map[int]*frothyStopTarget)
	for _, port := range ports {
		for _, target := range s.targetsFromHolders(port, s.lookupHolders(port)) {
			merged := targets[target.pid]
			if merged == nil {
				copyTarget := target
				copyTarget.ports = append([]string(nil), target.ports...)
				targets[target.pid] = &copyTarget
				continue
			}
			for _, p := range target.ports {
				if !stringSliceContains(merged.ports, p) {
					merged.ports = append(merged.ports, p)
				}
			}
		}
	}
	return sortedStopTargets(targets)
}

func sortedStopTargets(targets map[int]*frothyStopTarget) []frothyStopTarget {
	out := make([]frothyStopTarget, 0, len(targets))
	for _, target := range targets {
		sort.Strings(target.ports)
		out = append(out, *target)
	}
	sort.Slice(out, func(i, j int) bool {
		return out[i].pid < out[j].pid
	})
	return out
}

func stringSliceContains(items []string, item string) bool {
	for _, existing := range items {
		if existing == item {
			return true
		}
	}
	return false
}

func (s serialStopper) stopRunningSerialSessions() ([]frothyStoppedProcess, error) {
	s = s.withDefaults()
	ports, err := s.listPorts()
	if err != nil {
		return nil, err
	}
	return s.stopTargets(s.targetsForPorts(ports), frothyStopGrace)
}

func (s serialStopper) stopTargets(targets []frothyStopTarget, grace time.Duration) ([]frothyStoppedProcess, error) {
	s = s.withDefaults()
	if len(targets) == 0 {
		return nil, nil
	}
	stopped := make([]frothyStoppedProcess, 0, len(targets))
	var firstErr error
	for _, target := range targets {
		if err := s.signal(target.pid, syscall.SIGTERM); err != nil {
			firstErr = errors.Join(firstErr, fmt.Errorf("signal pid %d: %w", target.pid, err))
			continue
		}
		stopped = append(stopped, frothyStoppedProcess{target: target})
	}
	if firstErr != nil {
		return stopped, firstErr
	}

	s.waitForExit(targets, grace)
	for i := range stopped {
		if !s.alive(stopped[i].target.pid) {
			continue
		}
		if err := s.signal(stopped[i].target.pid, syscall.SIGKILL); err != nil {
			firstErr = errors.Join(firstErr, fmt.Errorf("kill pid %d: %w", stopped[i].target.pid, err))
			continue
		}
		stopped[i].killed = true
	}
	if firstErr != nil {
		return stopped, firstErr
	}
	return stopped, nil
}

func (s serialStopper) waitForExit(targets []frothyStopTarget, grace time.Duration) {
	if grace <= 0 || len(targets) == 0 {
		return
	}
	steps := int(grace / frothyStopPoll)
	if steps < 1 {
		steps = 1
	}
	for i := 0; i < steps; i++ {
		allExited := true
		for _, target := range targets {
			if s.alive(target.pid) {
				allExited = false
				break
			}
		}
		if allExited {
			return
		}
		s.sleep(frothyStopPoll)
	}
}

func (s serialStopper) takeoverPort(port string, targets []frothyStopTarget, open func() (*serialDevice, func(), error)) (*serialDevice, func(), error) {
	s = s.withDefaults()
	if len(targets) == 0 {
		return nil, nil, fmt.Errorf("port %s is in use, but no verified Frothy holder can be stopped", port)
	}
	for _, target := range targets {
		if err := s.signal(target.pid, syscall.SIGTERM); err != nil {
			return nil, nil, fmt.Errorf("could not stop frothy pid %d: %w", target.pid, err)
		}
	}
	if dev, closeDev, err := s.retryOpen(open, frothyTakeoverGrace); err == nil {
		return dev, closeDev, nil
	} else if !isSerialBusyError(err) {
		return nil, nil, err
	}

	for _, target := range targets {
		if !s.alive(target.pid) {
			continue
		}
		if err := s.signal(target.pid, syscall.SIGKILL); err != nil {
			return nil, nil, fmt.Errorf("could not kill frothy pid %d: %w", target.pid, err)
		}
	}
	if dev, closeDev, err := s.retryOpen(open, frothyTakeoverKill); err == nil {
		return dev, closeDev, nil
	} else if !isSerialBusyError(err) {
		return nil, nil, err
	}
	return nil, nil, fmt.Errorf("port %s is still in use after stopping frothy holder(s)", port)
}

func (s serialStopper) retryOpen(open func() (*serialDevice, func(), error), window time.Duration) (*serialDevice, func(), error) {
	steps := int(window / frothyStopPoll)
	if steps < 1 {
		steps = 1
	}
	var lastErr error
	for i := 0; i <= steps; i++ {
		dev, closeDev, err := open()
		if err == nil {
			return dev, closeDev, nil
		}
		lastErr = err
		if !isSerialBusyError(err) {
			return nil, nil, err
		}
		if i < steps {
			s.sleep(frothyStopPoll)
		}
	}
	return nil, nil, lastErr
}

func formatStopTargetPIDs(targets []frothyStopTarget) string {
	var pids []string
	for _, target := range targets {
		pids = append(pids, fmt.Sprintf("%d", target.pid))
	}
	if len(pids) == 1 {
		return "pid " + pids[0]
	}
	return "pids " + strings.Join(pids, ", ")
}
