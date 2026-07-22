package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPartitionCSVNameFromSdkconfig(t *testing.T) {
	sdkconfig := []byte("CONFIG_FOO=y\nCONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions-esp32c6.csv\"\n")
	name, err := partitionCSVName(sdkconfig)
	if err != nil || name != "partitions-esp32c6.csv" {
		t.Fatalf("want partitions-esp32c6.csv, got %q, %v", name, err)
	}
	if _, err := partitionCSVName([]byte("CONFIG_FOO=y\n")); err == nil {
		t.Fatal("want error when no partition table is named")
	}
}

func TestSmallestAppPartitionBytes(t *testing.T) {
	csv := []byte(`# Name, Type, SubType, Offset, Size, Flags
nvs,     data, nvs,     0x9000,  0x6000,
factory, app,  factory, 0x10000, 0x177000,
frothy,  data, 0x40,    0x187000, 0x40000,
`)
	size, err := smallestAppPartitionBytes(csv)
	if err != nil || size != 0x177000 {
		t.Fatalf("want 0x177000, got %#x, %v", size, err)
	}
}

func TestSmallestAppPartitionBytesPicksSmallest(t *testing.T) {
	csv := []byte("ota_0, app, ota_0, 0x10000, 2M,\nota_1, app, ota_1, 0x210000, 1500K,\n")
	size, err := smallestAppPartitionBytes(csv)
	if err != nil || size != 1500*1024 {
		t.Fatalf("want 1500K, got %d, %v", size, err)
	}
}

func TestSmallestAppPartitionBytesNoApp(t *testing.T) {
	if _, err := smallestAppPartitionBytes([]byte("nvs, data, nvs, 0x9000, 0x6000,\n")); err == nil {
		t.Fatal("want error for a table without an app partition")
	}
}

func TestParsePartitionSizeForms(t *testing.T) {
	cases := map[string]int64{"0x177000": 0x177000, "4096": 4096, "64K": 64 * 1024, "2M": 2 * 1024 * 1024}
	for text, want := range cases {
		got, err := parsePartitionSize(text)
		if err != nil || got != want {
			t.Fatalf("%q: want %d, got %d, %v", text, want, got, err)
		}
	}
	for _, bad := range []string{"", "-1", "0", "lots"} {
		if _, err := parsePartitionSize(bad); err == nil {
			t.Fatalf("%q: want error", bad)
		}
	}
}

// buildSizeReport reads only artifacts the build already wrote; prove the
// composition from a synthetic source root.
func TestBuildSizeReportFromArtifacts(t *testing.T) {
	root := t.TempDir()
	buildDir := filepath.Join(root, "build", "esp32_devkit_v1")
	if err := os.MkdirAll(buildDir, 0o755); err != nil {
		t.Fatal(err)
	}
	espDir := filepath.Join(root, "targets", "esp-idf")
	if err := os.MkdirAll(espDir, 0o755); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(buildDir, "frothy.bin"), strings.Repeat("x", 1234))
	writeTestFile(t, filepath.Join(buildDir, "sdkconfig"),
		"CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions.csv\"\n")
	writeTestFile(t, filepath.Join(espDir, "partitions.csv"),
		"factory, app, factory, 0x10000, 0x177000,\n")
	writeTestFile(t, filepath.Join(buildDir, "frothy.size.json"),
		`{"used_iram": 60919, "iram_total": 131072, "used_dram": 59992, "dram_total": 180736}`)

	report, err := buildSizeReport(root, "esp32_devkit_v1")
	if err != nil {
		t.Fatal(err)
	}
	want := sizeReport{
		AppImageBytes:     1234,
		AppPartitionBytes: 0x177000,
		IramUsed:          60919,
		IramTotal:         131072,
		DramUsed:          59992,
		DramTotal:         180736,
	}
	if report != want {
		t.Fatalf("report mismatch:\n got %+v\nwant %+v", report, want)
	}
	if _, err := json.Marshal(report); err != nil {
		t.Fatal(err)
	}
}

func writeTestFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}
