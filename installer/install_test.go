package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"
)

// TestInstallUninstallRoundTrip exercises install -> uninstall against a synthetic bundle + fake DOOM dir.
// Fully hermetic (no real DOOM, no network): it verifies the overlay deploys, a pre-existing file is backed
// up, a legacy root-level snaphak_logs/ is migrated into snapmap-plus/logs/ on install, and uninstall restores
// the original + removes everything it placed (including the runtime-logs dir).
func TestInstallUninstallRoundTrip(t *testing.T) {
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	dist := filepath.Join(tmp, "dist")
	mkdirAll(t, doom)
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))

	// a pre-existing file to exercise backup/restore
	writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), "exe")
	writeF(t, filepath.Join(doom, "XINPUT1_3.dll"), "ORIGINAL")

	// a legacy root-level logs dir from an older install -- install must fold it into snapmap-plus\logs\
	writeF(t, filepath.Join(doom, "snaphak_logs", "old_backend.log"), "old log")

	// a synthetic dist with a MANIFEST.sha256 (matches package.ps1's "hash  relpath" format)
	overlay := map[string]string{
		"XINPUT1_3.dll": "backend",
		filepath.Join("snapmap-plus", "snapmap-plus-ui.dll"): "ui",
		filepath.Join("platforms", "qwindows.dll"):           "qt",
	}
	manifest := ""
	for rel, content := range overlay {
		writeF(t, filepath.Join(dist, rel), content)
		sum := sha256.Sum256([]byte(content))
		manifest += hex.EncodeToString(sum[:]) + "  " + rel + "\n"
	}
	writeF(t, filepath.Join(dist, "MANIFEST.sha256"), manifest)

	if err := cmdInstall(flags{doom: doom, local: dist}); err != nil {
		t.Fatalf("install: %v", err)
	}
	if !exists(filepath.Join(doom, "XINPUT1_3.dll.snapmap-plus-bak")) {
		t.Error("expected a backup of the pre-existing XINPUT1_3.dll")
	}
	if got := readF(t, filepath.Join(doom, "XINPUT1_3.dll")); got != "backend" {
		t.Errorf("deployed XINPUT1_3.dll = %q, want %q", got, "backend")
	}
	if got := readF(t, filepath.Join(doom, "snapmap-plus", "logs", "old_backend.log")); got != "old log" {
		t.Errorf("legacy log migrated = %q, want %q", got, "old log")
	}
	if exists(filepath.Join(doom, "snaphak_logs")) {
		t.Error("legacy snaphak_logs dir should be gone after migration")
	}

	// simulate runtime logs written since install that uninstall must clean
	writeF(t, filepath.Join(doom, "snapmap-plus", "logs", "sh_backend.log"), "log")

	if err := cmdUninstall(flags{}); err != nil {
		t.Fatalf("uninstall: %v", err)
	}
	if got := readF(t, filepath.Join(doom, "XINPUT1_3.dll")); got != "ORIGINAL" {
		t.Errorf("after uninstall XINPUT1_3.dll = %q, want the restored %q", got, "ORIGINAL")
	}
	for _, p := range []string{"snapmap-plus", "platforms", "XINPUT1_3.dll.snapmap-plus-bak"} {
		if exists(filepath.Join(doom, p)) {
			t.Errorf("expected %q removed after uninstall", p)
		}
	}
}

// TestMigrateLegacyLogs covers the legacy snaphak_logs -> snapmap-plus\logs fold directly (the round-trip
// test exercises it via cmdInstall too, but that path refuses while a real DOOM runs on the dev box -- this
// one always runs).
func TestMigrateLegacyLogs(t *testing.T) {
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	writeF(t, filepath.Join(doom, "snaphak_logs", "a.log"), "A")
	writeF(t, filepath.Join(doom, "snaphak_logs", "b.log"), "B")
	// a name collision: the new location already has b.log -> keep the new one, leave the old copy behind
	writeF(t, filepath.Join(doom, "snapmap-plus", "logs", "b.log"), "NEW")
	migrateLegacyLogs(doom)
	if got := readF(t, filepath.Join(doom, "snapmap-plus", "logs", "a.log")); got != "A" {
		t.Errorf("migrated a.log = %q, want %q", got, "A")
	}
	if got := readF(t, filepath.Join(doom, "snapmap-plus", "logs", "b.log")); got != "NEW" {
		t.Errorf("colliding b.log = %q, want the pre-existing %q kept", got, "NEW")
	}
	if !exists(filepath.Join(doom, "snaphak_logs", "b.log")) {
		t.Error("colliding old b.log should stay behind (old dir kept, since non-empty)")
	}
	// no collisions: a fresh legacy dir migrates fully and the emptied dir is removed
	doom2 := filepath.Join(tmp, "DOOM2")
	writeF(t, filepath.Join(doom2, "snaphak_logs", "only.log"), "x")
	migrateLegacyLogs(doom2)
	if exists(filepath.Join(doom2, "snaphak_logs")) {
		t.Error("emptied legacy dir should be removed after a full migration")
	}
	if got := readF(t, filepath.Join(doom2, "snapmap-plus", "logs", "only.log")); got != "x" {
		t.Errorf("migrated only.log = %q, want %q", got, "x")
	}
}

// TestUpdateDoesNotBackUpOwnDLL guards the vanilla-restore against the "backed up our own DLL" bug: on a clean
// DOOM dir (no genuine XINPUT1_3.dll -- the common case), a fresh install must create no backup, an update must
// NOT back up our own previously-installed DLL, and uninstall must then leave the dir with NO XINPUT1_3.dll
// (there was nothing genuine to restore) rather than "restoring" our own DLL as if it were vanilla.
func TestUpdateDoesNotBackUpOwnDLL(t *testing.T) {
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	mkdirAll(t, doom)
	t.Setenv("LOCALAPPDATA", filepath.Join(tmp, "appdata"))
	writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), "exe") // a clean DOOM dir -- NO pre-existing XINPUT1_3.dll

	if err := cmdInstall(flags{doom: doom, local: synthDist(t, tmp, "v1")}); err != nil {
		t.Fatalf("install v1: %v", err)
	}
	if exists(filepath.Join(doom, "XINPUT1_3.dll.snapmap-plus-bak")) {
		t.Fatal("fresh install into a clean dir must not create a backup (there was no genuine file)")
	}
	if err := cmdInstall(flags{doom: doom, local: synthDist(t, tmp, "v2")}); err != nil { // an update
		t.Fatalf("update: %v", err)
	}
	if exists(filepath.Join(doom, "XINPUT1_3.dll.snapmap-plus-bak")) {
		t.Error("update must not back up our own previously-installed DLL")
	}
	if err := cmdUninstall(flags{}); err != nil {
		t.Fatalf("uninstall: %v", err)
	}
	if exists(filepath.Join(doom, "XINPUT1_3.dll")) {
		t.Error("after uninstall a clean dir must have no XINPUT1_3.dll (nothing genuine to restore)")
	}
}

// TestRuntimeConfigSurvivesInstallerLifecycle locks the ownership boundary between the runtime and
// installer: config.json is player preference data, never an installed payload or cleanup target.
func TestRuntimeConfigSurvivesInstallerLifecycle(t *testing.T) {
	tmp := t.TempDir()
	doom := filepath.Join(tmp, "DOOM")
	localAppData := filepath.Join(tmp, "appdata")
	mkdirAll(t, doom)
	t.Setenv("LOCALAPPDATA", localAppData)
	t.Setenv("USERPROFILE", filepath.Join(tmp, "profile"))
	writeF(t, filepath.Join(doom, "DOOMx64vk.exe"), "exe")

	if err := cmdInstall(flags{doom: doom, local: synthDist(t, tmp, "config-v1")}); err != nil {
		t.Fatalf("install v1: %v", err)
	}
	configPath := filepath.Join(localAppData, "snapmap-plus", "config.json")
	configBytes := `{"schema_version":1,"settings":{"theme":"dark","future_setting":[1,true,null]},"future_root":{"keep":"exact"}}`
	writeF(t, configPath, configBytes)
	assertConfig := func(stage string) {
		t.Helper()
		if got := readF(t, configPath); got != configBytes {
			t.Fatalf("%s changed runtime config:\n got: %q\nwant: %q", stage, got, configBytes)
		}
	}

	if err := cmdUpdate(flags{local: synthDist(t, tmp, "config-v2"), noSelf: true}); err != nil {
		t.Fatalf("update: %v", err)
	}
	assertConfig("update")

	if err := cmdUninstall(flags{}); err != nil {
		t.Fatalf("uninstall: %v", err)
	}
	if exists(filepath.Join(localAppData, "snapmap-plus", "install.json")) {
		t.Error("uninstall should remove installer-owned install.json")
	}
	assertConfig("uninstall")

	if err := cmdInstall(flags{doom: doom, local: synthDist(t, tmp, "config-v3")}); err != nil {
		t.Fatalf("reinstall: %v", err)
	}
	assertConfig("reinstall")
}

// synthDist writes a synthetic dist/ (the 3 overlay files + a matching MANIFEST.sha256) whose content varies by
// tag, so installing two different tags exercises the update path (distinct bytes -> distinct hashes).
func synthDist(t *testing.T, dir, tag string) string {
	t.Helper()
	dist := filepath.Join(dir, "dist-"+tag)
	overlay := map[string]string{
		"XINPUT1_3.dll": "backend-" + tag,
		filepath.Join("snapmap-plus", "snapmap-plus-ui.dll"): "ui-" + tag,
		filepath.Join("platforms", "qwindows.dll"):           "qt-" + tag,
	}
	manifest := ""
	for rel, content := range overlay {
		writeF(t, filepath.Join(dist, rel), content)
		sum := sha256.Sum256([]byte(content))
		manifest += hex.EncodeToString(sum[:]) + "  " + rel + "\n"
	}
	writeF(t, filepath.Join(dist, "MANIFEST.sha256"), manifest)
	return dist
}

func mkdirAll(t *testing.T, dir string) {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
}

func writeF(t *testing.T, path, content string) {
	t.Helper()
	mkdirAll(t, filepath.Dir(path))
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func readF(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}
