package main

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestPrepareLogFileNew(t *testing.T) {
	dir := t.TempDir()
	originalDir, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Chdir(dir); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := os.Chdir(originalDir); err != nil {
			t.Fatal(err)
		}
	})
	path := "./neko.log"
	if err := prepareLogFile(path); err != nil {
		t.Fatal(err)
	}
	info, err := os.Stat(filepath.Join(dir, "neko.log"))
	if err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS == "windows" {
		t.Log("skipping Unix permission assertion on Windows")
	} else if mode := info.Mode().Perm(); mode != 0600 {
		t.Fatalf("mode = %04o, want 0600", mode)
	}
}

func TestPrepareLogFileExisting(t *testing.T) {
	path := filepath.Join(t.TempDir(), "neko.log")
	const content = "existing log content\n"
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
	// Ensure the initial mode is 0644 even under a restrictive umask.
	if err := os.Chmod(path, 0644); err != nil {
		t.Fatal(err)
	}
	if err := prepareLogFile(path); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != content {
		t.Fatalf("content = %q, want %q", got, content)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if runtime.GOOS == "windows" {
		t.Log("skipping Unix permission assertion on Windows")
	} else if mode := info.Mode().Perm(); mode != 0644 {
		t.Fatalf("mode = %04o, want 0644", mode)
	}
}

func TestPrepareLogFileMissingDirectory(t *testing.T) {
	path := filepath.Join(t.TempDir(), "missing", "neko.log")
	if err := prepareLogFile(path); err == nil {
		t.Fatal("expected an error for a missing parent directory")
	}
	if _, err := os.Stat(filepath.Dir(path)); !os.IsNotExist(err) {
		t.Fatalf("parent directory stat error = %v, want not exist", err)
	}
}
