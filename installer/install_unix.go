//go:build !windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// installToPath 在 Unix 上把工具链 bin 目录追加到 ~/.profile 的 PATH。
// 与 Windows 版(registry)保持同名接口,由 build tag 按平台选择。
func installToPath(binDir string) error {
	home, err := os.UserHomeDir()
	if err != nil {
		return fmt.Errorf("cannot determine home directory: %w", err)
	}
	profilePath := filepath.Join(home, ".profile")

	// 已存在则跳过,避免重复追加
	existing, err := os.ReadFile(profilePath)
	if err == nil && strings.Contains(string(existing), binDir) {
		return nil
	}

	line := fmt.Sprintf("\n# added by ravendrelup\nexport PATH=\"%s:$PATH\"\n", binDir)
	f, err := os.OpenFile(profilePath, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		return fmt.Errorf("cannot write to %s: %w", profilePath, err)
	}
	defer f.Close()
	if _, err := f.WriteString(line); err != nil {
		return fmt.Errorf("failed to append PATH entry: %w", err)
	}
	return nil
}
