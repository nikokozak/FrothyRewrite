package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"testing"
	"time"

	"frothyrewrite/internal/testpty"
)

func TestFrothySessionProcessChild(t *testing.T) {
	if os.Getenv("FROTHY_SESSION_PROCESS_CHILD") != "1" {
		return
	}

	args := childSessionArgs()
	flag.CommandLine = flag.NewFlagSet("frothy-session", flag.ExitOnError)
	os.Args = append([]string{"frothy-session"}, args...)
	main()
	os.Exit(0)
}

func childSessionArgs() []string {
	for i, arg := range os.Args {
		if arg == "--" {
			return os.Args[i+1:]
		}
	}
	return nil
}

func TestFrothySessionRecordsAcrossProcessBoundary(t *testing.T) {
	master, slave, slavePath, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	// Put the PTY in serial-like raw/no-echo mode before the fake device
	// writes its first prompt. The child process still configures the port
	// through its normal openSerial path.
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	closeSlave := func() {
		if slave != nil {
			_ = slave.Close()
			slave = nil
		}
	}
	defer closeSlave()

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	cmd := exec.CommandContext(
		ctx,
		os.Args[0],
		"-test.run=TestFrothySessionProcessChild",
		"--",
		"--records",
		"--port", slavePath,
		"--settle", "0s",
		"--timeout", "5s",
	)
	cmd.Env = append(os.Environ(), "FROTHY_SESSION_PROCESS_CHILD=1")

	stdin, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr

	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	closeSlave()
	deviceErrs := serveProcessFakeDevice(master)

	if _, err := io.WriteString(stdin, "words\n"); err != nil {
		t.Fatal(err)
	}
	if err := stdin.Close(); err != nil {
		t.Fatal(err)
	}

	err = cmd.Wait()
	_ = master.Close()
	if ctx.Err() != nil {
		t.Fatalf("frothy-session process timed out\nstdout:\n%s\nstderr:\n%s", stdout.String(), stderr.String())
	}
	if err != nil {
		t.Fatalf("frothy-session process failed: %v\nstdout:\n%s\nstderr:\n%s", err, stdout.String(), stderr.String())
	}
	if deviceErr := waitForProcessFakeDevice(deviceErrs); deviceErr != nil {
		t.Fatal(deviceErr)
	}

	records := decodeRecords(t, stdout.String())
	if got, want := recordKinds(records), "session_start,status,send,response,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q\nstdout:\n%s\nstderr:\n%s", got, want, stdout.String(), stderr.String())
	}

	var view editorSessionView
	for _, record := range records {
		view.applyRecord(record)
	}
	if view.state != "closed" || view.mirror != "none" ||
		view.mode != "device" || view.profile != "test" ||
		view.lastSource != "words" || view.lastLine != "words" ||
		view.lastStatus != "ok" {
		t.Fatalf("editor view = %#v\nstdout:\n%s\nstderr:\n%s", view, stdout.String(), stderr.String())
	}
}

func TestFrothySessionProcessSignalInterruptsForegroundRun(t *testing.T) {
	master, slave, slavePath, err := testpty.Open()
	if err != nil {
		t.Skip(err)
	}
	if err := configureSerial(slave, 115200); err != nil {
		t.Fatal(err)
	}
	defer master.Close()
	closeSlave := func() {
		if slave != nil {
			_ = slave.Close()
			slave = nil
		}
	}
	defer closeSlave()

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	cmd := exec.CommandContext(
		ctx,
		os.Args[0],
		"-test.run=TestFrothySessionProcessChild",
		"--",
		"--records",
		"--port", slavePath,
		"--settle", "0s",
		"--timeout", "5s",
	)
	cmd.Env = append(os.Environ(), "FROTHY_SESSION_PROCESS_CHILD=1")

	stdin, err := cmd.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr

	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	closeSlave()
	foregroundLine := make(chan string, 1)
	interrupts := make(chan int, 1)
	deviceErrs := serveProcessInterruptFakeDevice(master, foregroundLine, interrupts)

	if _, err := io.WriteString(stdin, "forever [ 1 ]\n"); err != nil {
		t.Fatal(err)
	}
	if err := stdin.Close(); err != nil {
		t.Fatal(err)
	}

	select {
	case line := <-foregroundLine:
		if line != "forever [ 1 ]" {
			t.Fatalf("foreground line %q, want source form", line)
		}
	case <-time.After(5 * time.Second):
		t.Fatalf("fake serial did not receive foreground run\nstdout:\n%s\nstderr:\n%s", stdout.String(), stderr.String())
	}

	if err := cmd.Process.Signal(os.Interrupt); err != nil {
		t.Fatal(err)
	}

	err = cmd.Wait()
	_ = master.Close()
	if ctx.Err() != nil {
		t.Fatalf("frothy-session process timed out\nstdout:\n%s\nstderr:\n%s", stdout.String(), stderr.String())
	}
	if err != nil {
		t.Fatalf("frothy-session process failed: %v\nstdout:\n%s\nstderr:\n%s", err, stdout.String(), stderr.String())
	}
	if deviceErr := waitForProcessFakeDevice(deviceErrs); deviceErr != nil {
		t.Fatal(deviceErr)
	}

	select {
	case count := <-interrupts:
		if count != 1 {
			t.Fatalf("interrupt bytes=%d, want 1", count)
		}
	default:
		t.Fatal("fake serial did not observe Ctrl-C byte")
	}

	records := decodeRecords(t, stdout.String())
	if got, want := recordKinds(records), "session_start,status,send,interrupt,session_end"; got != want {
		t.Fatalf("record kinds %q, want %q\nstdout:\n%s\nstderr:\n%s", got, want, stdout.String(), stderr.String())
	}
	interrupt := recordWithKind(records, "interrupt")
	if interrupt["state"] != "idle" || interrupt["mirror"] != "none" ||
		interrupt["settled"] != true || interrupt["status"] != "error: interrupted (10)" {
		t.Fatalf("interrupt record = %#v\nstdout:\n%s\nstderr:\n%s", interrupt, stdout.String(), stderr.String())
	}
}

func serveProcessFakeDevice(master *os.File) <-chan error {
	errs := make(chan error, 1)
	go func() {
		defer close(errs)
		if _, err := master.Write([]byte("> ")); err != nil {
			errs <- fmt.Errorf("fake serial prompt: %w", err)
			return
		}

		reader := bufio.NewReader(master)
		for {
			line, err := reader.ReadString('\n')
			if err != nil {
				if fakeDeviceClosed(err) {
					return
				}
				errs <- fmt.Errorf("fake serial read: %w", err)
				return
			}

			switch strings.TrimSpace(line) {
			case "status":
				if _, err := master.Write([]byte(statusResponse("device") + "> ")); err != nil {
					errs <- fmt.Errorf("fake serial status: %w", err)
					return
				}
			case "":
			default:
				response := strings.TrimSpace(line) + "\r\nok\r\n> "
				if _, err := master.Write([]byte(response)); err != nil {
					errs <- fmt.Errorf("fake serial response: %w", err)
					return
				}
			}
		}
	}()
	return errs
}

func serveProcessInterruptFakeDevice(master *os.File, foregroundLine chan<- string, interrupts chan<- int) <-chan error {
	errs := make(chan error, 1)
	go func() {
		defer close(errs)
		if _, err := master.Write([]byte("> ")); err != nil {
			errs <- fmt.Errorf("fake serial prompt: %w", err)
			return
		}

		reader := bufio.NewReader(master)
		for {
			line, err := reader.ReadString('\n')
			if err != nil {
				if fakeDeviceClosed(err) {
					return
				}
				errs <- fmt.Errorf("fake serial read: %w", err)
				return
			}

			switch strings.TrimSpace(line) {
			case "status":
				if _, err := master.Write([]byte(statusResponse("device") + "> ")); err != nil {
					errs <- fmt.Errorf("fake serial status: %w", err)
					return
				}
			case "forever [ 1 ]":
				foregroundLine <- "forever [ 1 ]"
				count := 0
				for {
					b, err := reader.ReadByte()
					if err != nil {
						if fakeDeviceClosed(err) {
							return
						}
						errs <- fmt.Errorf("fake serial interrupt read: %w", err)
						return
					}
					if b != 0x03 {
						errs <- fmt.Errorf("fake serial unexpected foreground byte 0x%02x", b)
						return
					}
					count += 1
					interrupts <- count
					if _, err := master.Write([]byte("error: interrupted (10)\r\n> ")); err != nil {
						errs <- fmt.Errorf("fake serial interrupt response: %w", err)
						return
					}
					break
				}
			case "":
			default:
				errs <- fmt.Errorf("fake serial unexpected line %q", strings.TrimSpace(line))
				return
			}
		}
	}()
	return errs
}

func waitForProcessFakeDevice(errs <-chan error) error {
	select {
	case err := <-errs:
		return err
	case <-time.After(2 * time.Second):
		return errors.New("fake serial device did not stop")
	}
}

func fakeDeviceClosed(err error) bool {
	return errors.Is(err, os.ErrClosed) || errors.Is(err, io.EOF) || errors.Is(err, syscall.EIO)
}
