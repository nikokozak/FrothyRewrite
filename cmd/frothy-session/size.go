package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// size.go composes .frothy/build/<board>/size.json after an ESP-IDF make from
// artifacts the build already wrote: frothy.bin (image bytes), the partition
// CSV named by the build's sdkconfig (app partition capacity), and
// frothy.size.json (esp_idf_size's map summary, emitted by target.mk). The
// report is measured truth, not manifest arithmetic: flash deltas are not
// additive under LTO, so nothing here estimates — it reads what was built.
// Reporting never fails a build that produced firmware; targets without the
// inputs (host) simply skip it.

type sizeReport struct {
	AppImageBytes     int64 `json:"app_image_bytes"`
	AppPartitionBytes int64 `json:"app_partition_bytes"`
	IramUsed          int64 `json:"iram_used"`
	IramTotal         int64 `json:"iram_total"`
	DramUsed          int64 `json:"dram_used"`
	DramTotal         int64 `json:"dram_total"`
}

// esp_idf_size --format json fields this report consumes (legacy shape).
type espIdfSizeSummary struct {
	UsedIram  int64 `json:"used_iram"`
	IramTotal int64 `json:"iram_total"`
	UsedDram  int64 `json:"used_dram"`
	DramTotal int64 `json:"dram_total"`
}

// partitionCSVName extracts CONFIG_PARTITION_TABLE_CUSTOM_FILENAME from a
// built sdkconfig, so the report reads the same table the build flashed
// (boards override it per chip, e.g. partitions-esp32c6.csv).
func partitionCSVName(sdkconfig []byte) (string, error) {
	const key = `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="`
	for _, line := range strings.Split(string(sdkconfig), "\n") {
		if rest, ok := strings.CutPrefix(line, key); ok {
			if name, ok := strings.CutSuffix(rest, `"`); ok && name != "" {
				return name, nil
			}
		}
	}
	return "", fmt.Errorf("sdkconfig names no custom partition table")
}

// smallestAppPartitionBytes parses an ESP-IDF partition CSV and returns the
// smallest app-type partition — the bound check_sizes enforces, so the report
// and the build agree on capacity.
func smallestAppPartitionBytes(csv []byte) (int64, error) {
	var smallest int64
	for _, line := range strings.Split(string(csv), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := strings.Split(line, ",")
		if len(fields) < 5 || strings.TrimSpace(fields[1]) != "app" {
			continue
		}
		size, err := parsePartitionSize(strings.TrimSpace(fields[4]))
		if err != nil {
			return 0, fmt.Errorf("partition %q: %w", strings.TrimSpace(fields[0]), err)
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

// parsePartitionSize accepts the CSV size forms ESP-IDF does: hex (0x177000),
// decimal, and K/M suffixes.
func parsePartitionSize(text string) (int64, error) {
	multiplier := int64(1)
	upper := strings.ToUpper(text)
	if suffix, ok := strings.CutSuffix(upper, "K"); ok {
		text, multiplier = suffix, 1024
	} else if suffix, ok := strings.CutSuffix(upper, "M"); ok {
		text, multiplier = suffix, 1024*1024
	}
	value, err := strconv.ParseInt(strings.TrimSpace(text), 0, 64)
	if err != nil || value <= 0 {
		return 0, fmt.Errorf("invalid partition size %q", text)
	}
	return value * multiplier, nil
}

func buildSizeReport(sourceRoot, board string) (sizeReport, error) {
	buildDir := filepath.Join(sourceRoot, "build", board)

	image, err := os.Stat(filepath.Join(buildDir, "frothy.bin"))
	if err != nil {
		return sizeReport{}, err
	}
	sdkconfig, err := os.ReadFile(filepath.Join(buildDir, "sdkconfig"))
	if err != nil {
		return sizeReport{}, err
	}
	csvName, err := partitionCSVName(sdkconfig)
	if err != nil {
		return sizeReport{}, err
	}
	csv, err := os.ReadFile(filepath.Join(sourceRoot, "targets", "esp-idf", csvName))
	if err != nil {
		return sizeReport{}, err
	}
	partition, err := smallestAppPartitionBytes(csv)
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
	}, nil
}

// emitSizeReport writes size.json next to the other generated outputs and
// prints a one-line summary. A build whose target produced no size inputs
// (host) is not an error; any other failure is reported as a warning because
// the firmware itself already built.
func emitSizeReport(sourceRoot, projectDir, board string, stdout, stderr io.Writer) {
	report, err := buildSizeReport(sourceRoot, board)
	if err != nil {
		if os.IsNotExist(err) {
			return
		}
		fmt.Fprintf(stderr, "frothy build: size report skipped: %v\n", err)
		return
	}
	encoded, err := json.MarshalIndent(report, "", "  ")
	if err != nil {
		fmt.Fprintf(stderr, "frothy build: size report skipped: %v\n", err)
		return
	}
	outPath := filepath.Join(buildOutputDir(projectDir, board), "size.json")
	if err := os.WriteFile(outPath, append(encoded, '\n'), 0o644); err != nil {
		fmt.Fprintf(stderr, "frothy build: size report skipped: %v\n", err)
		return
	}
	free := report.AppPartitionBytes - report.AppImageBytes
	fmt.Fprintf(stdout, "size: app %d of %d bytes (%d free), IRAM %d/%d, DRAM %d/%d\n",
		report.AppImageBytes, report.AppPartitionBytes, free,
		report.IramUsed, report.IramTotal, report.DramUsed, report.DramTotal)
}
