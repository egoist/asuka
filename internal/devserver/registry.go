package devserver

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// The running asuka devs, for editors: each notes its app's directory
// and its inspector's address in a file of its own, in this computer's
// cache (Register), so a language server finds the one that runs the
// app a file is in (Find) and asks it to show a preview, as the
// inspector's page does.

// An Entry is a running asuka dev: its app's directory and its
// inspector's address ("http://127.0.0.1:52817/").
type Entry struct {
	Dir string `json:"dir"`
	URL string `json:"url"`
	PID int    `json:"pid"`
}

// registryDir is where the entries are: ASUKA_DEV_REGISTRY, else the
// user's cache's asuka/dev.
func registryDir() (string, error) {
	if d := os.Getenv("ASUKA_DEV_REGISTRY"); d != "" {
		return d, nil
	}
	c, err := os.UserCacheDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(c, "asuka", "dev"), nil
}

func entryFile(dir string) (string, error) {
	r, err := registryDir()
	if err != nil {
		return "", err
	}
	h := sha256.Sum256([]byte(dir))
	return filepath.Join(r, hex.EncodeToString(h[:8])+".json"), nil
}

// Register notes that this process serves the app in dir, its inspector
// at url, and returns what forgets it (as asuka dev ends).
func Register(dir, url string) (forget func(), err error) {
	dir, err = filepath.Abs(dir)
	if err != nil {
		return func() {}, err
	}
	f, err := entryFile(dir)
	if err != nil {
		return func() {}, err
	}
	if err := os.MkdirAll(filepath.Dir(f), 0o755); err != nil {
		return func() {}, err
	}
	b, _ := json.Marshal(Entry{Dir: dir, URL: url, PID: os.Getpid()})
	if err := os.WriteFile(f, b, 0o644); err != nil {
		return func() {}, err
	}
	return func() {
		// Another asuka dev of the app's may have replaced it since.
		if old, err := os.ReadFile(f); err == nil && string(old) == string(b) {
			os.Remove(f)
		}
	}, nil
}

// Find is the running asuka dev whose app's directory holds file (the
// deepest, for an app inside another's directory): one that answers.
func Find(file string) (Entry, bool) {
	r, err := registryDir()
	if err != nil {
		return Entry{}, false
	}
	abs, err := filepath.Abs(file)
	if err != nil {
		return Entry{}, false
	}
	names, _ := filepath.Glob(filepath.Join(r, "*.json"))
	var best Entry
	for _, n := range names {
		b, err := os.ReadFile(n)
		if err != nil {
			continue
		}
		var e Entry
		if json.Unmarshal(b, &e) != nil || e.Dir == "" || e.URL == "" {
			continue
		}
		if abs != e.Dir && !strings.HasPrefix(abs, e.Dir+string(filepath.Separator)) {
			continue
		}
		if len(e.Dir) <= len(best.Dir) {
			continue
		}
		if !answers(e.URL) {
			os.Remove(n) // an asuka dev that ended without forgetting it
			continue
		}
		best = e
	}
	return best, best.Dir != ""
}

// answers reports whether an inspector answers at url.
func answers(url string) bool {
	c := http.Client{Timeout: 2 * time.Second}
	resp, err := c.Get(url + "previews")
	if err != nil {
		return false
	}
	resp.Body.Close()
	return resp.StatusCode == http.StatusOK
}
