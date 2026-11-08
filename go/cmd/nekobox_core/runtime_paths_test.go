package main

import (
	"bytes"
	"context"
	"flag"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/matsuridayo/libneko/neko_common"
)

// Each setup uses a fresh process because the library logger is startup-only.
func TestRuntimePathsProcess(t *testing.T) {
	if os.Getenv("NK02_CHILD") != "1" {
		return
	}
	// Baseline has no flag. Still exercise its real setupCore for behavioral RED.
	if f := flag.Lookup("data-dir"); f != nil {
		if err := f.Value.Set(os.Getenv("NK02_DATA_DIR")); err != nil {
			t.Fatal(err)
		}
	}
	neko_common.RunMode = neko_common.RunMode_NekoBox_Core
	setupCore()
	log.Print("NK02-core-still-running")
}

func runtimePathsChild(t *testing.T, cwd, data string) string {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, os.Args[0], "-test.run=^TestRuntimePathsProcess$", "-test.v")
	cmd.Dir = cwd
	cmd.Env = append(os.Environ(), "NK02_CHILD=1", "NK02_DATA_DIR="+data)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("core setup failed: %v\n%s", err, out)
	}
	if !bytes.Contains(out, []byte("NK02-core-still-running")) {
		t.Fatalf("console logging lost: %s", out)
	}
	return string(out)
}

func runtimePathsDirs(t *testing.T) (cwd, data, path string) {
	t.Helper()
	// Positive tests use physical paths: macOS /var is a refused symlink.
	root, err := filepath.EvalSymlinks(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	cwd, data = filepath.Join(root, "unrelated-cwd"), filepath.Join(root, "portable config")
	for _, dir := range []string{cwd, data, filepath.Join(data, "logs")} {
		if err := os.Mkdir(dir, 0700); err != nil {
			t.Fatal(err)
		}
	}
	return cwd, data, filepath.Join(data, "logs", "neko.log")
}

func TestRuntimePaths(t *testing.T) {
	if runtime.GOOS != "darwin" && runtime.GOOS != "linux" {
		t.Skip("Unix permission regressions")
	}
	t.Run("SelectedConfigNotCWD", func(t *testing.T) {
		cwd, data, path := runtimePathsDirs(t)
		if err := os.Remove(filepath.Dir(path)); err != nil {
			t.Fatal(err)
		}
		runtimePathsChild(t, cwd, data)
		b, err := os.ReadFile(path)
		if err != nil {
			t.Fatalf("selected config log: %v", err)
		}
		if !bytes.Contains(b, []byte("NK02-core-still-running")) {
			t.Fatalf("log lacks core output: %q", b)
		}
		for name, mode := range map[string]os.FileMode{path: 0600, filepath.Dir(path): 0700} {
			st, err := os.Stat(name)
			if err != nil {
				t.Fatal(err)
			}
			if st.Mode().Perm() != mode {
				t.Errorf("%s: mode %o, want %o", name, st.Mode().Perm(), mode)
			}
		}
		if _, err := os.Lstat(filepath.Join(cwd, "neko.log")); !os.IsNotExist(err) {
			t.Fatalf("cwd log created: %v", err)
		}
	})
	t.Run("ExistingPrivatePreserved", func(t *testing.T) {
		cwd, data, path := runtimePathsDirs(t)
		if err := os.WriteFile(path, []byte("previous-private-log\n"), 0600); err != nil {
			t.Fatal(err)
		}
		runtimePathsChild(t, cwd, data)
		b, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Contains(b, []byte("previous-private-log")) || !bytes.Contains(b, []byte("NK02-core-still-running")) {
			t.Fatalf("existing log not continued: %q", b)
		}
	})
	for _, kind := range []string{"symlink", "hardlink", "broad-file", "broad-parent", "parent-symlink", "ancestor-symlink", "unwritable", "missing-data", "relative-data", "no-data", "directory-file"} {
		t.Run(kind, func(t *testing.T) {
			cwd, data, path := runtimePathsDirs(t)
			target := filepath.Join(cwd, "target")
			original := []byte(strings.Repeat("preserve-sensitive-data\n", 4000))
			if err := os.WriteFile(target, original, 0600); err != nil {
				t.Fatal(err)
			}
			switch kind {
			case "symlink", "hardlink":
				for _, name := range []string{path, filepath.Join(cwd, "neko.log")} {
					var err error
					if kind == "symlink" {
						err = os.Symlink(target, name)
					} else {
						err = os.Link(target, name)
					}
					if err != nil {
						t.Fatal(err)
					}
				}
			case "broad-file":
				if err := os.WriteFile(path, original, 0644); err != nil {
					t.Fatal(err)
				}
				if err := os.Chmod(path, 0644); err != nil {
					t.Fatal(err)
				}
			case "broad-parent":
				if err := os.Chmod(filepath.Dir(path), 0777); err != nil {
					t.Fatal(err)
				}
			case "parent-symlink":
				if err := os.Remove(filepath.Dir(path)); err != nil {
					t.Fatal(err)
				}
				if err := os.Symlink(cwd, filepath.Dir(path)); err != nil {
					t.Fatal(err)
				}
			case "ancestor-symlink":
				alias := filepath.Join(filepath.Dir(data), "alias")
				if err := os.Symlink(data, alias); err != nil {
					t.Fatal(err)
				}
				data = alias
			case "unwritable":
				if os.Geteuid() == 0 {
					t.Skip("requires ordinary user")
				}
				if err := os.Chmod(filepath.Dir(path), 0500); err != nil {
					t.Fatal(err)
				}
				defer os.Chmod(filepath.Dir(path), 0700)
			case "missing-data":
				data = filepath.Join(data, "missing")
			case "relative-data":
				data = "../portable config"
			case "no-data":
				data = ""
			case "directory-file":
				if err := os.Mkdir(path, 0700); err != nil {
					t.Fatal(err)
				}
			}
			out := runtimePathsChild(t, cwd, data)
			if !strings.Contains(out, "file logging disabled") {
				t.Errorf("missing visible fallback diagnostic: %s", out)
			}
			got, err := os.ReadFile(target)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(got, original) {
				t.Errorf("%s target changed (got %d bytes, want %d)", kind, len(got), len(original))
			}
			st, err := os.Stat(target)
			if err != nil {
				t.Fatal(err)
			}
			if st.Mode().Perm() != 0600 {
				t.Errorf("target mode changed: %o", st.Mode().Perm())
			}
			if kind == "broad-file" {
				got, err := os.ReadFile(path)
				if err != nil {
					t.Fatal(err)
				}
				st, err := os.Stat(path)
				if err != nil {
					t.Fatal(err)
				}
				if !bytes.Equal(got, original) || st.Mode().Perm() != 0644 {
					t.Error("unsafe old log was migrated or modified")
				}
			}
			if kind == "broad-parent" {
				st, err := os.Stat(filepath.Dir(path))
				if err != nil {
					t.Fatal(err)
				}
				if st.Mode().Perm() != 0777 {
					t.Error("existing parent permissions changed")
				}
			}
		})
	}
}
