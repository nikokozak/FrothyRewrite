package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type boardManifest struct {
	Target string `json:"target"`
}

type firmwareSegment struct {
	Address uint32 `json:"address"`
	File    string `json:"file"`
}

type firmwareBundleRow struct {
	Board    string            `json:"board"`
	Chip     string            `json:"chip"`
	Segments []firmwareSegment `json:"segments"`
}

func flashableBoard(boardsDir, id string) bool {
	if id == "" || id == "." || id == ".." || strings.ContainsAny(id, "/\\") {
		return false
	}
	data, err := os.ReadFile(filepath.Join(boardsDir, id, "board.json"))
	if err != nil {
		return false
	}
	var manifest boardManifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		return false
	}
	return manifest.Target != "" && manifest.Target != "host"
}

func listFlashableBoards(boardsDir string) []string {
	entries, err := os.ReadDir(boardsDir)
	if err != nil {
		return nil
	}
	var boards []string
	for _, entry := range entries {
		if entry.IsDir() && flashableBoard(boardsDir, entry.Name()) {
			boards = append(boards, entry.Name())
		}
	}
	return boards
}

func packagedFirmwareRoot(executable string) string {
	executable = canonicalPath(executable)
	if executable == "" {
		return ""
	}
	root := filepath.Join(filepath.Dir(executable), "..", "share", "frothy", "firmware")
	if !fileExists(filepath.Join(root, "manifest.json")) {
		return ""
	}
	return canonicalPath(root)
}

func loadPackagedFirmware(root, board string) (firmwareBundleRow, error) {
	var rows []firmwareBundleRow
	data, err := os.ReadFile(filepath.Join(root, "manifest.json"))
	if err != nil {
		return firmwareBundleRow{}, fmt.Errorf("packaged firmware is unavailable: %w", err)
	}
	if err := json.Unmarshal(data, &rows); err != nil {
		return firmwareBundleRow{}, fmt.Errorf("invalid packaged firmware manifest: %w", err)
	}
	for _, row := range rows {
		if row.Board != board {
			continue
		}
		if !safeBundleName(row.Chip) || len(row.Segments) == 0 {
			return firmwareBundleRow{}, fmt.Errorf("invalid packaged firmware for board %q", board)
		}
		seen := map[uint32]bool{}
		for _, segment := range row.Segments {
			if seen[segment.Address] || filepath.Base(segment.File) != segment.File ||
				!strings.HasSuffix(segment.File, ".bin") ||
				!fileExists(filepath.Join(root, segment.File)) {
				return firmwareBundleRow{}, fmt.Errorf("invalid packaged firmware for board %q", board)
			}
			seen[segment.Address] = true
		}
		return row, nil
	}
	return firmwareBundleRow{}, fmt.Errorf("unknown board %q", board)
}

func safeBundleName(value string) bool {
	if value == "" {
		return false
	}
	for _, char := range value {
		if (char < 'a' || char > 'z') && (char < '0' || char > '9') {
			return false
		}
	}
	return true
}
