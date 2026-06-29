package main

import (
	"bufio"
	"fmt"
	"os"
	"strings"
)

var stdin = bufio.NewReader(os.Stdin)

// interactiveInstall is the double-click / no-args experience: auto-detect DOOM, confirm, install, then pause
// so the console window (spawned by a double-click) stays up long enough to read the result.
func interactiveInstall() {
	fmt.Println("SnapHak installer", version)
	fmt.Println()

	doom, err := resolveDoom("")
	if err != nil {
		fmt.Println("Couldn't find your DOOM 2016 install automatically.")
		fmt.Print("Enter the path to your DOOM folder (the one with DOOMx64vk.exe), or leave blank to cancel: ")
		doom = readLine()
		if doom == "" || !hasDoomExe(doom) {
			fmt.Println("No valid DOOM folder provided -- cancelled.")
			pause()
			return
		}
	} else {
		fmt.Printf("Found DOOM: %s\n", doom)
	}

	if err := cmdInstall(flags{doom: doom}); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
	}
	pause()
}

func readLine() string {
	s, _ := stdin.ReadString('\n')
	return strings.TrimSpace(s)
}

func isYes(s string) bool {
	s = strings.ToLower(strings.TrimSpace(s))
	return s == "" || s == "y" || s == "yes"
}

func pause() {
	fmt.Print("\nPress Enter to exit...")
	readLine()
}

// isInteractive reports whether stdin is a real terminal -- so we only prompt when a human can answer.
// Scripts / CI / piped input skip the confirmation automatically; force-skip with --yes.
func isInteractive() bool {
	fi, err := os.Stdin.Stat()
	return err == nil && (fi.Mode()&os.ModeCharDevice) != 0
}

// confirm prints a yes/no prompt and returns true on yes (Enter / "y" / "yes").
func confirm(prompt string) bool {
	fmt.Print(prompt + " [Y/n] ")
	return isYes(readLine())
}
