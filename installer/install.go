package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"
)

// installRecord is what an install wrote: where, which version, which files it placed, and which pre-existing
// files it backed up. Stored at %LOCALAPPDATA%\open-snaphak\install.json so uninstall reverses exactly this.
type installRecord struct {
	Version     string   `json:"version"`
	DoomPath    string   `json:"doom_path"`
	InstalledAt string   `json:"installed_at"`
	Files       []string `json:"files"`   // overlay-relative paths placed under DoomPath
	Backups     []backup `json:"backups"` // pre-existing files moved aside
}

type backup struct {
	Rel    string `json:"rel"`    // overlay-relative path that was already present
	Backup string `json:"backup"` // absolute path of the saved-aside copy
}

func recordPath() (string, error) {
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		return "", fmt.Errorf("couldn't find your local app-data folder (the LOCALAPPDATA environment variable is not set)")
	}
	dir := filepath.Join(base, "open-snaphak")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	return filepath.Join(dir, "install.json"), nil
}

func loadRecord() (*installRecord, error) {
	p, err := recordPath()
	if err != nil {
		return nil, err
	}
	data, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	var rec installRecord
	if err := json.Unmarshal(data, &rec); err != nil {
		return nil, err
	}
	return &rec, nil
}

func saveRecord(rec *installRecord) error {
	p, err := recordPath()
	if err != nil {
		return err
	}
	data, err := json.MarshalIndent(rec, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(p, data, 0o644)
}

func cmdInstall(f flags) error {
	doom, err := resolveDoom(f.doom)
	if err != nil {
		return err
	}
	if doomIsRunning() {
		return fmt.Errorf("DOOM is running -- close it and run this again (SnapHak's files can't be replaced while the game has them open)")
	}
	b, cleanup, err := acquireBundle(f)
	if err != nil {
		return err
	}
	defer cleanup()

	rec := &installRecord{
		Version:     bundleVersion(f),
		DoomPath:    doom,
		InstalledAt: time.Now().UTC().Format(time.RFC3339),
	}
	// FINAL gate -- runs only after every check passed (DOOM found, DOOM closed, bundle downloaded + verified).
	if !f.yes && isInteractive() {
		if !confirm(fmt.Sprintf("About to install SnapHak %s into\n  %s\nContinue?", rec.Version, doom)) {
			fmt.Println("Cancelled -- nothing was changed.")
			return nil
		}
	}
	fmt.Printf("Installing SnapHak into %s\n", doom)
	for _, e := range b.files {
		target := filepath.Join(doom, e.rel)
		// back up a pre-existing real file we'd overwrite (e.g. a genuine XINPUT1_3.dll)
		if st, err := os.Stat(target); err == nil && !st.IsDir() {
			bak := target + ".snaphak-bak"
			if _, err := os.Stat(bak); err != nil { // never clobber an existing backup
				if err := os.Rename(target, bak); err != nil {
					return fmt.Errorf("couldn't back up the existing %s (%v) -- check you can write to your DOOM folder (try running as administrator)", e.rel, err)
				}
				rec.Backups = append(rec.Backups, backup{Rel: e.rel, Backup: bak})
			}
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return fmt.Errorf("couldn't create a folder in your DOOM install (%v) -- check you have permission (try running as administrator)", err)
		}
		if err := copyFile(filepath.Join(b.root, e.rel), target); err != nil {
			return fmt.Errorf("couldn't write %s into your DOOM folder (%v) -- check you have permission (try running as administrator)", e.rel, err)
		}
		rec.Files = append(rec.Files, e.rel)
		fmt.Printf("  + %s\n", e.rel)
	}
	if err := saveRecord(rec); err != nil {
		return fmt.Errorf("installed the files, but couldn't write the install record (%v) -- uninstall may not fully clean up", err)
	}
	fmt.Printf("Done. SnapHak %s installed. Launch DOOM and open the SnapMap editor.\n", rec.Version)
	return nil
}

func cmdUpdate(f flags) error {
	rec, err := loadRecord()
	if err != nil {
		fmt.Println("No existing install found -- doing a fresh install.")
		return cmdInstall(f)
	}
	if f.doom == "" {
		f.doom = rec.DoomPath
	}
	fmt.Printf("Updating SnapHak in %s (current: %s)\n", rec.DoomPath, rec.Version)
	return cmdInstall(f)
}

func cmdUninstall(f flags) error {
	rec, err := loadRecord()
	if err != nil {
		return fmt.Errorf("SnapHak doesn't appear to be installed (no install record found) -- nothing to uninstall")
	}
	doom := rec.DoomPath
	if f.doom != "" {
		doom = f.doom
	}
	if doomIsRunning() {
		return fmt.Errorf("DOOM is running -- close it and run this again (its files are in use)")
	}
	if !f.yes && isInteractive() {
		if !confirm(fmt.Sprintf("About to remove SnapHak from\n  %s\nand restore vanilla. Continue?", doom)) {
			fmt.Println("Cancelled -- nothing was changed.")
			return nil
		}
	}
	fmt.Printf("Removing SnapHak from %s\n", doom)

	// 1) delete exactly the files this install placed (never whole directories)
	for _, rel := range rec.Files {
		target := filepath.Join(doom, rel)
		if err := os.Remove(target); err != nil && !os.IsNotExist(err) {
			fmt.Fprintf(os.Stderr, "  ! couldn't remove %s (%v) -- is DOOM still running?\n", rel, err)
			continue
		}
		fmt.Printf("  - %s\n", rel)
	}
	// 2) restore any files we backed up
	for _, bk := range rec.Backups {
		target := filepath.Join(doom, bk.Rel)
		if err := os.Rename(bk.Backup, target); err != nil {
			fmt.Fprintf(os.Stderr, "  ! couldn't restore %s (%v)\n", bk.Rel, err)
			continue
		}
		fmt.Printf("  ~ restored %s\n", bk.Rel)
	}
	// 3) clean up the dirs we created. snaphak_logs/ is unambiguously ours (runtime logs) -> remove it whole;
	//    snaphak/ + plugins/ only if now empty (a pre-existing tree is left intact).
	os.RemoveAll(filepath.Join(doom, "snaphak_logs"))
	removeIfEmpty(filepath.Join(doom, "snaphak"))
	removeIfEmpty(filepath.Join(doom, "plugins", "platforms"))
	removeIfEmpty(filepath.Join(doom, "plugins"))
	// 4) drop the record. The user's %USERPROFILE%\snaphak data is NEVER touched.
	if p, err := recordPath(); err == nil {
		os.Remove(p)
	}
	fmt.Println("Done. DOOM restored to vanilla. (Your SnapHak user data folder was left untouched.)")
	return nil
}

func cmdStatus(f flags) error {
	rec, err := loadRecord()
	if err != nil {
		fmt.Println("SnapHak is not installed (no install record).")
		return nil
	}
	fmt.Printf("SnapHak %s\n", rec.Version)
	fmt.Printf("  DOOM:      %s\n", rec.DoomPath)
	fmt.Printf("  Installed: %s\n", rec.InstalledAt)
	fmt.Printf("  Files:     %d\n", len(rec.Files))
	fmt.Printf("  Backups:   %d\n", len(rec.Backups))
	missing := 0
	for _, rel := range rec.Files {
		if _, err := os.Stat(filepath.Join(rec.DoomPath, rel)); err != nil {
			missing++
		}
	}
	if missing > 0 {
		fmt.Printf("  WARNING:   %d installed file(s) missing from the DOOM folder\n", missing)
	}
	return nil
}

// cmdVersion prints the installer's own version and, if a mod is installed, the installed mod version.
func cmdVersion() {
	fmt.Println("snaphak", version)
	if rec, err := loadRecord(); err == nil {
		fmt.Printf("installed mod: %s  (in %s)\n", rec.Version, rec.DoomPath)
	}
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(out, in)
	closeErr := out.Close()
	if copyErr != nil {
		return copyErr
	}
	return closeErr
}

func removeIfEmpty(dir string) {
	entries, err := os.ReadDir(dir)
	if err == nil && len(entries) == 0 {
		os.Remove(dir)
	}
}

// bundleVersion derives a label for the install record: the release tag, "local", or the installer's version.
func bundleVersion(f flags) string {
	switch {
	case f.release != "":
		return f.release
	case f.local != "":
		return "local"
	default:
		return version
	}
}
