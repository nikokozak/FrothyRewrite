package main

import (
	"bufio"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
)

const historyMax = 1000

type connectHistory struct {
	enabled bool
	path    string
	initial []string
}

func defaultHistoryPath(env func(string) string) string {
	if xdg := env("XDG_DATA_HOME"); xdg != "" {
		return filepath.Join(xdg, "frothy", "history")
	}
	if home := env("HOME"); home != "" {
		return filepath.Join(home, ".local", "share", "frothy", "history")
	}
	return ""
}

func resolveHistoryConfig(noHistory bool, override string, env func(string) string) connectHistory {
	if noHistory {
		return connectHistory{}
	}
	path := override
	if path == "" {
		path = defaultHistoryPath(env)
	}
	if path == "" {
		return connectHistory{}
	}
	initial, _ := loadHistory(path)
	return connectHistory{enabled: true, path: path, initial: initial}
}

func appendHistory(entries []string, line string) []string {
	if line == "" {
		return entries
	}
	if n := len(entries); n > 0 && entries[n-1] == line {
		return entries
	}
	entries = append(entries, line)
	if over := len(entries) - historyMax; over > 0 {
		entries = entries[over:]
	}
	return entries
}

func loadHistory(path string) ([]string, error) {
	f, err := os.Open(path)
	if err != nil {
		if errors.Is(err, fs.ErrNotExist) {
			return nil, nil
		}
		return nil, err
	}
	defer f.Close()
	var out []string
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 64*1024), 1<<20)
	for sc.Scan() {
		out = appendHistory(out, sc.Text())
	}
	if err := sc.Err(); err != nil {
		return out, err
	}
	return out, nil
}

// saveHistory writes entries to path via a tempfile + rename so a crash
// mid-write cannot leave a partial file in place.
func saveHistory(path string, entries []string) error {
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(dir, ".history-*.tmp")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	w := bufio.NewWriter(tmp)
	for _, e := range entries {
		if _, err := fmt.Fprintln(w, e); err != nil {
			tmp.Close()
			os.Remove(tmpPath)
			return err
		}
	}
	if err := w.Flush(); err != nil {
		tmp.Close()
		os.Remove(tmpPath)
		return err
	}
	if err := tmp.Close(); err != nil {
		os.Remove(tmpPath)
		return err
	}
	return os.Rename(tmpPath, path)
}
