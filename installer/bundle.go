package main

import (
	"archive/zip"
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
)

// repoSlug is the GitHub "owner/repo" the installer downloads releases from.
const repoSlug = "snaphak/open-snaphak"

// releaseAsset is the stable asset name CI publishes on every release (so latest/download works).
const releaseAsset = "snaphak-bundle.zip"

// bundle is a ready-to-deploy overlay tree (a dist/ dir) plus its MANIFEST.sha256 file list.
type bundle struct {
	root    string
	files   []manifestEntry
	version string // the resolved release tag (e.g. v0.1.0-beta.1), or "local"
}

type manifestEntry struct {
	rel    string // overlay-relative path, e.g. "snaphak\snaphakui.dll"
	sha256 string
}

// acquireBundle returns a verified bundle (from --local, else a downloaded release) plus a cleanup func.
func acquireBundle(f flags) (*bundle, func(), error) {
	noop := func() {}
	if f.local != "" {
		b, err := loadBundle(f.local)
		if b != nil {
			b.version = "local"
		}
		return b, noop, err
	}
	dir, tag, cleanup, err := downloadRelease(f)
	if err != nil {
		return nil, noop, err
	}
	b, err := loadBundle(dir)
	if err != nil {
		cleanup()
		return nil, noop, err
	}
	b.version = tag
	return b, cleanup, nil
}

// loadBundle reads dist/MANIFEST.sha256 and verifies every listed file is present and hash-correct,
// so we never start writing into a DOOM install from a partial or tampered bundle.
func loadBundle(root string) (*bundle, error) {
	f, err := os.Open(filepath.Join(root, "MANIFEST.sha256"))
	if err != nil {
		return nil, fmt.Errorf("%q isn't a SnapHak build folder (no MANIFEST.sha256) -- point --local at a dist/ folder built by package.ps1", root)
	}
	defer f.Close()

	var entries []manifestEntry
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			return nil, fmt.Errorf("the build's MANIFEST is malformed (%q) -- re-download or rebuild with package.ps1", line)
		}
		entries = append(entries, manifestEntry{
			rel:    filepath.FromSlash(fields[1]),
			sha256: strings.ToLower(fields[0]),
		})
	}
	if len(entries) == 0 {
		return nil, fmt.Errorf("%q has an empty MANIFEST -- the build looks incomplete", root)
	}
	for _, e := range entries {
		got, err := fileSHA256(filepath.Join(root, e.rel))
		if err != nil {
			return nil, fmt.Errorf("the build is incomplete -- %s is missing. Rebuild with package.ps1, or re-download", e.rel)
		}
		if got != e.sha256 {
			return nil, fmt.Errorf("integrity check failed on %s (hash mismatch) -- the download may be corrupted or tampered. Try again", e.rel)
		}
	}
	return &bundle{root: root, files: entries}, nil
}

func fileSHA256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// --- GitHub release resolution ------------------------------------------------------------------------
// install/update download the release bundle from GitHub via the API. A PRIVATE repo (closed beta) needs a
// token -- set once with `snaphak set-token <tok>`, or via SNAPHAK_TOKEN / --token; it also enables --beta.
// When the repo is public, no token is needed.

type ghRelease struct {
	TagName     string    `json:"tag_name"`
	Name        string    `json:"name"`
	Prerelease  bool      `json:"prerelease"`
	PublishedAt string    `json:"published_at"`
	Body        string    `json:"body"`
	HTMLURL     string    `json:"html_url"`
	Assets      []ghAsset `json:"assets"`
}

type ghAsset struct {
	Name               string `json:"name"`
	URL                string `json:"url"`                  // API URL (token-authed private download)
	BrowserDownloadURL string `json:"browser_download_url"` // public URL
}

// downloadRelease resolves the requested release (stable / --beta pre-release / --release tag), downloads
// its snaphak-bundle.zip asset (with the token if set), and extracts it into a temp dir.
func downloadRelease(f flags) (dir string, tag string, cleanup func(), err error) {
	noop := func() {}
	token := resolveToken(f)

	rel, err := fetchRelease(f, token)
	if err != nil {
		return "", "", noop, err
	}
	var asset *ghAsset
	for i := range rel.Assets {
		if rel.Assets[i].Name == releaseAsset {
			asset = &rel.Assets[i]
			break
		}
	}
	if asset == nil {
		return "", "", noop, fmt.Errorf("release %s is missing its download -- it may still be publishing, so try again in a minute", rel.TagName)
	}

	tmp, err := os.MkdirTemp("", "snaphak-bundle-")
	if err != nil {
		return "", "", noop, err
	}
	cleanup = func() { os.RemoveAll(tmp) }
	zipPath := filepath.Join(tmp, releaseAsset)
	if err := downloadAsset(asset, token, zipPath); err != nil {
		cleanup()
		return "", "", noop, err
	}
	if err := unzip(zipPath, tmp); err != nil {
		cleanup()
		return "", "", noop, err
	}
	return tmp, rel.TagName, cleanup, nil
}

// fetchRelease picks: an explicit --release tag, the latest --beta pre-release, or the latest stable.
func fetchRelease(f flags, token string) (*ghRelease, error) {
	base := "https://api.github.com/repos/" + repoSlug + "/releases"
	switch {
	case f.release != "":
		var r ghRelease
		return &r, apiGet(base+"/tags/"+f.release, token, &r)
	case f.beta:
		var list []ghRelease
		if err := apiGet(base+"?per_page=30", token, &list); err != nil {
			return nil, err
		}
		for i := range list {
			if list[i].Prerelease {
				return &list[i], nil
			}
		}
		return nil, fmt.Errorf("no beta build is available yet")
	default:
		var r ghRelease
		return &r, apiGet(base+"/latest", token, &r)
	}
}

func apiGet(url, token string, out interface{}) error {
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "snaphak-installer")
	req.Header.Set("Accept", "application/vnd.github+json")
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return fmt.Errorf("couldn't reach GitHub (%v) -- check your internet connection and try again", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("%s", friendlyHTTP(resp.StatusCode, token))
	}
	return json.NewDecoder(resp.Body).Decode(out)
}

// friendlyHTTP turns a GitHub API status code into a plain-English, actionable message.
func friendlyHTTP(code int, token string) string {
	switch code {
	case http.StatusUnauthorized: // 401
		return "your access token was rejected (401) -- double-check it with whoever gave it to you, then run: snaphak set-token <token>"
	case http.StatusForbidden: // 403
		return "GitHub denied or rate-limited the request (403) -- wait a few minutes and try again"
	case http.StatusNotFound: // 404
		if token == "" {
			return "couldn't find the release (404) -- if this is a private or beta build, set your access token first: snaphak set-token <token>"
		}
		return "release not found (404) -- check the version, or your token may not have access to this repository"
	default:
		return fmt.Sprintf("GitHub returned an error (HTTP %d) -- try again in a moment", code)
	}
}

// downloadAsset fetches the asset: with a token, the API asset URL + octet-stream (the only way to pull a
// private-repo asset); without, the public browser_download_url.
func downloadAsset(a *ghAsset, token, dest string) error {
	url, accept := a.BrowserDownloadURL, ""
	if token != "" {
		url, accept = a.URL, "application/octet-stream"
	}
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "snaphak-installer")
	if accept != "" {
		req.Header.Set("Accept", accept)
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return fmt.Errorf("couldn't download %s (%v) -- check your internet connection and try again", a.Name, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("couldn't download %s (HTTP %d) -- try again in a moment", a.Name, resp.StatusCode)
	}
	out, err := os.Create(dest)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, resp.Body)
	return err
}

// --- token storage ------------------------------------------------------------------------------------

func tokenPath() (string, error) {
	base := os.Getenv("LOCALAPPDATA")
	if base == "" {
		return "", fmt.Errorf("couldn't find your local app-data folder (the LOCALAPPDATA environment variable is not set)")
	}
	return filepath.Join(base, "open-snaphak", "token"), nil
}

// resolveToken: --token flag > SNAPHAK_TOKEN env > the saved token file.
func resolveToken(f flags) string {
	if f.token != "" {
		return f.token
	}
	if t := os.Getenv("SNAPHAK_TOKEN"); t != "" {
		return t
	}
	if p, err := tokenPath(); err == nil {
		if b, err := os.ReadFile(p); err == nil {
			return strings.TrimSpace(string(b))
		}
	}
	return ""
}

// cmdSetToken saves a GitHub token so install/update can pull from a private repo (closed beta).
func cmdSetToken(args []string) error {
	if len(args) < 1 || strings.TrimSpace(args[0]) == "" {
		return fmt.Errorf("usage: snaphak set-token <github-token>")
	}
	selfInstall() // a tester's first command -> keep a stable copy of snaphak.exe
	p, err := tokenPath()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return fmt.Errorf("couldn't create the SnapHak settings folder (%v)", err)
	}
	if err := os.WriteFile(p, []byte(strings.TrimSpace(args[0])), 0o600); err != nil {
		return fmt.Errorf("couldn't save your token (%v)", err)
	}
	fmt.Println("Token saved. You can now run:  snaphak update --beta")
	return nil
}

// cmdChangelog prints the release history. The NEWEST release's notes are printed IN FULL to the console --
// so you can read exactly what changed without leaving the terminal -- while OLDER releases are listed
// compactly with a link to their GitHub page. The notes are the commit log CI bakes into each release's body
// (see release.yml); that keeps the newest changelog self-contained (no link needed to read it).
func cmdChangelog(f flags) error {
	token := resolveToken(f)
	var list []ghRelease
	if err := apiGet("https://api.github.com/repos/"+repoSlug+"/releases?per_page=20", token, &list); err != nil {
		return err
	}
	if len(list) == 0 {
		fmt.Println("No releases have been published yet.")
		return nil
	}
	installed := ""
	if rec, err := loadRecord(); err == nil {
		installed = rec.Version
	}
	fmt.Print(formatChangelog(list, installed))
	return nil
}

// changelogPreviewLines caps how many notes lines an OLDER release shows before it defers to its GitHub link.
const changelogPreviewLines = 12

// formatChangelog renders the release history. The NEWEST release shows ALL of its notes (no truncation) plus
// a link to the full GitHub notes; each OLDER release shows a preview (up to changelogPreviewLines lines) then
// a "full notes" link when there is more. Pure (no I/O) so it is unit-testable. `list` is newest-first (the
// GitHub API order); `installed` is the currently-installed tag (marked with "<- you have this").
func formatChangelog(list []ghRelease, installed string) string {
	var b strings.Builder

	// The newest release: every line, no cap, then the link (in case a user wants the full per-commit notes).
	newest := list[0]
	fmt.Fprintf(&b, "Latest release: %s\n\n", releaseHeadline(newest, installed))
	writeNotes(&b, newest, -1)
	fmt.Fprintf(&b, "    Full notes: %s\n", newest.HTMLURL)

	// Older releases: a headline + a capped preview, deferring to the link when truncated.
	if len(list) > 1 {
		b.WriteString("\nEarlier releases:\n")
		for _, r := range list[1:] {
			fmt.Fprintf(&b, "\n  %s\n", releaseHeadline(r, installed))
			writeNotes(&b, r, changelogPreviewLines)
		}
	}
	return b.String()
}

// writeNotes writes a release's notes, indented. limit < 0 prints every line verbatim (blank lines kept). With
// limit >= 0 it prints at most `limit` non-blank lines and, if the notes are longer, appends a
// "... full notes: <link>" line instead of the remainder.
func writeNotes(b *strings.Builder, r ghRelease, limit int) {
	body := strings.TrimRight(r.Body, " \t\r\n")
	if body == "" {
		b.WriteString("    (no notes were published for this release)\n")
		return
	}
	shown := 0
	for _, line := range strings.Split(body, "\n") {
		if limit >= 0 {
			if strings.TrimSpace(line) == "" {
				continue // skip blank lines in the capped preview
			}
			if shown >= limit {
				fmt.Fprintf(b, "    ... full notes: %s\n", r.HTMLURL)
				return
			}
		}
		fmt.Fprintf(b, "    %s\n", strings.TrimRight(line, "\r"))
		shown++
	}
}

// releaseHeadline formats a one-line "tag (beta)   date   <- you have this" summary for a release.
func releaseHeadline(r ghRelease, installed string) string {
	kind := ""
	if r.Prerelease {
		kind = " (beta)"
	}
	date := r.PublishedAt
	if len(date) >= 10 {
		date = date[:10]
	}
	you := ""
	if r.TagName == installed {
		you = "   <- you have this"
	}
	return fmt.Sprintf("%s%s   %s%s", r.TagName, kind, date, you)
}

// unzip extracts src into dest, guarding against zip-slip path traversal.
func unzip(src, dest string) error {
	r, err := zip.OpenReader(src)
	if err != nil {
		return err
	}
	defer r.Close()
	prefix := filepath.Clean(dest) + string(os.PathSeparator)
	for _, zf := range r.File {
		target := filepath.Join(dest, filepath.FromSlash(zf.Name))
		if !strings.HasPrefix(target, prefix) {
			return fmt.Errorf("unsafe zip entry: %s", zf.Name)
		}
		if zf.FileInfo().IsDir() {
			if err := os.MkdirAll(target, 0o755); err != nil {
				return err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		rc, err := zf.Open()
		if err != nil {
			return err
		}
		out, err := os.Create(target)
		if err != nil {
			rc.Close()
			return err
		}
		_, copyErr := io.Copy(out, rc)
		out.Close()
		rc.Close()
		if copyErr != nil {
			return copyErr
		}
	}
	return nil
}
