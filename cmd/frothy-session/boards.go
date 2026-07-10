package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
)

type boardManifest struct {
	Target string `json:"target"`
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
