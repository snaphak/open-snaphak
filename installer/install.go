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
// files it backed up. Stored at %LOCALAPPDATA%\snapmap-plus\install.json so uninstall reverses exactly this.
type installRecord struct {
	Version     string   `json:"version"`
	DoomPath    string   `json:"doom_path"`
	InstalledAt string   `json:"installed_at"`
	Files       []string `json:"files"`   // overlay-relative paths placed under DoomPath
	Backups     []backup `json:"backups"` // pre-existing files moved aside
	// LegacyRemoved: original-SnapHak files a migration deleted (informational; carried forward).
	LegacyRemoved []string `json:"legacy_removed,omitempty"`
}

type backup struct {
	Rel    string `json:"rel"`    // overlay-relative path that was already present
	Backup string `json:"backup"` // absolute path of the saved-aside copy
}

func recordPath() (string, error) {
	dir := appDataDir()
	if dir == "" {
		return "", fmt.Errorf("couldn't find your local app-data folder (the LOCALAPPDATA environment variable is not set)")
	}
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
		// Pre-rename installs kept the record at %LOCALAPPDATA%\open-snaphak\; fall back to it and migrate it
		// forward, so update / status / uninstall still recognise an existing install after the rename.
		op := oldRecordPath()
		od, oerr := os.ReadFile(op)
		if op == "" || oerr != nil {
			return nil, err
		}
		os.WriteFile(p, od, 0o644)
		data = od
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
	if f.local == "" {
		selfInstall() // a real (release) install -> keep a stable copy of snapmap-plus.exe; skip for dev --local builds
	}
	doom, err := resolveDoom(f.doom)
	if err != nil {
		return err
	}
	if doomIsRunning() {
		return fmt.Errorf("DOOM is running -- close it and run this again (Snapmap+'s files can't be replaced while the game has them open)")
	}
	b, cleanup, err := acquireBundle(f)
	if err != nil {
		return err
	}
	defer cleanup()

	// Files a PREVIOUS install placed are OUR own DLLs, not vanilla -- never "back them up" on an update, or
	// uninstall would later restore our DLL as if it were the genuine file and leave DOOM non-vanilla. Carry
	// the earlier install's genuine backups forward so uninstall still restores them. Best-effort: no prior
	// record (a first install) just yields empty sets.
	ours := map[string]bool{}
	var priorBackups []backup
	var priorLegacy []string
	if prior, err := loadRecord(); err == nil {
		for _, rel := range prior.Files {
			ours[rel] = true
		}
		priorBackups = prior.Backups
		priorLegacy = prior.LegacyRemoved
	}

	rec := &installRecord{
		Version:       b.version,
		DoomPath:      doom,
		InstalledAt:   time.Now().UTC().Format(time.RFC3339),
		Backups:       priorBackups,
		LegacyRemoved: priorLegacy,
	}
	// The original SnapHak in this DOOM folder? Snapmap+ replaces it -- migrate (remove its files)
	// as part of the install, after the confirmation below.
	legacy := detectLegacy(doom)
	confirmMsg := fmt.Sprintf("About to install Snapmap+ %s into\n  %s\nContinue?", rec.Version, doom)
	if len(legacy) > 0 {
		fmt.Println("Found the original SnapHak in this DOOM folder. Snapmap+ replaces it, so its")
		fmt.Printf("files (%d) will be removed. Your maps, prefabs and overrides under\n", len(legacy))
		fmt.Printf("%%USERPROFILE%%\\snaphak are yours and are kept (they carry over).\n")
		confirmMsg = fmt.Sprintf("About to remove the original SnapHak and install Snapmap+ %s into\n  %s\nContinue?", rec.Version, doom)
	}
	// FINAL gate -- runs only after every check passed (DOOM found, DOOM closed, bundle downloaded + verified).
	if !f.yes && isInteractive() {
		if !confirm(confirmMsg) {
			fmt.Println("Cancelled -- nothing was changed.")
			return nil
		}
	}
	if len(legacy) > 0 {
		fmt.Println("Removing the original SnapHak:")
		rec.Backups = dropLegacyBackups(doom, rec.Backups)
		rec.LegacyRemoved = append(rec.LegacyRemoved, removeLegacy(doom, legacy)...)
	}
	backedUp := map[string]bool{}
	for _, bk := range rec.Backups {
		backedUp[bk.Rel] = true
	}
	fmt.Printf("Installing Snapmap+ into %s\n", doom)
	for _, e := range b.files {
		target := filepath.Join(doom, e.rel)
		// Back up only a GENUINE pre-existing file we'd overwrite (e.g. a real XINPUT1_3.dll). Skip if a
		// previous install already attributed this path to us, or the on-disk file is byte-identical to the
		// DLL we're about to write (also ours) -- backing either up would corrupt the vanilla-restore.
		if st, err := os.Stat(target); err == nil && !st.IsDir() {
			ourFile := ours[e.rel]
			if !ourFile {
				if got, herr := fileSHA256(target); herr == nil && got == e.sha256 {
					ourFile = true
				}
			}
			if !ourFile && !backedUp[e.rel] {
				bak := target + ".snapmap-plus-bak"
				if _, err := os.Stat(bak); err != nil { // never clobber an existing backup
					if err := os.Rename(target, bak); err != nil {
						return fmt.Errorf("couldn't back up the existing %s (%v) -- check you can write to your DOOM folder (try running as administrator)", e.rel, err)
					}
					rec.Backups = append(rec.Backups, backup{Rel: e.rel, Backup: bak})
					backedUp[e.rel] = true
				}
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
	// Remove what a PRIOR install of OURS placed that this one no longer does -- notably a renamed overlay
	// (the old snaphak\snaphakui.dll, superseded by snapmap-plus\snapmap-plus-ui.dll). These paths come from
	// our OWN prior install record, so they are always ours to remove (never the original SnapHak's files).
	for rel := range ours {
		placed := false
		for _, r := range rec.Files {
			if r == rel {
				placed = true
				break
			}
		}
		if !placed {
			if os.Remove(filepath.Join(doom, rel)) == nil {
				fmt.Printf("  - removed superseded %s\n", rel)
			}
			removeIfEmpty(filepath.Dir(filepath.Join(doom, rel)))
		}
	}
	migrateLegacyLogs(doom) // runtime logs live in snapmap-plus\logs\ -- fold older locations (snaphak_logs\, snaphak\logs\) in (best-effort)
	migrateUserData()       // scaffold the app-data content tree + fold a user's old %USERPROFILE%\snaphak\ content forward (best-effort)
	fmt.Printf("Done. Snapmap+ %s installed.\n", rec.Version)
	ensureWebView2Runtime(f) // the HTML UI renders in the WebView2 runtime -- ensure it's present (never fails the install)
	fmt.Println("Launch DOOM and open the SnapMap editor.")
	return nil
}

// migrateLegacyLogs folds runtime logs from older locations -- the oldest releases' root-level snaphak_logs\,
// and the previous-name overlay's snaphak\logs\ -- into the current snapmap-plus\logs\, so an update doesn't
// strand old logs in a dir nothing writes to anymore, then drops the emptied old overlay dir. Best-effort: a
// file that won't move (locked, name collision) stays behind and its dir is left in place; never fails install.
func migrateLegacyLogs(doom string) {
	newDir := filepath.Join(doom, "snapmap-plus", "logs")
	foldLogsForward(filepath.Join(doom, "snaphak_logs"), newDir)
	foldLogsForward(filepath.Join(doom, "snaphak", "logs"), newDir)
	removeIfEmpty(filepath.Join(doom, "snaphak")) // the previous-name overlay dir, once its DLL + logs are gone
}

// foldLogsForward moves every file in old into newDir (keeping any that already exist there), then removes old
// if it ends up empty. Best-effort.
func foldLogsForward(old, newDir string) {
	entries, err := os.ReadDir(old)
	if err != nil {
		return // nothing to migrate
	}
	if err := os.MkdirAll(newDir, 0o755); err != nil {
		return
	}
	moved := false
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		dst := filepath.Join(newDir, e.Name())
		if _, err := os.Stat(dst); err == nil {
			continue // already exists at the new location -- keep it, leave the old copy
		}
		if os.Rename(filepath.Join(old, e.Name()), dst) == nil {
			moved = true
		}
	}
	if rest, err := os.ReadDir(old); err == nil && len(rest) == 0 {
		os.Remove(old)
		if moved {
			fmt.Println("  ~ moved runtime logs into snapmap-plus\\logs\\")
		}
	}
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
	fmt.Printf("Updating Snapmap+ in %s (current: %s)\n", rec.DoomPath, rec.Version)
	if err := cmdInstall(f); err != nil {
		return err
	}
	// The overlay is updated; now refresh snapmap-plus.exe itself (best-effort -- never fails the overlay update).
	if !f.noSelf {
		selfUpdate(f, resolveToken(f))
	}
	return nil
}

func cmdUninstall(f flags) error {
	rec, err := loadRecord()
	if err != nil {
		return fmt.Errorf("Snapmap+ doesn't appear to be installed (no install record found) -- nothing to uninstall")
	}
	doom := rec.DoomPath
	if f.doom != "" {
		doom = f.doom
	}
	if doomIsRunning() {
		return fmt.Errorf("DOOM is running -- close it and run this again (its files are in use)")
	}
	if !f.yes && isInteractive() {
		if !confirm(fmt.Sprintf("About to remove Snapmap+ from\n  %s\nand restore vanilla. Continue?", doom)) {
			fmt.Println("Cancelled -- nothing was changed.")
			return nil
		}
	}
	fmt.Printf("Removing Snapmap+ from %s\n", doom)

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
	// 3) clean up the dirs we created. The runtime-logs dir (snapmap-plus\logs\; releases before the
	//    rename used snaphak\logs\, the oldest a root-level snaphak_logs\) is unambiguously ours ->
	//    remove it whole; snapmap-plus/ + snaphak/ + platforms/ only if now empty (a pre-existing
	//    tree is left intact).
	os.RemoveAll(filepath.Join(doom, "snapmap-plus", "logs"))
	os.RemoveAll(filepath.Join(doom, "snaphak", "logs"))
	os.RemoveAll(filepath.Join(doom, "snaphak_logs"))
	removeIfEmpty(filepath.Join(doom, "snapmap-plus"))
	removeIfEmpty(filepath.Join(doom, "snaphak"))
	removeIfEmpty(filepath.Join(doom, "platforms"))
	// 4) auto-cleanup our app-data folder: the record, the saved token, and the stable snapmap-plus.exe copy.
	//    Runtime-owned config.json preferences and the user's modding data (the
	//    %LOCALAPPDATA%\snapmap-plus content folders and any old %USERPROFILE%\snaphak) are NEVER touched.
	cleanupAppData()
	fmt.Println("Done. DOOM restored to vanilla. (Your Snapmap+ modding data was left untouched.)")
	return nil
}

func cmdStatus(f flags) error {
	rec, err := loadRecord()
	if err != nil {
		fmt.Println("Snapmap+ is not installed (no install record).")
		return nil
	}
	fmt.Printf("Snapmap+ %s\n", rec.Version)
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
	fmt.Println("snapmap-plus", version)
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
