// snaphak -- the open-snaphak installer.
//
// A single static Windows CLI that installs / updates / removes the SnapHak overlay in a DOOM 2016 install,
// with backup and an uninstall that restores vanilla. It detects DOOM, deploys the 6-file overlay (or
// downloads a release), and keeps a record so uninstall reverses exactly what it placed. Stdlib only, no
// external dependencies.
package main

import (
	"fmt"
	"os"
)

// version is set at build time: `go build -ldflags "-X main.version=v1.2.3" -o snaphak.exe .`
var version = "dev"

func usage() {
	fmt.Print(`snaphak -- open-snaphak installer (` + version + `)

Usage:
  snaphak install   [--doom <path>] [--local <dist-dir>] [--release <tag>] [--beta] [--yes]
  snaphak update    [--doom <path>] [--release <tag>] [--beta] [--no-self] [--yes]
  snaphak uninstall [--doom <path>] [--yes]
  snaphak changelog
  snaphak status
  snaphak version
  snaphak help

Options:
  --doom <path>       The DOOM install dir (the folder with DOOMx64vk.exe).
                      Auto-detected from your Steam libraries if omitted.
  --local <dist-dir>  Install from a local dist/ tree (built by package.ps1) instead of
                      downloading a release.
  --release <tag>     Install a specific release version instead of the latest.
  --beta              Install the latest beta (pre-release) instead of the latest stable.
  --no-self           With "update": don't also update snaphak.exe itself (overlay only).
  --yes, -y           Skip the "are you sure?" confirmation (for scripts / automation).

With no --local, install/update download from GitHub. Uninstall restores any files it
replaced and leaves your %USERPROFILE%\snaphak data untouched.
`)
}

func main() {
	cleanupSelfUpdateLeftovers() // remove any snaphak.exe.old a prior self-update left behind
	if len(os.Args) < 2 {
		// no args = a double-click -> the friendly interactive install (auto-detect, confirm, pause)
		interactiveInstall()
		return
	}
	cmd := os.Args[1]
	f := parseFlags(os.Args[2:])

	var err error
	switch cmd {
	case "install":
		err = cmdInstall(f)
	case "update":
		err = cmdUpdate(f)
	case "uninstall":
		err = cmdUninstall(f)
	case "status":
		err = cmdStatus(f)
	case "changelog", "info", "--info":
		err = cmdChangelog(f)
	case "set-token":
		err = cmdSetToken(os.Args[2:])
	case "version", "--version", "-v":
		cmdVersion()
	case "help", "-h", "--help":
		usage()
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n\n", cmd)
		usage()
		os.Exit(2)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

// flags holds the parsed command-line options.
type flags struct {
	doom    string
	local   string
	release string
	beta    bool
	token   string
	yes     bool
	noSelf  bool // with `update`: skip refreshing snaphak.exe itself
}

// parseFlags is a tiny "--key value" parser (the tool's option surface is small + fixed).
func parseFlags(a []string) flags {
	var f flags
	for i := 0; i < len(a); i++ {
		switch a[i] {
		case "--doom":
			if i+1 < len(a) {
				i++
				f.doom = a[i]
			}
		case "--local":
			if i+1 < len(a) {
				i++
				f.local = a[i]
			}
		case "--release":
			if i+1 < len(a) {
				i++
				f.release = a[i]
			}
		case "--beta":
			f.beta = true
		case "--no-self":
			f.noSelf = true
		case "--yes", "-y":
			f.yes = true
		case "--token":
			if i+1 < len(a) {
				i++
				f.token = a[i]
			}
		}
	}
	return f
}
