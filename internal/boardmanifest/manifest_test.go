package boardmanifest

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type manifest struct {
	Name        string         `json:"name"`
	Chip        string         `json:"chip"`
	Target      string         `json:"target"`
	Profile     string         `json:"profile"`
	Peripherals []string       `json:"peripherals"`
	Pins        map[string]int `json:"pins"`
}

func loadManifest(t *testing.T, path string) manifest {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	dec := json.NewDecoder(strings.NewReader(string(raw)))
	dec.DisallowUnknownFields()
	var m manifest
	if err := dec.Decode(&m); err != nil {
		t.Fatalf("parse %s: %v", path, err)
	}
	return m
}

func checkRequired(t *testing.T, label string, m manifest) {
	t.Helper()
	if m.Name == "" {
		t.Errorf("%s: name is empty", label)
	}
	if m.Chip == "" {
		t.Errorf("%s: chip is empty", label)
	}
	if m.Target == "" {
		t.Errorf("%s: target is empty", label)
	}
	if m.Profile == "" {
		t.Errorf("%s: profile is empty", label)
	}
	if m.Peripherals == nil {
		t.Errorf("%s: peripherals missing (use [] if none)", label)
	}
	for _, p := range m.Peripherals {
		if p == "pad" {
			t.Errorf("%s: pad must not appear under peripherals (it is a runtime feature, not hardware)", label)
		}
	}
	if m.Pins == nil {
		t.Errorf("%s: pins missing (use {} if none)", label)
	}
}

func repoRoot(t *testing.T) string {
	t.Helper()
	wd, err := os.Getwd()
	if err != nil {
		t.Fatalf("getwd: %v", err)
	}
	return filepath.Join(wd, "..", "..")
}

func TestHostBoardManifest(t *testing.T) {
	m := loadManifest(t, filepath.Join(repoRoot(t), "boards", "host", "board.json"))
	checkRequired(t, "host", m)
	if _, ok := m.Pins["$led_builtin"]; !ok {
		t.Errorf("host: pins missing $led_builtin")
	}
}

func TestEsp32DevkitV1BoardManifest(t *testing.T) {
	m := loadManifest(t, filepath.Join(repoRoot(t), "boards", "esp32_devkit_v1", "board.json"))
	checkRequired(t, "esp32_devkit_v1", m)
	for _, key := range []string{"$led_builtin", "$boot_button", "$a0", "uart_tx", "uart_rx", "uart_baud"} {
		if _, ok := m.Pins[key]; !ok {
			t.Errorf("esp32_devkit_v1: pins missing %s", key)
		}
	}
	want := map[string]bool{"gpio": true, "adc": true, "uart": true}
	got := map[string]bool{}
	for _, p := range m.Peripherals {
		got[p] = true
	}
	for k := range want {
		if !got[k] {
			t.Errorf("esp32_devkit_v1: peripherals missing %q", k)
		}
	}
}
