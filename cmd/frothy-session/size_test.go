package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// partitionEntry builds one 32-byte partition-table.bin record. The magic is
// spelled as the literal on-disk bytes AA 50 (not via the parser's own
// constant) so a byte-order mistake in the parser cannot hide in the fixture
// — round 1 shipped exactly that bug, verified against a real table.
func partitionEntry(entryType byte, size uint32) []byte {
	entry := make([]byte, partitionEntryBytes)
	entry[0], entry[1] = 0xAA, 0x50
	entry[2] = entryType
	binary.LittleEndian.PutUint32(entry[8:12], size)
	return entry
}

func TestSmallestAppPartitionBytesFromBin(t *testing.T) {
	table := append(partitionEntry(0x01, 0x6000), partitionEntry(partitionTypeApp, 0x200000)...)
	size, err := smallestAppPartitionBytes(table)
	if err != nil || size != 0x200000 {
		t.Fatalf("want 0x200000, got %#x, %v", size, err)
	}
}

func TestSmallestAppPartitionBytesPicksSmallest(t *testing.T) {
	table := append(partitionEntry(partitionTypeApp, 0x200000), partitionEntry(partitionTypeApp, 0x180000)...)
	size, err := smallestAppPartitionBytes(table)
	if err != nil || size != 0x180000 {
		t.Fatalf("want 0x180000, got %#x, %v", size, err)
	}
}

func TestSmallestAppPartitionBytesStopsAtNonMagic(t *testing.T) {
	// The MD5 sentinel row (0xEBEB) ends the table; an app entry after it is
	// not part of the flashed table and must be ignored.
	md5Row := make([]byte, partitionEntryBytes)
	md5Row[0], md5Row[1] = 0xEB, 0xEB
	table := append(partitionEntry(partitionTypeApp, 0x100000), md5Row...)
	table = append(table, partitionEntry(partitionTypeApp, 0x1000)...)
	size, err := smallestAppPartitionBytes(table)
	if err != nil || size != 0x100000 {
		t.Fatalf("want 0x100000, got %#x, %v", size, err)
	}
}

func TestSmallestAppPartitionBytesNoApp(t *testing.T) {
	if _, err := smallestAppPartitionBytes(partitionEntry(0x01, 0x6000)); err == nil {
		t.Fatal("want error for a table without an app partition")
	}
	if _, err := smallestAppPartitionBytes(nil); err == nil {
		t.Fatal("want error for an empty table")
	}
}

// buildSizeReport reads only artifacts the build already wrote; prove the
// composition, including the S3-style shared DIRAM pool, from a synthetic
// source root.
func TestBuildSizeReportFromArtifacts(t *testing.T) {
	root := t.TempDir()
	buildDir := filepath.Join(root, "build", "seeed_xiao_esp32s3")
	if err := os.MkdirAll(filepath.Join(buildDir, "partition_table"), 0o755); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(buildDir, "frothy.bin"), strings.Repeat("x", 1234))
	if err := os.WriteFile(filepath.Join(buildDir, "partition_table", "partition-table.bin"),
		partitionEntry(partitionTypeApp, 0x200000), 0o644); err != nil {
		t.Fatal(err)
	}
	writeTestFile(t, filepath.Join(buildDir, "frothy.size.json"),
		`{"used_iram": 16384, "iram_total": 16384, "used_dram": 0, "dram_total": 0,
		  "used_diram": 200763, "diram_total": 341760}`)

	report, err := buildSizeReport(root, "seeed_xiao_esp32s3")
	if err != nil {
		t.Fatal(err)
	}
	want := sizeReport{
		AppImageBytes:     1234,
		AppPartitionBytes: 0x200000,
		IramUsed:          16384,
		IramTotal:         16384,
		DramUsed:          0,
		DramTotal:         0,
		DiramUsed:         200763,
		DiramTotal:        341760,
	}
	if report != want {
		t.Fatalf("report mismatch:\n got %+v\nwant %+v", report, want)
	}
}

// A skipped report must remove last run's size.json, never leave it to be
// attached to new firmware.
func TestEmitSizeReportRemovesStaleReport(t *testing.T) {
	root := t.TempDir()
	project := t.TempDir()
	outDir := buildOutputDir(project, "esp32_devkit_v1")
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		t.Fatal(err)
	}
	stale := filepath.Join(outDir, "size.json")
	writeTestFile(t, stale, `{"app_image_bytes": 1}`)

	// No build artifacts under root: the report must skip AND remove the stale file.
	emitSizeReport(root, project, "esp32_devkit_v1", os.Stderr, os.Stderr)
	if _, err := os.Stat(stale); !os.IsNotExist(err) {
		t.Fatalf("stale size.json must be removed on skip, stat err = %v", err)
	}
}

func writeTestFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}
