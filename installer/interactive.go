package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"strings"
	"time"
)

var stdin = bufio.NewReader(os.Stdin)

// interactiveMain is the double-click / no-args experience: a status-aware prompt. Not installed ->
// Enter installs (the one-keypress first run). Installed with a newer release available -> an update
// notice, Enter updates. Either way it then drops into a command prompt, so update / uninstall /
// changelog / status all work from a double-click -- no terminal or PATH needed.
func interactiveMain() {
	fmt.Println("SnapHak", version)
	fmt.Println()

	rec, err := loadRecord()
	installed := err == nil
	installedVersion := ""
	if installed {
		installedVersion = rec.Version
		fmt.Printf("Installed: SnapHak %s at %s\n", rec.Version, rec.DoomPath)
	} else {
		fmt.Println("SnapHak is not installed yet.")
		if doom, derr := resolveDoom(""); derr == nil {
			fmt.Printf("Found DOOM: %s\n", doom)
		} else {
			fmt.Println("(couldn't find DOOM automatically -- you can type:  install --doom \"C:\\path\\to\\DOOM\")")
		}
	}

	latest := ""
	if installed {
		latest = latestDefaultTag()
	}
	enter := enterDefault(installed, installedVersion, latest)
	switch enter {
	case "install":
		fmt.Println("\nPress Enter to install now, or type a command (help lists them).")
	case "update":
		fmt.Printf("\nA new version of SnapHak is available: %s (you have %s).\n", latest, installedVersion)
		fmt.Println("Press Enter to update now, or type a command (help lists them).")
	default:
		if installed && latest != "" {
			fmt.Println("You're up to date.")
		}
		fmt.Println("\nType a command (help lists them), or quit to close.")
	}
	commandLoop(enter)
}

// enterDefault is the command a bare Enter runs at the interactive prompt: "install" when not
// installed, "update" when a release newer than the installed one is known, "" otherwise (Enter just
// re-prompts). Pure so it is unit-testable.
func enterDefault(installed bool, installedVersion, latestTag string) string {
	if !installed {
		return "install"
	}
	if latestTag != "" && latestTag != installedVersion {
		return "update"
	}
	return ""
}

// commandLoop reads and runs commands until quit / EOF. enterCmd is what the first bare Enter runs
// (the pre-selected obvious action); after firing once, a bare Enter just re-prompts.
func commandLoop(enterCmd string) {
	for {
		fmt.Print("\nsnaphak> ")
		line, ok := readLineEOF()
		if !ok {
			return // stdin closed (piped input ran out) -- never spin on EOF
		}
		fields := splitArgs(line)
		if len(fields) == 0 {
			if enterCmd == "" {
				continue
			}
			fields = []string{enterCmd}
		}
		enterCmd = ""
		cmd := fields[0]
		if cmd == "quit" || cmd == "exit" || cmd == "q" {
			return
		}
		if err := runCommand(cmd, fields[1:]); err != nil {
			if errors.Is(err, errUnknownCommand) {
				fmt.Printf("Unknown command %q -- type help for the list, or quit to close.\n", cmd)
				continue
			}
			fmt.Fprintln(os.Stderr, "error:", err)
		}
	}
}

// splitArgs tokenizes an interactive command line, honoring double quotes so paths with spaces work
// (install --doom "C:\Program Files (x86)\Steam\steamapps\common\DOOM"). Quote characters delimit;
// they never become part of a token. Pure so it is unit-testable.
func splitArgs(line string) []string {
	var out []string
	var cur strings.Builder
	inQuote := false
	flush := func() {
		if cur.Len() > 0 {
			out = append(out, cur.String())
			cur.Reset()
		}
	}
	for _, r := range line {
		switch {
		case r == '"':
			inQuote = !inQuote
		case !inQuote && (r == ' ' || r == '\t'):
			flush()
		default:
			cur.WriteRune(r)
		}
	}
	flush()
	return out
}

// noticeClient bounds the best-effort update check -- a double-click must never hang on a dead network.
var noticeClient = &http.Client{Timeout: 5 * time.Second}

// latestDefaultTag returns the tag a no-flags update would install (the newest stable release, else the
// newest beta while no stable exists), or "" when it can't be determined quickly (offline, rate-limited).
// Purely informational: a "" simply means no update notice is shown.
func latestDefaultTag() string {
	req, err := http.NewRequest("GET", "https://api.github.com/repos/"+repoSlug+"/releases?per_page=30", nil)
	if err != nil {
		return ""
	}
	req.Header.Set("User-Agent", "snaphak-installer")
	req.Header.Set("Accept", "application/vnd.github+json")
	if t := resolveToken(flags{}); t != "" {
		req.Header.Set("Authorization", "Bearer "+t)
	}
	resp, err := noticeClient.Do(req)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return ""
	}
	var list []ghRelease
	if json.NewDecoder(resp.Body).Decode(&list) != nil {
		return ""
	}
	if pick, _ := pickDefaultRelease(list); pick != nil {
		return pick.TagName
	}
	return ""
}

func readLine() string {
	s, _ := stdin.ReadString('\n')
	return cleanLine(s)
}

// readLineEOF reads a line and additionally reports whether stdin is still open -- false means EOF with
// nothing read (the interactive loop must stop rather than treat EOF as an endless bare Enter).
func readLineEOF() (string, bool) {
	s, err := stdin.ReadString('\n')
	if err != nil && s == "" {
		return "", false
	}
	return cleanLine(s), true
}

// cleanLine trims whitespace and any UTF-8 BOM (U+FEFF) that pasted or piped input can carry.
func cleanLine(s string) string {
	s = strings.TrimSpace(s)
	s = strings.TrimPrefix(s, string(rune(0xFEFF)))
	return strings.TrimSpace(s)
}

func isYes(s string) bool {
	s = strings.ToLower(strings.TrimSpace(s))
	return s == "" || s == "y" || s == "yes"
}

func pause() {
	fmt.Print("\nPress Enter to exit...")
	readLine()
}

// isInteractive reports whether stdin is a real terminal -- so we only prompt when a human can answer.
// Scripts / CI / piped input skip the confirmation automatically; force-skip with --yes.
func isInteractive() bool {
	fi, err := os.Stdin.Stat()
	return err == nil && (fi.Mode()&os.ModeCharDevice) != 0
}

// confirm prints a yes/no prompt and returns true on yes (Enter / "y" / "yes").
func confirm(prompt string) bool {
	fmt.Print(prompt + " [Y/n] ")
	return isYes(readLine())
}
