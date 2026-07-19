package main

import (
	"os"
	"path/filepath"
	"testing"
)

// newDataDirs points LOCALAPPDATA and USERPROFILE at fresh temp dirs so every path helper (appDataDir,
// oldAppDataDir, oldUserContentDir) resolves under the test's control. Returns (localAppData, userProfile).
func newDataDirs(t *testing.T) (string, string) {
	t.Helper()
	la := t.TempDir()
	up := t.TempDir()
	t.Setenv("LOCALAPPDATA", la)
	t.Setenv("USERPROFILE", up)
	return la, up
}

// TestMigrateUserData_scaffolds: a fresh profile with no old content still gets the empty content tree created.
func TestMigrateUserData_scaffolds(t *testing.T) {
	la, _ := newDataDirs(t)
	migrateUserData()
	for _, sub := range userContentSubdirs {
		if _, err := os.Stat(filepath.Join(la, "snapmap-plus", sub)); err != nil {
			t.Errorf("expected scaffolded subfolder %q, got: %v", sub, err)
		}
	}
}

// TestMigrateUserData_foldsOldContentForward: content in the old %USERPROFILE%\snaphak\ tree is copied into the
// new app-data dir, and the old copy is left untouched as a backup.
func TestMigrateUserData_foldsOldContentForward(t *testing.T) {
	la, up := newDataDirs(t)
	oldOverride := filepath.Join(up, "snaphak", "overrides", "unknown_entity.decl")
	if err := os.MkdirAll(filepath.Dir(oldOverride), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(oldOverride, []byte("MY OVERRIDE"), 0o644); err != nil {
		t.Fatal(err)
	}

	migrateUserData()

	got, err := os.ReadFile(filepath.Join(la, "snapmap-plus", "overrides", "unknown_entity.decl"))
	if err != nil || string(got) != "MY OVERRIDE" {
		t.Fatalf("content not migrated to the new location: %q, %v", got, err)
	}
	// It's a VERIFIED MOVE: once every file is mirrored at the new location, the old home-root folder is
	// removed (not left as a stale backup).
	if _, err := os.Stat(filepath.Join(up, "snaphak")); !os.IsNotExist(err) {
		t.Errorf("the old %%USERPROFILE%%\\snaphak folder should be removed after a full migration, but it still exists")
	}
}

// TestMigrateUserData_neverClobbersNewer: if the user already has a file at the new location, an old-location
// file of the same name must NOT overwrite it (and re-running is a no-op).
func TestMigrateUserData_neverClobbersNewer(t *testing.T) {
	la, up := newDataDirs(t)
	name := filepath.Join("overrides", "keep.decl")

	oldFile := filepath.Join(up, "snaphak", name)
	os.MkdirAll(filepath.Dir(oldFile), 0o755)
	os.WriteFile(oldFile, []byte("OLD"), 0o644)

	newFile := filepath.Join(la, "snapmap-plus", name)
	os.MkdirAll(filepath.Dir(newFile), 0o755)
	os.WriteFile(newFile, []byte("NEW"), 0o644)

	migrateUserData()
	migrateUserData() // idempotent

	got, _ := os.ReadFile(newFile)
	if string(got) != "NEW" {
		t.Errorf("migration clobbered newer content: got %q, want %q", got, "NEW")
	}
}

// TestLoadRecord_fallsBackToOldLocation: a record written by a pre-rename install (at %LOCALAPPDATA%\open-snaphak)
// is found by loadRecord and migrated forward to the new location.
func TestLoadRecord_fallsBackToOldLocation(t *testing.T) {
	la, _ := newDataDirs(t)
	oldDir := filepath.Join(la, "open-snaphak")
	os.MkdirAll(oldDir, 0o755)
	os.WriteFile(filepath.Join(oldDir, "install.json"), []byte(`{"version":"v0.9.9","doom_path":"D:\\DOOM"}`), 0o644)

	rec, err := loadRecord()
	if err != nil {
		t.Fatalf("loadRecord should have found the old record: %v", err)
	}
	if rec.Version != "v0.9.9" {
		t.Errorf("wrong version from fallback record: %q", rec.Version)
	}
	if _, err := os.Stat(filepath.Join(la, "snapmap-plus", "install.json")); err != nil {
		t.Errorf("the old record should have been migrated forward to the new location: %v", err)
	}
}

// TestResolveToken_fallsBackToOldLocation: a token saved by a pre-rename install is still honored.
func TestResolveToken_fallsBackToOldLocation(t *testing.T) {
	la, _ := newDataDirs(t)
	oldDir := filepath.Join(la, "open-snaphak")
	os.MkdirAll(oldDir, 0o755)
	os.WriteFile(filepath.Join(oldDir, "token"), []byte("ghp_oldtoken\n"), 0o600)

	if got := resolveToken(flags{}); got != "ghp_oldtoken" {
		t.Errorf("resolveToken should fall back to the old token location: got %q", got)
	}
}
