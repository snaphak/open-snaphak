package main

import "testing"

// TestEnterDefault covers what a bare Enter does at the interactive prompt.
func TestEnterDefault(t *testing.T) {
	cases := []struct {
		name             string
		installed        bool
		installedVersion string
		latestTag        string
		want             string
	}{
		{"not installed -> install", false, "", "", "install"},
		{"not installed ignores latest", false, "", "v0.3.0", "install"},
		{"installed, newer available -> update", true, "v0.2.0-beta.4", "v0.2.0-beta.5", "update"},
		{"installed, up to date -> nothing", true, "v0.2.0-beta.5", "v0.2.0-beta.5", ""},
		{"installed, check failed -> nothing (no false notice)", true, "v0.2.0-beta.5", "", ""},
	}
	for _, c := range cases {
		if got := enterDefault(c.installed, c.installedVersion, c.latestTag); got != c.want {
			t.Errorf("%s: enterDefault(%v,%q,%q)=%q want %q", c.name, c.installed, c.installedVersion, c.latestTag, got, c.want)
		}
	}
}

// TestSplitArgs covers the interactive tokenizer: quoted paths with spaces stay one token, quotes
// never leak into tokens.
func TestSplitArgs(t *testing.T) {
	cases := []struct {
		in   string
		want []string
	}{
		{"", nil},
		{"   ", nil},
		{"update", []string{"update"}},
		{"install --beta --yes", []string{"install", "--beta", "--yes"}},
		{`install --doom "C:\Program Files (x86)\Steam\steamapps\common\DOOM"`,
			[]string{"install", "--doom", `C:\Program Files (x86)\Steam\steamapps\common\DOOM`}},
		{`install --doom "C:\DOOM"`, []string{"install", "--doom", `C:\DOOM`}},
		{`--release v0.2.0-beta.4`, []string{"--release", "v0.2.0-beta.4"}},
		{`say "unterminated quote`, []string{"say", "unterminated quote"}},
	}
	for _, c := range cases {
		got := splitArgs(c.in)
		if len(got) != len(c.want) {
			t.Errorf("splitArgs(%q)=%v want %v", c.in, got, c.want)
			continue
		}
		for i := range got {
			if got[i] != c.want[i] {
				t.Errorf("splitArgs(%q)[%d]=%q want %q", c.in, i, got[i], c.want[i])
			}
		}
	}
}
