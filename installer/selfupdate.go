package main

import (
	"fmt"
	"os"
	"path/filepath"
)

// selfExeAsset is the standalone CLI published alongside each release's overlay bundle.
const selfExeAsset = "snaphak.exe"

// shouldSelfUpdate decides whether the running snaphak.exe should replace itself with the resolved release's
// exe. It stays put for a dev/local build (version "dev" -- never clobber a hand-built binary) or when the
// running version already matches the release tag.
func shouldSelfUpdate(runningVersion, releaseTag string) bool {
	if runningVersion == "" || runningVersion == "dev" {
		return false
	}
	if releaseTag == "" || releaseTag == runningVersion {
		return false
	}
	return true
}

// selfUpdate refreshes snaphak.exe itself from the resolved release. It is BEST-EFFORT and called AFTER the
// overlay update, so a self-update problem never blocks the real install -- every failure just prints a note.
// The new exe takes effect on the NEXT run (a running Windows image can't overwrite itself in place).
func selfUpdate(f flags, token string) {
	rel, err := fetchRelease(f, token)
	if err != nil {
		fmt.Printf("(couldn't check for a snaphak.exe update: %v)\n", err)
		return
	}
	if !shouldSelfUpdate(version, rel.TagName) {
		return // already current, or a dev build
	}
	var asset *ghAsset
	for i := range rel.Assets {
		if rel.Assets[i].Name == selfExeAsset {
			asset = &rel.Assets[i]
			break
		}
	}
	if asset == nil {
		return // this release doesn't ship a standalone exe
	}

	exe, err := os.Executable()
	if err != nil {
		fmt.Printf("(couldn't locate the running snaphak.exe to update it: %v)\n", err)
		return
	}
	tmp, err := os.MkdirTemp("", "snaphak-exe-")
	if err != nil {
		return
	}
	defer os.RemoveAll(tmp)
	newExe := filepath.Join(tmp, selfExeAsset)
	if err := downloadAsset(asset, token, newExe); err != nil {
		fmt.Printf("(couldn't download the new snaphak.exe: %v)\n", err)
		return
	}
	if err := replaceExe(exe, newExe); err != nil {
		fmt.Printf("(couldn't replace snaphak.exe (%v) -- the overlay updated fine; you can grab the new snaphak.exe from the release if needed)\n", err)
		return
	}
	// Keep the stable %LOCALAPPDATA% copy current too, if we're running from somewhere else.
	if dir := appDataDir(); dir != "" {
		if stable := filepath.Join(dir, selfExeAsset); !sameFile(stable, exe) {
			if os.MkdirAll(dir, 0o755) == nil {
				_ = copyFile(newExe, stable) // best-effort
			}
		}
	}
	fmt.Printf("Updated snaphak.exe to %s (takes effect next time you run snaphak).\n", rel.TagName)
}

// replaceExe swaps a (possibly running) exe for a new one, the Windows way: rename the current file aside to
// <path>.old -- allowed even while it runs -- then copy the new bytes into <path>. The .old copy is the still-
// running image and can't be deleted yet; cleanupSelfUpdateLeftovers removes it on the next launch. On a copy
// failure it rolls the rename back so the caller is never left without an exe.
func replaceExe(path, newExe string) error {
	old := path + ".old"
	_ = os.Remove(old) // clear any stale leftover first
	if err := os.Rename(path, old); err != nil {
		return err
	}
	if err := copyFile(newExe, path); err != nil {
		_ = os.Rename(old, path) // roll back
		return err
	}
	return nil
}

// cleanupSelfUpdateLeftovers best-effort deletes the <exe>.old file a previous self-update left behind (it
// couldn't be removed while it was still the running image). Called once at startup.
func cleanupSelfUpdateLeftovers() {
	if exe, err := os.Executable(); err == nil {
		_ = os.Remove(exe + ".old")
	}
}
