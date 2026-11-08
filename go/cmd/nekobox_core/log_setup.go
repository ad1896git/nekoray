package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"github.com/matsuridayo/libneko/neko_log"
)

// Parsed only by the nekobox entry point; sing-box run keeps its own logging.
var logDataDir = flag.String("data-dir", "", "absolute GUI config directory for the core log")

func setupRunLog(dataDir string) error {
	path, err := runLogPath(dataDir)
	if err != nil {
		// Use libneko's stdout-only fallback even when directory selection fails.
		// The GUI receives stdout before its stderr authentication handshake.
		_ = neko_log.SetupLog(-1, "")
		return fmt.Errorf("file logging disabled: %w", err)
	}
	if err := neko_log.SetupLog(50*1024, path); err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	return nil
}

func runLogPath(dataDir string) (string, error) {
	if !filepath.IsAbs(dataDir) {
		return "", fmt.Errorf("-data-dir must be an absolute config directory")
	}
	dir := filepath.Join(dataDir, "logs")
	// Create only the leaf directory. Do not create a missing config tree, chmod
	// existing paths, or pre-open the file. libneko validates all ancestors and
	// opens the final file without following links, including on Windows.
	if err := os.Mkdir(dir, 0700); err != nil && !os.IsExist(err) {
		return "", fmt.Errorf("create log directory %s: %w", dir, err)
	}
	return filepath.Join(dir, "neko.log"), nil
}
