package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// User modding content (overrides, prefabs, custom strings) lives in the app-data folder alongside the install
// record -- one consolidated place, %LOCALAPPDATA%\snapmap-plus\ (appDataDir). This is the right home for mod
// data: it is reliably writable (unlike a game folder under Program Files, where non-elevated writes fail or get
// redirected), it survives a game uninstall / "verify integrity of game files" / reinstall, and it is out of the
// home directory so its name never collides with a repository clone. The backend reads the same tree; its startup
// scaffolder (dllmain.c) mirrors userContentSubdirs -- keep the two in sync.

// userContentSubdirs is the content tree the installer scaffolds and the backend reads. Mirrors dllmain.c's subs[].
var userContentSubdirs = []string{"strings", "overrides", "prefabs"}

// oldUserContentDir is where content lived before this version: %USERPROFILE%\snaphak\ (a path reused from the
// original tool, historically in the home root). Empty if USERPROFILE is unset.
func oldUserContentDir() string {
	base := os.Getenv("USERPROFILE")
	if base == "" {
		return ""
	}
	return filepath.Join(base, "snaphak")
}

// oldAppDataDir is the pre-rename app-data folder, %LOCALAPPDATA%\open-snaphak (install record + token).
func oldAppDataDir() string {
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		return ""
	}
	return filepath.Join(base, "open-snaphak")
}

func oldRecordPath() string {
	if d := oldAppDataDir(); d != "" {
		return filepath.Join(d, "install.json")
	}
	return ""
}

func oldTokenPath() string {
	if d := oldAppDataDir(); d != "" {
		return filepath.Join(d, "token")
	}
	return ""
}

// ensureUserDataTree creates the content folders (overrides / prefabs / strings) under the app-data dir if a
// fresh profile lacks them, so the disk-backed features work on a clean install instead of silently no-opping.
func ensureUserDataTree() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	for _, sub := range userContentSubdirs {
		os.MkdirAll(filepath.Join(dir, sub), 0o755)
	}
}

// migrateUserData scaffolds the content tree and, one time, MOVES a user's existing content forward from the
// old home-root %USERPROFILE%\snaphak\ folder into the app-data dir: it copies every missing file, then removes
// the old folder once every one of its files is confirmed present at the new location. That's a VERIFIED move --
// the delete only happens after the content is safely mirrored, so nothing is ever lost, but no stale "backup"
// is left behind either. It also clears the pre-rename %LOCALAPPDATA%\open-snaphak\ app-data folder once its
// record/token have migrated forward. Best-effort; never fails the install; never deletes an unmirrored folder.
func migrateUserData() {
	dir := appDataDir()
	if dir == "" {
		return
	}
	ensureUserDataTree()

	// 1) User content: %USERPROFILE%\snaphak -> the app-data dir, then remove the old folder if fully mirrored.
	if old := oldUserContentDir(); old != "" && !sameFile(old, dir) {
		if _, err := os.Stat(old); err == nil {
			copyTreeMissing(old, dir)
			if fullyMirrored(old, dir) {
				if os.RemoveAll(old) == nil {
					fmt.Printf("  ~ moved your saved content (overrides / prefabs / rawmaps) to %s\n", dir)
				}
			} else {
				fmt.Printf("  ~ copied your content to %s -- kept %s (some files could not be verified)\n", dir, old)
			}
		}
	}

	// 2) Old app-data folder (%LOCALAPPDATA%\open-snaphak): the record + token migrate forward lazily via
	//    loadRecord / resolveToken; make sure both are present at the new location, then delete the old folder
	//    (its stale exe copy goes with it).
	if oldAD := oldAppDataDir(); oldAD != "" && !sameFile(oldAD, dir) {
		if _, err := os.Stat(oldAD); err == nil {
			for _, name := range []string{"install.json", "token"} {
				s, t := filepath.Join(oldAD, name), filepath.Join(dir, name)
				if _, e := os.Stat(t); e != nil {
					if _, e := os.Stat(s); e == nil {
						copyFile(s, t)
					}
				}
			}
			os.RemoveAll(oldAD)
		}
	}
}

// fullyMirrored reports whether every file under src also exists under dst -- the safety check that lets a
// migration delete src without risking data loss.
func fullyMirrored(src, dst string) bool {
	ok := true
	filepath.WalkDir(src, func(path string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		rel, rerr := filepath.Rel(src, path)
		if rerr != nil {
			ok = false
			return nil
		}
		if _, e := os.Stat(filepath.Join(dst, rel)); e != nil {
			ok = false
		}
		return nil
	})
	return ok
}

// copyTreeMissing recursively copies every file under src into dst, preserving the relative layout, but only
// when the destination file does NOT already exist (it never overwrites). Returns the number of files copied.
// Best-effort: an unreadable/unwritable file (or a missing src) is skipped, not fatal.
func copyTreeMissing(src, dst string) int {
	copied := 0
	filepath.WalkDir(src, func(path string, d os.DirEntry, err error) error {
		if err != nil { // unreadable entry, or src doesn't exist -> nothing to migrate
			return nil
		}
		if d.IsDir() {
			return nil
		}
		rel, rerr := filepath.Rel(src, path)
		if rerr != nil {
			return nil
		}
		target := filepath.Join(dst, rel)
		if _, err := os.Stat(target); err == nil {
			return nil // already present -> never overwrite
		}
		if os.MkdirAll(filepath.Dir(target), 0o755) != nil {
			return nil
		}
		if copyFile(path, target) == nil {
			copied++
		}
		return nil
	})
	return copied
}
