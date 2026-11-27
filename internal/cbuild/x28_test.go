package cbuild

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// pressure wants every register: values live across a call, more of them
// than arm64 has callee-saved registers.
const pressure = `
extern void g(void);
long f(long *p) {
	long a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3], a4 = p[4], a5 = p[5], a6 = p[6], a7 = p[7], a8 = p[8], a9 = p[9];
	long a10 = p[10], a11 = p[11], a12 = p[12], a13 = p[13], a14 = p[14], a15 = p[15], a16 = p[16], a17 = p[17], a18 = p[18], a19 = p[19];
	long a20 = p[20], a21 = p[21], a22 = p[22], a23 = p[23], a24 = p[24], a25 = p[25], a26 = p[26], a27 = p[27], a28 = p[28], a29 = p[29];
	g();
	return a0 * a1 + a2 * a3 + a4 * a5 + a6 * a7 + a8 * a9 + a10 * a11 + a12 * a13 + a14 * a15 + a16 * a17 + a18 * a19 +
	       a20 * a21 + a22 * a23 + a24 * a25 + a26 * a27 + a28 * a29;
}
`

var x28 = regexp.MustCompile(`\b[xw]28\b`)

// TestAppFlagsReserveX28 compiles code that wants every register with
// AppFlags, for each arm64 target with each compiler there is for it
// (Zig's too, which ignores -ffixed-x28), and checks that no instruction
// names x28: the ways into the app's C write it behind the compiler's
// back (core/src/vm/ops.h's AK_CG_ENTER), so a value the compiler kept
// there would be lost.
func TestAppFlagsReserveX28(t *testing.T) {
	src := filepath.Join(t.TempDir(), "pressure.c")
	if err := os.WriteFile(src, []byte(pressure), 0o644); err != nil {
		t.Fatal(err)
	}
	targets := []Target{{"linux", "arm64"}, {"darwin", "arm64"}, {"ios", "arm64"}, {"android", "arm64"}}
	for _, zig := range []bool{false, true} {
		for _, tg := range targets {
			name := tg.String()
			if zig {
				if tg.OS == "ios" || tg.OS == "android" {
					continue // Zig doesn't build these
				}
				name = "zig/" + name
			}
			t.Run(name, func(t *testing.T) {
				if zig {
					if _, err := exec.LookPath("zig"); err != nil {
						t.Skip("no Zig")
					}
					t.Setenv("ASUKA_CC", "zig")
				}
				tc, err := ForTarget(tg)
				if err != nil {
					t.Skip(err)
				}
				flags := AppFlags(tc.CC, tg)
				if len(flags) == 0 {
					t.Fatalf("no flags for %s: x28 isn't reserved", tg)
				}
				out := filepath.Join(t.TempDir(), "pressure.s")
				args := append(append(append(append([]string{}, tc.CC.Args...), tc.Flags...), flags...), "-O2", "-S", "-o", out, src)
				if b, err := exec.Command(tc.CC.Path, args...).CombinedOutput(); err != nil {
					t.Fatalf("%s %s: %v\n%s", tc.CC.Path, strings.Join(args, " "), err, b)
				}
				b, err := os.ReadFile(out)
				if err != nil {
					t.Fatal(err)
				}
				for _, line := range strings.Split(string(b), "\n") {
					ins := strings.TrimSpace(line)
					if i := strings.IndexAny(ins, ";/"); i >= 0 && (ins[i] == ';' || strings.HasPrefix(ins[i:], "//")) {
						ins = ins[:i] // a comment
					}
					if ins == "" || strings.HasPrefix(ins, ".") || strings.HasSuffix(ins, ":") {
						continue // a directive (.cfi_offset w28, the file's path), a label
					}
					if x28.MatchString(ins) {
						t.Fatalf("%s with %s uses x28: %q\n%s", tc.CC.Path, strings.Join(flags, " "), line, b)
					}
				}
			})
		}
	}
}
