package main

import (
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// The stable client GUID of the Microsoft Edge WebView2 *Runtime* (Evergreen). When the runtime is installed
// it registers under EdgeUpdate\Clients\{GUID} with a non-empty "pv" (product version) -- machine-wide under
// HKLM (EdgeUpdate is 32-bit, so WOW6432Node on 64-bit Windows) or per-user under HKCU.
const webview2ClientGUID = `{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}`

// Microsoft's permanent "Evergreen Bootstrapper" link: a ~2 MB stub that downloads + installs the current
// WebView2 runtime. Documented at https://developer.microsoft.com/microsoft-edge/webview2/ .
const webview2BootstrapURL = "https://go.microsoft.com/fwlink/p/?LinkId=2124703"

// webview2Version returns the installed WebView2 runtime version, or "" if it is not installed. It reads the
// EdgeUpdate Clients registry via `reg query` (the same no-x/sys-dependency approach as steamPath), checking the
// machine-wide (HKLM WOW6432Node), per-user (HKCU), and native-HKLM locations.
func webview2Version() string {
	keys := []string{
		`HKLM\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\` + webview2ClientGUID,
		`HKCU\SOFTWARE\Microsoft\EdgeUpdate\Clients\` + webview2ClientGUID,
		`HKLM\SOFTWARE\Microsoft\EdgeUpdate\Clients\` + webview2ClientGUID,
	}
	for _, k := range keys {
		out, err := exec.Command("reg", "query", k, "/v", "pv").Output()
		if err != nil {
			continue
		}
		// a value line looks like:  "    pv    REG_SZ    150.0.4078.65"
		for _, line := range strings.Split(string(out), "\n") {
			if !strings.Contains(line, "pv") {
				continue
			}
			if i := strings.Index(line, "REG_SZ"); i >= 0 {
				v := strings.TrimSpace(line[i+len("REG_SZ"):])
				if v != "" && v != "0.0.0.0" {
					return v
				}
			}
		}
	}
	return ""
}

// ensureWebView2Runtime makes sure the WebView2 runtime (which the HTML frontend renders in) is present. It
// NEVER fails the install: the mod files are already deployed by the time this runs, so a missing runtime only
// means the UI won't render until it's installed. On most machines (Windows 11 / updated Windows 10) this is a
// no-op -- the runtime is already there. On install it prompts (auto-yes under --yes / non-interactive); on
// update it is a silent presence-check unless the runtime is actually missing.
func ensureWebView2Runtime(f flags) {
	if v := webview2Version(); v != "" {
		fmt.Printf("WebView2 runtime: present (%s).\n", v)
		return
	}
	fmt.Println("The SnapHak UI renders in Microsoft's Edge WebView2 runtime, which isn't installed on this machine.")
	if !f.yes && isInteractive() {
		if !confirm("Install the WebView2 runtime now (a ~2 MB download from Microsoft)?") {
			fmt.Println("Skipped -- SnapHak is installed, but its window won't appear until the runtime is present.")
			fmt.Println("Get it later from https://developer.microsoft.com/microsoft-edge/webview2/ (the \"Evergreen\" runtime).")
			return
		}
	} else {
		fmt.Println("Installing it now (a ~2 MB download from Microsoft)...")
	}
	if err := installWebView2Runtime(); err != nil {
		fmt.Fprintf(os.Stderr, "  ! couldn't install the WebView2 runtime automatically (%v).\n", err)
		fmt.Println("    SnapHak is installed. Get the runtime from https://developer.microsoft.com/microsoft-edge/webview2/")
		fmt.Println("    (the \"Evergreen Standalone Installer\") and the UI will work on the next launch.")
		return
	}
	if v := webview2Version(); v != "" {
		fmt.Printf("WebView2 runtime installed (%s).\n", v)
	} else {
		fmt.Println("WebView2 runtime installer finished.")
	}
}

// installWebView2Runtime downloads Microsoft's evergreen bootstrapper and runs it with the documented
// unattended flags (/silent /install).
func installWebView2Runtime() error {
	tmp := filepath.Join(os.TempDir(), fmt.Sprintf("MicrosoftEdgeWebview2Setup-%d.exe", time.Now().UnixNano()))
	if err := downloadTo(webview2BootstrapURL, tmp); err != nil {
		return fmt.Errorf("download failed: %w", err)
	}
	defer os.Remove(tmp)
	if out, err := exec.Command(tmp, "/silent", "/install").CombinedOutput(); err != nil {
		if s := strings.TrimSpace(string(out)); s != "" {
			return fmt.Errorf("%v: %s", err, s)
		}
		return err
	}
	return nil
}

// downloadTo fetches url into path (net/http follows the fwlink's redirect to the real installer).
func downloadTo(url, path string) error {
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", "snaphak-installer")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("HTTP %d", resp.StatusCode)
	}
	out, err := os.Create(path)
	if err != nil {
		return err
	}
	_, cerr := io.Copy(out, resp.Body)
	if closeErr := out.Close(); cerr == nil {
		cerr = closeErr
	}
	return cerr
}
