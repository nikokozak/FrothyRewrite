package main

import (
	"bytes"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func fakeInstallFactory(dev *fakeDevice) installDeviceFactory {
	return func(port string, baud int) (sessionDevice, func(), error) {
		return dev, func() {}, nil
	}
}

func writeFrothyToml(t *testing.T, dir, target string) {
	t.Helper()
	manifest := "name = \"install-test\"\ntarget = \"" + target + "\"\n"
	if err := os.WriteFile(filepath.Join(dir, "frothy.toml"), []byte(manifest), 0o644); err != nil {
		t.Fatalf("write frothy.toml: %v", err)
	}
}

func writeLibraryFr(t *testing.T, dir, target, content string) string {
	t.Helper()
	buildDir := filepath.Join(dir, ".frothy", "build", target)
	if err := os.MkdirAll(buildDir, 0o755); err != nil {
		t.Fatalf("mkdir build dir: %v", err)
	}
	path := filepath.Join(buildDir, "library.fr")
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatalf("write library.fr: %v", err)
	}
	return path
}

func TestRunInstallSendsLibraryThenExitsZero(t *testing.T) {
	projectDir := t.TempDir()
	writeFrothyToml(t, projectDir, "esp32_devkit_v1")
	writeLibraryFr(t, projectDir, "esp32_devkit_v1", "lib_word is fn [ 42 ]\n")

	dev := &fakeDevice{responses: []string{"ok\n", "ok\n"}}
	var stderr bytes.Buffer

	code := runInstallCommand(
		[]string{"--port", "/dev/cu.usbserial-0001", "--project", projectDir},
		io.Discard, &stderr, singlePortLister("/dev/cu.usbserial-0001"), fakeInstallFactory(dev),
		115200, time.Second, 0,
	)
	if code != 0 {
		t.Fatalf("expected exit 0, got %d (stderr=%q)", code, stderr.String())
	}
	want := []string{"install-library", "lib_word is fn [ 42 ]"}
	if len(dev.sent) != len(want) {
		t.Fatalf("sent %v, want %v", dev.sent, want)
	}
	for i, line := range want {
		if dev.sent[i] != line {
			t.Fatalf("sent[%d]=%q, want %q (full=%v)", i, dev.sent[i], line, dev.sent)
		}
	}
	if dev.syncs != 1 {
		t.Fatalf("expected one sync, got %d", dev.syncs)
	}
}

func TestRunInstallReportsMissingLibraryFr(t *testing.T) {
	projectDir := t.TempDir()
	writeFrothyToml(t, projectDir, "esp32_devkit_v1")

	dev := &fakeDevice{}
	var stderr bytes.Buffer

	code := runInstallCommand(
		[]string{"--port", "/dev/cu.usbserial-0001", "--project", projectDir},
		io.Discard, &stderr, singlePortLister("/dev/cu.usbserial-0001"), fakeInstallFactory(dev),
		115200, time.Second, 0,
	)
	if code != 1 {
		t.Fatalf("expected exit 1, got %d (stderr=%q)", code, stderr.String())
	}
	if !strings.Contains(stderr.String(), "error: library.fr not found at ") {
		t.Fatalf("expected missing-library message, got %q", stderr.String())
	}
	if !strings.Contains(stderr.String(), "run frothy build first") {
		t.Fatalf("expected hint to run frothy build, got %q", stderr.String())
	}
	if len(dev.sent) != 0 {
		t.Fatalf("expected no device traffic, got %v", dev.sent)
	}
}

func TestRunInstallSurfacesDeviceErrorMidPipe(t *testing.T) {
	projectDir := t.TempDir()
	writeFrothyToml(t, projectDir, "esp32_devkit_v1")
	writeLibraryFr(t, projectDir, "esp32_devkit_v1",
		"lib_one is fn [ 1 ]\nlib_two is fn [ 2 ]\n")

	dev := &fakeDevice{responses: []string{"ok\n", "error: unsupported (9)\n", "ok\n"}}
	var stderr bytes.Buffer

	code := runInstallCommand(
		[]string{"--port", "/dev/cu.usbserial-0001", "--project", projectDir},
		io.Discard, &stderr, singlePortLister("/dev/cu.usbserial-0001"), fakeInstallFactory(dev),
		115200, time.Second, 0,
	)
	if code != 1 {
		t.Fatalf("expected exit 1, got %d (stderr=%q)", code, stderr.String())
	}
	if !strings.Contains(stderr.String(), "error: device returned error: unsupported (9)") {
		t.Fatalf("expected surfaced device error, got %q", stderr.String())
	}
	wantSent := []string{"install-library", "lib_one is fn [ 1 ]"}
	if len(dev.sent) != len(wantSent) {
		t.Fatalf("sent %v, want %v (no further writes after device error)", dev.sent, wantSent)
	}
	for i, line := range wantSent {
		if dev.sent[i] != line {
			t.Fatalf("sent[%d]=%q, want %q (full=%v)", i, dev.sent[i], line, dev.sent)
		}
	}
}

func TestRunInstallReportsOpenFailure(t *testing.T) {
	projectDir := t.TempDir()
	writeFrothyToml(t, projectDir, "esp32_devkit_v1")
	writeLibraryFr(t, projectDir, "esp32_devkit_v1", "lib_word is fn [ 42 ]\n")

	openErr := errors.New("permission denied")
	factory := func(port string, baud int) (sessionDevice, func(), error) {
		return nil, nil, openErr
	}
	var stderr bytes.Buffer

	code := runInstallCommand(
		[]string{"--port", "/dev/cu.usbserial-0001", "--project", projectDir},
		io.Discard, &stderr, singlePortLister("/dev/cu.usbserial-0001"), factory,
		115200, time.Second, 0,
	)
	if code != 1 {
		t.Fatalf("expected exit 1, got %d", code)
	}
	if !strings.Contains(stderr.String(), "error: cannot open /dev/cu.usbserial-0001: permission denied") {
		t.Fatalf("expected open-failure error, got %q", stderr.String())
	}
}

func TestRunInstallRejectsPositionalArgs(t *testing.T) {
	dev := &fakeDevice{}
	var stderr bytes.Buffer

	code := runInstallCommand(
		[]string{"library", "--port", "/dev/cu.usbserial-0001"},
		io.Discard, &stderr, singlePortLister("/dev/cu.usbserial-0001"), fakeInstallFactory(dev),
		115200, time.Second, 0,
	)
	if code != 2 {
		t.Fatalf("expected exit 2 for positional args, got %d", code)
	}
	if len(dev.sent) != 0 {
		t.Fatalf("expected no device traffic, got %v", dev.sent)
	}
}
