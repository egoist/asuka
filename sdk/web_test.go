package sdk

import (
	"os/exec"
	"path/filepath"
	"testing"
)

// The packages' JavaScript tests (web/*.test.js), under Node: each
// package's web code against the browser APIs it uses, stubbed.
func TestWebJS(t *testing.T) {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("no Node")
	}
	files, _ := filepath.Glob(filepath.Join("*", "web", "*.test.js"))
	std, _ := filepath.Glob(filepath.Join("..", "std", "*", "web", "*.test.js"))
	files = append(files, std...)
	if len(files) == 0 {
		t.Skip("no tests")
	}
	out, err := exec.Command(node, append([]string{"--test"}, files...)...).CombinedOutput()
	if err != nil {
		t.Fatalf("%v\n%s", err, out)
	}
}
