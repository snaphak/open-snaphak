package main

import (
	"strings"
	"testing"
)

// TestFormatChangelogNewestFullPlusLink: the newest release prints EVERY notes line (no truncation) and adds a
// "Full notes:" link. Regression guard for the closed-beta report where the console showed only links.
func TestFormatChangelogNewestFullPlusLink(t *testing.T) {
	var lines []string
	for i := 1; i <= 30; i++ { // far more than changelogPreviewLines
		lines = append(lines, "- change number "+itoa(i))
	}
	newestBody := "Changes since v0.1.0:\n\n" + strings.Join(lines, "\n")
	list := []ghRelease{
		{TagName: "v0.2.0", Prerelease: true, PublishedAt: "2026-07-01T00:00:00Z", Body: newestBody,
			HTMLURL: "https://github.com/snaphak/open-snaphak/releases/tag/v0.2.0"},
	}

	out := formatChangelog(list, "v0.2.0")

	for _, ln := range lines { // every line present -> nothing truncated
		if !strings.Contains(out, ln) {
			t.Errorf("newest release note line missing from output: %q", ln)
		}
	}
	if strings.Contains(out, "... full notes:") {
		t.Error("the newest release must NOT truncate its notes")
	}
	if !strings.Contains(out, "Full notes: https://github.com/snaphak/open-snaphak/releases/tag/v0.2.0") {
		t.Errorf("the newest release must also offer its full-notes link; got:\n%s", out)
	}
	if !strings.Contains(out, "Latest release: v0.2.0 (beta)") {
		t.Errorf("missing latest-release headline; got:\n%s", out)
	}
	if !strings.Contains(out, "<- you have this") {
		t.Error("missing the installed-version marker")
	}
}

// TestFormatChangelogOlderPreviewAndTruncationLink: an older release shows a capped preview then defers to a
// "... full notes" link for the remainder.
func TestFormatChangelogOlderPreviewAndTruncationLink(t *testing.T) {
	var many []string
	for i := 1; i <= 20; i++ {
		many = append(many, "- old change "+itoa(i))
	}
	list := []ghRelease{
		{TagName: "v0.3.0", Prerelease: true, PublishedAt: "2026-07-05T00:00:00Z", Body: "- newest note",
			HTMLURL: "https://github.com/snaphak/open-snaphak/releases/tag/v0.3.0"},
		{TagName: "v0.2.0", Prerelease: true, PublishedAt: "2026-07-01T00:00:00Z", Body: strings.Join(many, "\n"),
			HTMLURL: "https://github.com/snaphak/open-snaphak/releases/tag/v0.2.0"},
	}

	out := formatChangelog(list, "")

	if !strings.Contains(out, "Earlier releases:") {
		t.Error("missing the 'Earlier releases:' section")
	}
	if !strings.Contains(out, "- old change "+itoa(changelogPreviewLines)) {
		t.Errorf("older release should show its first %d lines", changelogPreviewLines)
	}
	if strings.Contains(out, "- old change "+itoa(changelogPreviewLines+1)) {
		t.Errorf("older release should truncate after %d lines", changelogPreviewLines)
	}
	if !strings.Contains(out, "... full notes: https://github.com/snaphak/open-snaphak/releases/tag/v0.2.0") {
		t.Errorf("older release should defer to a truncation link; got:\n%s", out)
	}
}

// TestFormatChangelogOlderShortNoTruncationLink: a short older release shows all its lines and no truncation link.
func TestFormatChangelogOlderShortNoTruncationLink(t *testing.T) {
	list := []ghRelease{
		{TagName: "v0.3.0", Prerelease: true, PublishedAt: "2026-07-05T00:00:00Z", Body: "- newest",
			HTMLURL: "https://github.com/snaphak/open-snaphak/releases/tag/v0.3.0"},
		{TagName: "v0.2.0", Prerelease: true, PublishedAt: "2026-07-01T00:00:00Z", Body: "- a\n- b\n- c",
			HTMLURL: "https://github.com/snaphak/open-snaphak/releases/tag/v0.2.0"},
	}

	out := formatChangelog(list, "")

	for _, ln := range []string{"- a", "- b", "- c"} {
		if !strings.Contains(out, ln) {
			t.Errorf("short older release should show all lines; missing %q", ln)
		}
	}
	if strings.Contains(out, "... full notes: https://github.com/snaphak/open-snaphak/releases/tag/v0.2.0") {
		t.Error("a short older release should NOT have a truncation link")
	}
}

// TestFormatChangelogSingleRelease: one release -> full notes + link, and no "Earlier releases" section.
func TestFormatChangelogSingleRelease(t *testing.T) {
	list := []ghRelease{
		{TagName: "v0.1.0-beta.4", Prerelease: true, PublishedAt: "2026-07-01T12:34:56Z",
			Body:    "Changes since v0.1.0-beta.3:\n\n- fix the changelog command",
			HTMLURL: "https://github.com/snaphak/open-snaphak/releases/tag/v0.1.0-beta.4"},
	}
	out := formatChangelog(list, "")
	if !strings.Contains(out, "- fix the changelog command") {
		t.Errorf("full notes missing; got:\n%s", out)
	}
	if !strings.Contains(out, "Full notes: https://github.com/snaphak/open-snaphak/releases/tag/v0.1.0-beta.4") {
		t.Error("single release should still offer its full-notes link")
	}
	if strings.Contains(out, "Earlier releases:") {
		t.Error("a single release must not render an 'Earlier releases' section")
	}
	if strings.Contains(out, "2026-07-01T12:34:56Z") {
		t.Error("date should be trimmed to YYYY-MM-DD")
	}
	if !strings.Contains(out, "2026-07-01") {
		t.Error("expected the trimmed date")
	}
}

// TestReleaseHeadline covers the pure one-line formatter.
func TestReleaseHeadline(t *testing.T) {
	r := ghRelease{TagName: "v1.0.0", Prerelease: false, PublishedAt: "2026-08-15T09:00:00Z"}
	if got := releaseHeadline(r, "v1.0.0"); got != "v1.0.0   2026-08-15   <- you have this" {
		t.Errorf("stable installed headline = %q", got)
	}
	rb := ghRelease{TagName: "v1.1.0-beta.1", Prerelease: true, PublishedAt: "2026-09-01T00:00:00Z"}
	if got := releaseHeadline(rb, "v1.0.0"); got != "v1.1.0-beta.1 (beta)   2026-09-01" {
		t.Errorf("beta non-installed headline = %q", got)
	}
}

// itoa avoids importing strconv for the one-off table builders above.
func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var d []byte
	for n > 0 {
		d = append([]byte{byte('0' + n%10)}, d...)
		n /= 10
	}
	return string(d)
}
