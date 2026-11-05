package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
)

// cleanVersion 去除 active_version.txt 中可能存在的 BOM 与 NUL 字节。
func cleanVersion(raw string) string {
	s := strings.TrimPrefix(raw, "\xff\xfe")
	s = strings.TrimPrefix(s, "\xfe\xff")
	s = strings.TrimPrefix(s, "\xef\xbb\xbf")
	s = strings.ReplaceAll(s, "\x00", "")
	return strings.TrimSpace(s)
}

func main() {
	homeDir, err := os.UserHomeDir()
	if err != nil {
		fmt.Fprintln(os.Stderr, "Ravendrel Error: cannot determine home directory:", err)
		os.Exit(1)
	}
	ravendrelDir := filepath.Join(homeDir, ".ravendrel")
	activeVersionPath := filepath.Join(ravendrelDir, "active_version.txt")

	// install 子命令:把当前激活工具链的 bin 目录加入 PATH
	if len(os.Args) >= 2 && os.Args[1] == "install" {
		versionBytes, err := os.ReadFile(activeVersionPath)
		if err != nil {
			fmt.Fprintln(os.Stderr, "Ravendrel is installed, but no toolchain is active.")
			fmt.Fprintln(os.Stderr, "Run 'ravendrelup default stable' to configure your environment.")
			os.Exit(1)
		}
		activeVersion := cleanVersion(string(versionBytes))
		binDir := filepath.Join(ravendrelDir, "toolchains", activeVersion, "bin")
		if err := installToPath(binDir); err != nil {
			fmt.Fprintf(os.Stderr, "Ravendrel Error: failed to add toolchain to PATH: %v\n", err)
			os.Exit(1)
		}
		fmt.Printf("Ravendrel toolchain '%s' added to PATH (%s).\n", activeVersion, binDir)
		os.Exit(0)
	}

	versionBytes, err := os.ReadFile(activeVersionPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, "Ravendrel is installed, but no toolchain is active.")
		fmt.Fprintln(os.Stderr, "Run 'ravendrelup default stable' to configure your environment.")
		os.Exit(1)
	}

	activeVersion := cleanVersion(string(versionBytes))
	binDir := filepath.Join(ravendrelDir, "toolchains", activeVersion, "bin")
	// 工具链编译器文件名随目标平台：Windows 为 runtime.exe，其它平台为 runtime。
	compilerName := "runtime"
	if runtime.GOOS == "windows" {
		compilerName = "runtime.exe"
	}
	realCompiler := filepath.Join(binDir, compilerName)

	if _, err := os.Stat(realCompiler); os.IsNotExist(err) {
		fmt.Fprintf(os.Stderr, "Ravendrel Error: Active toolchain '%s' is missing or corrupted.\n", activeVersion)
		fmt.Fprintln(os.Stderr, "Run 'ravendrelup update' to restore it.")
		os.Exit(1)
	}

	cmd := exec.Command(realCompiler, os.Args[1:]...)

	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr

	err = cmd.Run()
	if err != nil {
		if exitError, ok := err.(*exec.ExitError); ok {
			os.Exit(exitError.ExitCode())
		}
		fmt.Fprintf(os.Stderr, "Ravendrel Execution Error: %v\n", err)
		os.Exit(1)
	}
}
