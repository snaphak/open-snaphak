package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
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

// assetMatchesFile reports whether the file on disk already has the release asset's exact bytes, via the
// asset's "sha256:<hex>" digest. False when the digest is absent -- the caller then downloads and compares.
func assetMatchesFile(a *ghAsset, path string) bool {
	hexDigest, ok := strings.CutPrefix(a.Digest, "sha256:")
	if !ok || hexDigest == "" {
		return false
	}
	sum, err := fileSHA256(path)
	return err == nil && sum == hexDigest
}

// selfUpdate refreshes snaphak.exe itself from the resolved release. It is BEST-EFFORT and called AFTER the
// overlay update, so a self-update problem never blocks the real install -- every failure just prints a note.
// The new exe takes effect on the NEXT run (a running Windows image can't overwrite itself in place).
//
// The version check compares the release tag against the RUNNING process's baked-in version -- which goes
// stale the moment a self-update succeeds (the process keeps reporting the version it was built as). So
// before touching anything, check what's actually ON DISK: if a previous run of this same session (or another
// window) already put the release's bytes in place, there is nothing to do -- without the disk check, every
// later update in the session would re-download the exe and then fail loudly trying to rename over
// snaphak.exe.old, which is the still-running old image Windows won't let go of.
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
	if assetMatchesFile(asset, exe) {
		return // the on-disk exe is already this release (an earlier run updated it) -- nothing to do
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
	if onDisk, err := fileSHA256(exe); err == nil {
		if fresh, err := fileSHA256(newExe); err == nil && fresh == onDisk {
			return // same bytes already in place (release had no digest to catch this earlier)
		}
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
// <path>.old -- allowed even while it runs -- then copy the new bytes into <path>. The aside may be a still-
// running image that can't be deleted or renamed over yet (e.g. this session already self-updated once); when
// <path>.old is wedged like that, fall back to the first free <path>.old<N> name. Leftovers are swept by
// cleanupSelfUpdateLeftovers on the next launch. On a copy failure it rolls the rename back so the caller is
// never left without an exe.
func replaceExe(path, newExe string) error {
	var err error
	for i := 0; i < 10; i++ {
		old := path + ".old"
		if i > 0 {
			old = fmt.Sprintf("%s.old%d", path, i+1)
		}
		_ = os.Remove(old) // clear any stale leftover first
		if err = os.Rename(path, old); err != nil {
			continue // this aside name is held by a still-running image -- try the next one
		}
		if cerr := copyFile(newExe, path); cerr != nil {
			_ = os.Rename(old, path) // roll back
			return cerr
		}
		return nil
	}
	return err
}

// cleanupSelfUpdateLeftovers best-effort deletes the <exe>.old* files previous self-updates left behind
// (an aside can't be removed while it is still a running image). Called once at startup.
func cleanupSelfUpdateLeftovers() {
	if exe, err := os.Executable(); err == nil {
		removeOldLeftovers(exe)
	}
}

// removeOldLeftovers deletes every sibling of exe named <base>.old, <base>.old2, ... Best-effort.
func removeOldLeftovers(exe string) {
	prefix := filepath.Base(exe) + ".old"
	entries, err := os.ReadDir(filepath.Dir(exe))
	if err != nil {
		return
	}
	for _, e := range entries {
		if !e.IsDir() && strings.HasPrefix(e.Name(), prefix) {
			_ = os.Remove(filepath.Join(filepath.Dir(exe), e.Name()))
		}
	}
}
