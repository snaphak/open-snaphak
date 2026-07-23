package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// appDataDir is %LOCALAPPDATA%\snapmap-plus -- the one consolidated app-data folder: the install record, the
// token, a stable copy of the installer exe, runtime-owned config.json preferences, AND the user's modding
// content (overrides / prefabs / strings). The installer never owns or rewrites config.json.
// Returns "" if LOCALAPPDATA is not set. (Pre-rename installs used %LOCALAPPDATA%\open-snaphak; loadRecord /
// resolveToken / migrateUserData fold that older location forward.)
func appDataDir() string {
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		return ""
	}
	return filepath.Join(base, "snapmap-plus")
}

// sameFile reports whether two paths are the same on-disk file (so we never try to overwrite/delete the exe
// we're currently running from).
func sameFile(a, b string) bool {
	sa, ea := os.Stat(a)
	sb, eb := os.Stat(b)
	return ea == nil && eb == nil && os.SameFile(sa, sb)
}

// selfInstall copies the running installer exe into appDataDir, so a hand-delivered exe lives in a stable place
// (survives a Downloads cleanup; can be added to PATH). No-op if it's already running from there. Best-effort:
// any failure is silent -- the tool still works from wherever it was launched.
func selfInstall() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	exe, err := os.Executable()
	if err != nil {
		return
	}
	// Keep the copy under the running exe's own name (so this survives the snaphak.exe -> snapmap-plus.exe
	// rename without hard-coding either).
	target := filepath.Join(dir, filepath.Base(exe))
	if sameFile(exe, target) {
		return // already running from the installed location
	}
	_, existed := os.Stat(target)
	if os.MkdirAll(dir, 0o755) != nil {
		return
	}
	if copyFile(exe, target) != nil {
		return
	}
	if existed != nil { // first time -> let the user know where it went
		fmt.Printf("(snapmap-plus is now installed at %s -- run it from there in future, or add that folder to your PATH)\n", target)
	}
}

// cleanupAppData removes the install record, the saved token, and the stable installer-exe copy, then the folder
// itself if empty. Called on uninstall. Runtime-owned config.json and the user's modding content (overrides /
// prefabs / strings) live in this same folder and are NEVER removed -- so if any of them is present, the folder
// is left in place with it intact.
// Can't delete the exe if you're running THAT copy -- left in place then.
func cleanupAppData() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	os.Remove(filepath.Join(dir, "install.json"))
	os.Remove(filepath.Join(dir, "token"))
	// Any *.exe here is a stable copy we placed (its name follows the installer's own -- snaphak.exe or
	// snapmap-plus.exe). Remove them, except the one we're currently running from.
	exe, _ := os.Executable()
	if matches, err := filepath.Glob(filepath.Join(dir, "*.exe")); err == nil {
		for _, m := range matches {
			if exe == "" || !sameFile(exe, m) {
				os.Remove(m)
			}
		}
	}
	// Drop the content subfolders only if they're empty (a user who never authored anything). A subfolder
	// with real content stays -> the parent stays -> their work is preserved.
	for _, sub := range userContentSubdirs {
		removeIfEmpty(filepath.Join(dir, sub))
	}
	removeIfEmpty(dir)
}
