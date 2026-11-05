//go:build windows

package main

import (
	"fmt"
	"strings"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows/registry"
)

func installToPath(newPath string) error {
	k, err := registry.OpenKey(registry.CURRENT_USER, `Environment`, registry.QUERY_VALUE|registry.SET_VALUE)
	if err != nil {
		return fmt.Errorf("failed to open registry key: %w", err)
	}
	defer k.Close()

	currentPath, _, err := k.GetStringValue("Path")
	if err != nil {
		currentPath = ""
	}

	if strings.Contains(currentPath, newPath) {
		return nil
	}

	updatedPath := currentPath
	if len(updatedPath) > 0 && !strings.HasSuffix(updatedPath, ";") {
		updatedPath += ";"
	}
	updatedPath += newPath

	err = k.SetStringValue("Path", updatedPath)
	if err != nil {
		return fmt.Errorf("failed to write new path to registry: %w", err)
	}

	user32 := syscall.NewLazyDLL("user32.dll")
	sendMessageTimeoutW := user32.NewProc("SendMessageTimeoutW")

	const (
		HWND_BROADCAST   = 0xFFFF
		WM_SETTINGCHANGE = 0x001A
		SMTO_ABORTIFHUNG = 0x0002
	)

	envStr, err := syscall.UTF16PtrFromString("Environment")
	if err != nil {
		return fmt.Errorf("failed to convert Environment string: %w", err)
	}

	var result uintptr

	ret, _, sysErr := sendMessageTimeoutW.Call(
		uintptr(HWND_BROADCAST),
		uintptr(WM_SETTINGCHANGE),
		0,
		uintptr(unsafe.Pointer(envStr)),
		uintptr(SMTO_ABORTIFHUNG),
		uintptr(5000),
		uintptr(unsafe.Pointer(&result)),
	)

	if ret == 0 {
		return fmt.Errorf("SendMessageTimeoutW failed: %v", sysErr)
	}

	return nil
}
