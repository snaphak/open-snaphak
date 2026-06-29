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
	root  string
	files []manifestEntry
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
		return b, noop, err
	}
	dir, cleanup, err := downloadRelease(f)
	if err != nil {
		return nil, noop, err
	}
	b, err := loadBundle(dir)
	if err != nil {
		cleanup()
		return nil, noop, err
	}
	return b, cleanup, nil
}

// loadBundle reads dist/MANIFEST.sha256 and verifies every listed file is present and hash-correct,
// so we never start writing into a DOOM install from a partial or tampered bundle.
func loadBundle(root string) (*bundle, error) {
	f, err := os.Open(filepath.Join(root, "MANIFEST.sha256"))
	if err != nil {
		return nil, fmt.Errorf("no MANIFEST.sha256 in %q (not a dist/ bundle): %w", root, err)
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
			return nil, fmt.Errorf("malformed MANIFEST line: %q", line)
		}
		entries = append(entries, manifestEntry{
			rel:    filepath.FromSlash(fields[1]),
			sha256: strings.ToLower(fields[0]),
		})
	}
	if len(entries) == 0 {
		return nil, fmt.Errorf("empty MANIFEST.sha256 in %q", root)
	}
	for _, e := range entries {
		got, err := fileSHA256(filepath.Join(root, e.rel))
		if err != nil {
			return nil, fmt.Errorf("bundle file missing: %s (%w)", e.rel, err)
		}
		if got != e.sha256 {
			return nil, fmt.Errorf("bundle file hash mismatch: %s", e.rel)
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
	TagName    string    `json:"tag_name"`
	Prerelease bool      `json:"prerelease"`
	Assets     []ghAsset `json:"assets"`
}

type ghAsset struct {
	Name               string `json:"name"`
	URL                string `json:"url"`                  // API URL (token-authed private download)
	BrowserDownloadURL string `json:"browser_download_url"` // public URL
}

// downloadRelease resolves the requested release (stable / --beta pre-release / --release tag), downloads
// its snaphak-bundle.zip asset (with the token if set), and extracts it into a temp dir.
func downloadRelease(f flags) (dir string, cleanup func(), err error) {
	noop := func() {}
	token := resolveToken(f)

	rel, err := fetchRelease(f, token)
	if err != nil {
		return "", noop, err
	}
	var asset *ghAsset
	for i := range rel.Assets {
		if rel.Assets[i].Name == releaseAsset {
			asset = &rel.Assets[i]
			break
		}
	}
	if asset == nil {
		return "", noop, fmt.Errorf("release %s has no %s asset", rel.TagName, releaseAsset)
	}

	tmp, err := os.MkdirTemp("", "snaphak-bundle-")
	if err != nil {
		return "", noop, err
	}
	cleanup = func() { os.RemoveAll(tmp) }
	zipPath := filepath.Join(tmp, releaseAsset)
	if err := downloadAsset(asset, token, zipPath); err != nil {
		cleanup()
		return "", noop, err
	}
	if err := unzip(zipPath, tmp); err != nil {
		cleanup()
		return "", noop, err
	}
	return tmp, cleanup, nil
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
		return nil, fmt.Errorf("no beta (pre-release) build is published yet")
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
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 400))
		hint := ""
		if resp.StatusCode == http.StatusNotFound && token == "" {
			hint = " (a private repo needs a token: run `snaphak set-token <tok>`)"
		}
		return fmt.Errorf("GitHub API HTTP %d%s: %s", resp.StatusCode, hint, strings.TrimSpace(string(body)))
	}
	return json.NewDecoder(resp.Body).Decode(out)
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
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("download %s: HTTP %d", a.Name, resp.StatusCode)
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
		return "", fmt.Errorf("LOCALAPPDATA is not set")
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
	p, err := tokenPath()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
		return err
	}
	if err := os.WriteFile(p, []byte(strings.TrimSpace(args[0])), 0o600); err != nil {
		return err
	}
	fmt.Println("Token saved. You can now run:  snaphak update --beta")
	return nil
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
