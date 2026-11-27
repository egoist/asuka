// Package sigv4 signs requests to Amazon S3 and compatible services
// (Cloudflare R2, MinIO, RustFS) with AWS Signature Version 4, in Go:
// asuka publish's uploads, and the tests' S3 server (internal/s3test).
package sigv4

import (
	"cmp"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"maps"
	"net/http"
	"net/url"
	"slices"
	"strings"
	"time"
)

// Key is who signs: an access key, its secret, and a temporary
// credential's session token.
type Key struct {
	AccessKey, SecretKey, SessionToken string
}

// EmptySHA256 is an empty payload's digest.
const EmptySHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

// Sign signs req at t for service ("s3") in region, its query and headers
// with the host, and a payload of the SHA-256 payloadHash (hex).
func Sign(req *http.Request, k Key, region, service, payloadHash string, t time.Time) {
	t = t.UTC()
	date, amzDate := t.Format("20060102"), t.Format("20060102T150405Z")
	req.Header.Set("X-Amz-Date", amzDate)
	req.Header.Set("X-Amz-Content-Sha256", payloadHash)
	if k.SessionToken != "" {
		req.Header.Set("X-Amz-Security-Token", k.SessionToken)
	}
	headers := map[string]string{"host": cmp.Or(req.Host, req.URL.Host)}
	for name, values := range req.Header {
		trimmed := make([]string, len(values))
		for i, v := range values {
			trimmed[i] = strings.Join(strings.Fields(v), " ")
		}
		headers[strings.ToLower(name)] = strings.Join(trimmed, ",")
	}
	names := slices.Sorted(maps.Keys(headers))
	var canonical strings.Builder
	for _, name := range names {
		canonical.WriteString(name + ":" + headers[name] + "\n")
	}
	signed := strings.Join(names, ";")
	request := req.Method + "\n" + req.URL.EscapedPath() + "\n" + Query(req.URL.Query()) + "\n" + canonical.String() + "\n" + signed + "\n" + payloadHash
	scope := date + "/" + region + "/" + service + "/aws4_request"
	hash := sha256.Sum256([]byte(request))
	key := hmacSHA256([]byte("AWS4"+k.SecretKey), date)
	for _, s := range []string{region, service, "aws4_request"} {
		key = hmacSHA256(key, s)
	}
	signature := hmacSHA256(key, "AWS4-HMAC-SHA256\n"+amzDate+"\n"+scope+"\n"+hex.EncodeToString(hash[:]))
	req.Header.Set("Authorization", "AWS4-HMAC-SHA256 Credential="+k.AccessKey+"/"+scope+
		", SignedHeaders="+signed+", Signature="+hex.EncodeToString(signature))
}

func hmacSHA256(key []byte, data string) []byte {
	h := hmac.New(sha256.New, key)
	h.Write([]byte(data))
	return h.Sum(nil)
}

// Query is a query as Signature Version 4 has it canonical: its names
// sorted, names and values escaped as EscapePath does, "/" too.
func Query(q url.Values) string {
	var parts []string
	for _, k := range slices.Sorted(maps.Keys(q)) {
		vs := slices.Clone(q[k])
		slices.Sort(vs)
		for _, v := range vs {
			parts = append(parts, escape(k, "-._~")+"="+escape(v, "-._~"))
		}
	}
	return strings.Join(parts, "&")
}

// EscapePath escapes a key for the path of a URL as Signature Version 4
// wants it: every byte but letters, digits, "-._~" and "/".
func EscapePath(s string) string { return escape(s, "-._~/") }

// escape escapes every byte of s but letters, digits and keep's.
func escape(s, keep string) string {
	var b strings.Builder
	for i := range len(s) {
		if ch := s[i]; 'A' <= ch && ch <= 'Z' || 'a' <= ch && ch <= 'z' || '0' <= ch && ch <= '9' || strings.IndexByte(keep, ch) >= 0 {
			b.WriteByte(ch)
		} else {
			fmt.Fprintf(&b, "%%%02X", ch)
		}
	}
	return b.String()
}
