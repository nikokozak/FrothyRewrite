package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// size.go composes .frothy/build/<board>/size.json after an ESP-IDF make from
// artifacts the build already wrote: frothy.bin (image bytes), the generated
// partition-table.bin (app partition capacity — the flashed artifact itself,
// not a source-CSV replica), and frothy.size.json (esp_idf_size's map
// summary, emitted best-effort by target.mk). The report is measured truth,
// not manifest arithmetic: flash deltas are not additive under LTO, so
// nothing here estimates — it reads what was built. Reporting never fails a
// build that produced firmware; targets without the inputs (host) skip it,
// and a skipped report always removes any stale size.json so old
// measurements cannot be attached to new firmware.

type sizeReport struct {
	AppImageBytes     int64 `json:"app_image_bytes"`
	AppPartitionBytes int64 `json:"app_partition_bytes"`
	IramUsed          int64 `json:"iram_used"`
	IramTotal         int64 `json:"iram_total"`
	DramUsed          int64 `json:"dram_used"`
	DramTotal         int64 `json:"dram_total"`
	DiramUsed         int64 `json:"diram_used"`
	DiramTotal        int64 `json:"diram_total"`
}

// esp_idf_size --format json fields this report consumes (legacy shape).
// Chips report distinct IRAM/DRAM (classic ESP32) or one shared DIRAM pool
// (ESP32-S3); both are carried so a zero region means "absent on this chip",
// never "unmeasured".
type espIdfSizeSummary struct {
	UsedIram   int64 `json:"used_iram"`
	IramTotal  int64 `json:"iram_total"`
	UsedDram   int64 `json:"used_dram"`
	DramTotal  int64 `json:"dram_total"`
	UsedDiram  int64 `json:"used_diram"`
	DiramTotal int64 `json:"diram_total"`
}

// ESP-IDF partition table binary format: 32-byte entries starting with the
// bytes AA 50 (ESP_PARTITION_MAGIC 0x50AA read little-endian), then type
// (0x00 = app), subtype, offset, size, label. The table ends at the first
// non-magic entry (the MD5 row reads 0xEBEB).
const (
	partitionEntryBytes = 32
	partitionMagic      = 0x50AA
	partitionTypeApp    = 0x00
)

// smallestAppPartitionBytes parses the generated partition-table.bin and
// returns the smallest app-type partition — the bound check_sizes enforces,
// so the report and the build agree on capacity.
func smallestAppPartitionBytes(table []byte) (int64, error) {
	var smallest int64
	for off := 0; off+partitionEntryBytes <= len(table); off += partitionEntryBytes {
		entry := table[off : off+partitionEntryBytes]
		if binary.LittleEndian.Uint16(entry[0:2]) != partitionMagic {
			break
		}
		if entry[2] != partitionTypeApp {
			continue
		}
		size := int64(binary.LittleEndian.Uint32(entry[8:12]))
		if size <= 0 {
			return 0, fmt.Errorf("app partition with invalid size %d", size)
		}
		if smallest == 0 || size < smallest {
			smallest = size
		}
	}
	if smallest == 0 {
		return 0, fmt.Errorf("partition table has no app partition")
	}
	return smallest, nil
}

func buildSizeReport(sourceRoot, board string) (sizeReport, error) {
	buildDir := filepath.Join(sourceRoot, "build", board)

	image, err := os.Stat(filepath.Join(buildDir, "frothy.bin"))
	if err != nil {
		return sizeReport{}, err
	}
	table, err := os.ReadFile(filepath.Join(buildDir, "partition_table", "partition-table.bin"))
	if err != nil {
		return sizeReport{}, err
	}
	partition, err := smallestAppPartitionBytes(table)
	if err != nil {
		return sizeReport{}, err
	}
	summaryBytes, err := os.ReadFile(filepath.Join(buildDir, "frothy.size.json"))
	if err != nil {
		return sizeReport{}, err
	}
	var summary espIdfSizeSummary
	if err := json.Unmarshal(summaryBytes, &summary); err != nil {
		return sizeReport{}, fmt.Errorf("frothy.size.json: %w", err)
	}

	return sizeReport{
		AppImageBytes:     image.Size(),
		AppPartitionBytes: partition,
		IramUsed:          summary.UsedIram,
		IramTotal:         summary.IramTotal,
		DramUsed:          summary.UsedDram,
		DramTotal:         summary.DramTotal,
		DiramUsed:         summary.UsedDiram,
		DiramTotal:        summary.DiramTotal,
	}, nil
}

// emitSizeReport writes size.json next to the other generated outputs and
// prints a one-line summary. Any stale report is removed first, so a build
// whose target produced no size inputs (host, or a failed esp_idf_size leg)
// leaves no report rather than last run's. The write is atomic (temp +
// rename) so a consumer never reads a torn report. Reporting failures other
// than missing inputs warn, because the firmware itself already built.
func emitSizeReport(sourceRoot, projectDir, board string, stdout, stderr io.Writer) {
	outPath := filepath.Join(buildOutputDir(projectDir, board), "size.json")
	if err := os.Remove(outPath); err != nil && !os.IsNotExist(err) {
		fmt.Fprintf(stderr, "frothy build: size report skipped: %v\n", err)
		return
	}

	report, err := buildSizeReport(sourceRoot, board)
	if err != nil {
		if !os.IsNotExist(err) {
			fmt.Fprintf(stderr, "frothy build: size report skipped: %v\n", err)
		}
		return
	}
	encoded, err := json.MarshalIndent(report, "", "  ")
	if err != nil {
		fmt.Fprintf(stderr, "frothy build: size report skipped: %v\n", err)
		return
	}
	tmpPath := outPath + ".tmp"
	if err := os.WriteFile(tmpPath, append(encoded, '\n'), 0o644); err == nil {
		err = os.Rename(tmpPath, outPath)
	} else {
		_ = os.Remove(tmpPath)
	}
	if err != nil {
		fmt.Fprintf(stderr, "frothy build: size report skipped: %v\n", err)
		return
	}
	free := report.AppPartitionBytes - report.AppImageBytes
	fmt.Fprintf(stdout, "size: app %d of %d bytes (%d free), IRAM %d/%d, DRAM %d/%d, DIRAM %d/%d\n",
		report.AppImageBytes, report.AppPartitionBytes, free,
		report.IramUsed, report.IramTotal, report.DramUsed, report.DramTotal,
		report.DiramUsed, report.DiramTotal)
}
