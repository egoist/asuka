// Package s3test gives tests a bucket of an S3-compatible server: the
// one an environment variable names (any server: Amazon S3, R2, MinIO),
// else a RustFS container (Image) started with Docker for the test and
// removed after it, else the test is skipped.
package s3test

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"

	"github.com/egoist/asuka/internal/sigv4"
)

// Image is the RustFS the tests start, pinned (docker.io/rustfs/rustfs).
const Image = "rustfs/rustfs:1.0.6@sha256:ee717072290bec9b11abfc0365f8310b00df1cd9b3639943edae47bdef05044a"

// Server is a bucket on an S3-compatible server.
type Server struct {
	// Endpoint is the server's URL, e.g. http://127.0.0.1:9000.
	Endpoint string
	Bucket   string
	// Key may do anything with the bucket.
	Key sigv4.Key
	// NoList, when set, may get, put and delete the bucket's objects but
	// not list them (no s3:ListBucket).
	NoList *sigv4.Key
	// container is the RustFS container's name, when the test started one.
	container string
}

// Start returns the bucket that env names (http://127.0.0.1:9000/pkgs:
// the server's URL and the bucket's name, the credentials in
// AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY, and env_NOLIST_KEY and
// env_NOLIST_SECRET's without s3:ListBucket), else starts RustFS in
// Docker, its bucket's objects readable by anyone (as a CDN serves
// them), with credentials without s3:ListBucket too; else skips t.
func Start(t testing.TB, env string) *Server {
	t.Helper()
	if s := os.Getenv(env); s != "" {
		i := strings.LastIndex(s, "/")
		if i < 0 || !strings.HasPrefix(s, "http") {
			t.Fatalf("%s=%q: want the server's URL and the bucket, http://127.0.0.1:9000/pkgs", env, s)
		}
		srv := &Server{Endpoint: s[:i], Bucket: s[i+1:], Key: sigv4.Key{
			AccessKey: os.Getenv("AWS_ACCESS_KEY_ID"), SecretKey: os.Getenv("AWS_SECRET_ACCESS_KEY"), SessionToken: os.Getenv("AWS_SESSION_TOKEN"),
		}}
		if k := os.Getenv(env + "_NOLIST_KEY"); k != "" {
			srv.NoList = &sigv4.Key{AccessKey: k, SecretKey: os.Getenv(env + "_NOLIST_SECRET")}
		}
		return srv
	}
	if !Docker() {
		t.Skipf("no S3 server: set %s to one's URL and bucket (http://127.0.0.1:9000/pkgs; AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY), or run Docker, for RustFS (%s)", env, Image)
	}
	return startRustFS(t)
}

// Docker reports whether Docker runs here.
func Docker() bool {
	if _, err := exec.LookPath("docker"); err != nil {
		return false
	}
	return exec.Command("docker", "info", "--format", "{{.ServerVersion}}").Run() == nil
}

func startRustFS(t testing.TB) *Server {
	t.Helper()
	// The port RustFS listens on is the one it's published on, so that a
	// container sharing its network (Docker) reaches it at the same URL.
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := l.Addr().(*net.TCPAddr).Port
	l.Close()
	srv := &Server{
		Endpoint:  fmt.Sprintf("http://127.0.0.1:%d", port),
		Bucket:    "pkgs",
		Key:       sigv4.Key{AccessKey: "asuka-" + random(4), SecretKey: random(16)},
		NoList:    &sigv4.Key{AccessKey: "nolist-" + random(4), SecretKey: random(16)},
		container: "asuka-s3-" + random(4),
	}
	out, err := exec.Command("docker", "run", "-d", "--rm", "--name", srv.container,
		"-p", fmt.Sprintf("127.0.0.1:%d:%d", port, port),
		"-e", fmt.Sprintf("RUSTFS_ADDRESS=:%d", port),
		"-e", "RUSTFS_ACCESS_KEY="+srv.Key.AccessKey, "-e", "RUSTFS_SECRET_KEY="+srv.Key.SecretKey,
		"-e", "RUSTFS_CONSOLE_ENABLE=false",
		Image).CombinedOutput()
	if err != nil {
		t.Fatalf("docker run %s: %v\n%s", Image, err, out)
	}
	t.Cleanup(func() { exec.Command("docker", "rm", "-f", srv.container).Run() })

	// Ready when it makes the bucket.
	var last error
	for deadline := time.Now().Add(2 * time.Minute); ; time.Sleep(200 * time.Millisecond) {
		if last = srv.Do(http.MethodPut, "", nil); last == nil || strings.Contains(last.Error(), "BucketAlreadyOwnedByYou") {
			break
		}
		if time.Now().After(deadline) {
			logs, _ := exec.Command("docker", "logs", srv.container).CombinedOutput()
			t.Fatalf("RustFS didn't make the bucket: %v\n%s", last, logs)
		}
	}
	policy := func(name string, actions ...string) string {
		return `{"Version":"2012-10-17","Statement":[{"Effect":"Allow",` + name + `"Action":["` + strings.Join(actions, `","`) + `"],"Resource":["arn:aws:s3:::` + srv.Bucket + `/*"]}]}`
	}
	// Anyone may read its objects, not list them.
	if err := srv.Do(http.MethodPut, "?policy", []byte(policy(`"Principal":{"AWS":["*"]},`, "s3:GetObject"))); err != nil {
		t.Fatalf("the bucket's policy: %v", err)
	}
	// A user without s3:ListBucket (RustFS's admin API, through its CLI).
	c := exec.Command("docker", "exec", "-i", srv.container, "sh", "-c", fmt.Sprintf(`set -e
cat > /tmp/nolist.json
rc alias set local http://127.0.0.1:%d %s %s
rc admin user add local %s %s
rc admin policy create local nolist /tmp/nolist.json
rc admin policy attach local nolist --user %s`, port, srv.Key.AccessKey, srv.Key.SecretKey, srv.NoList.AccessKey, srv.NoList.SecretKey, srv.NoList.AccessKey))
	c.Stdin = strings.NewReader(policy("", "s3:GetObject", "s3:PutObject", "s3:DeleteObject"))
	if out, err := c.CombinedOutput(); err != nil {
		t.Fatalf("a user without s3:ListBucket: %v\n%s", err, out)
	}
	return srv
}

// Env is the environment of a command that uploads to the bucket with
// key: the environment's, AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY and
// AWS_SESSION_TOKEN replaced.
func Env(key sigv4.Key) []string {
	var env []string
	for _, e := range os.Environ() {
		if !strings.HasPrefix(e, "AWS_ACCESS_KEY_ID=") && !strings.HasPrefix(e, "AWS_SECRET_ACCESS_KEY=") && !strings.HasPrefix(e, "AWS_SESSION_TOKEN=") {
			env = append(env, e)
		}
	}
	env = append(env, "AWS_ACCESS_KEY_ID="+key.AccessKey, "AWS_SECRET_ACCESS_KEY="+key.SecretKey)
	if key.SessionToken != "" {
		env = append(env, "AWS_SESSION_TOKEN="+key.SessionToken)
	}
	return env
}

// DockerNetwork is what docker run takes for a container to reach
// Endpoint: RustFS's network, or the host's for another server; ok is
// false without Docker.
func (s *Server) DockerNetwork() (args []string, ok bool) {
	switch {
	case s.container != "":
		return []string{"--network", "container:" + s.container}, true
	case Docker():
		return []string{"--network", "host"}, true
	}
	return nil, false
}

// Do sends a request for the bucket (key "", or "?policy") or one of its
// objects, signed with Key, with body: an error, with the status and the
// server's message, for a status other than 2xx.
func (s *Server) Do(method, key string, body []byte) error {
	u := s.Endpoint + "/" + sigv4.EscapePath(s.Bucket)
	if key != "" && key[0] != '?' {
		u += "/" + sigv4.EscapePath(key)
	} else {
		u += key
	}
	req, err := http.NewRequest(method, u, bytes.NewReader(body))
	if err != nil {
		return err
	}
	sum := sha256.Sum256(body)
	sigv4.Sign(req, s.Key, "us-east-1", "s3", hex.EncodeToString(sum[:]), time.Now())
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	if resp.StatusCode/100 != 2 {
		return fmt.Errorf("%s %s: %s\n%s", method, u, resp.Status, b)
	}
	return nil
}

func random(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	return hex.EncodeToString(b)
}
