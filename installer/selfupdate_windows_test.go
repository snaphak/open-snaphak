//go:build windows

package main

import (
	"path/filepath"
	"syscall"
	"testing"
)

// TestReplaceExeLockedOldFallsBack reproduces the field failure: snaphak.exe.old is a still-running old
// image, which Windows will neither delete nor rename over. We simulate that lock with a handle opened
// WITHOUT FILE_SHARE_DELETE. replaceExe must fall back to a free .old<N> aside name and still succeed.
func TestReplaceExeLockedOldFallsBack(t *testing.T) {
	tmp := t.TempDir()
	path := filepath.Join(tmp, "snaphak.exe")
	writeF(t, path, "CURRENT")
	writeF(t, path+".old", "RUNNING-OLD-IMAGE")
	newExe := filepath.Join(tmp, "new", "snaphak.exe")
	writeF(t, newExe, "NEW")

	h, err := syscall.CreateFile(syscall.StringToUTF16Ptr(path+".old"),
		syscall.GENERIC_READ, syscall.FILE_SHARE_READ, nil, syscall.OPEN_EXISTING, 0, 0)
	if err != nil {
		t.Fatalf("locking .old: %v", err)
	}
	defer syscall.CloseHandle(h)

	if err := replaceExe(path, newExe); err != nil {
		t.Fatalf("replaceExe with a locked .old: %v", err)
	}
	if got := readF(t, path); got != "NEW" {
		t.Errorf("after replace, exe = %q, want NEW", got)
	}
	if got := readF(t, path+".old"); got != "RUNNING-OLD-IMAGE" {
		t.Errorf("the locked .old must be left alone, got %q", got)
	}
	if got := readF(t, path+".old2"); got != "CURRENT" {
		t.Errorf("the replaced exe should be aside at .old2, got %q", got)
	}
}
