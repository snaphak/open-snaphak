package main

import (
	"path/filepath"
	"testing"
)

// TestShouldSelfUpdate covers the pure update decision.
func TestShouldSelfUpdate(t *testing.T) {
	cases := []struct {
		name         string
		running, tag string
		want         bool
	}{
		{"dev build never self-updates", "dev", "v0.1.0-beta.4", false},
		{"unknown running version", "", "v0.1.0-beta.4", false},
		{"already current", "v0.1.0-beta.4", "v0.1.0-beta.4", false},
		{"no release tag", "v0.1.0-beta.4", "", false},
		{"newer release", "v0.1.0-beta.3", "v0.1.0-beta.4", true},
	}
	for _, c := range cases {
		if got := shouldSelfUpdate(c.running, c.tag); got != c.want {
			t.Errorf("%s: shouldSelfUpdate(%q,%q)=%v want %v", c.name, c.running, c.tag, got, c.want)
		}
	}
}

// TestReplaceExe: the running exe is swapped for the new bytes, and the old bytes are preserved at <exe>.old
// (to be cleaned on the next launch).
func TestReplaceExe(t *testing.T) {
	tmp := t.TempDir()
	path := filepath.Join(tmp, "snaphak.exe")
	writeF(t, path, "OLD")
	newExe := filepath.Join(tmp, "new", "snaphak.exe")
	writeF(t, newExe, "NEW")

	if err := replaceExe(path, newExe); err != nil {
		t.Fatalf("replaceExe: %v", err)
	}
	if got := readF(t, path); got != "NEW" {
		t.Errorf("after replace, exe = %q, want NEW", got)
	}
	if got := readF(t, path+".old"); got != "OLD" {
		t.Errorf("expected the old exe preserved at .old, got %q", got)
	}
}

// TestReplaceExeRollbackOnMissingNew: if the new exe can't be copied in, the original is restored and no .old
// is left behind -- the caller is never left without a working exe.
func TestReplaceExeRollbackOnMissingNew(t *testing.T) {
	tmp := t.TempDir()
	path := filepath.Join(tmp, "snaphak.exe")
	writeF(t, path, "OLD")
	missing := filepath.Join(tmp, "does-not-exist.exe")

	if err := replaceExe(path, missing); err == nil {
		t.Fatal("expected an error when the new exe is missing")
	}
	if got := readF(t, path); got != "OLD" {
		t.Errorf("after a failed replace, exe should be rolled back to OLD, got %q", got)
	}
	if exists(path + ".old") {
		t.Error("a failed replace should leave no .old behind")
	}
}
