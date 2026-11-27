package main

import (
	"bytes"
	"cmp"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"maps"
	"net/http"
	"net/url"
	"os"
	"slices"
	"strings"
	"time"
)

// Uploads to Amazon S3 and compatible services, such as Cloudflare R2 and
// MinIO, in pure Go: objects are put with a single request signed with
// AWS Signature Version 4; a repository's indexes with conditional writes
// (If-Match, If-None-Match), so that publishes at once don't undo each
// other's.

// S3 is a bucket of Amazon S3, or of a compatible service such as
// Cloudflare R2. The credentials come from AWS_ACCESS_KEY_ID,
// AWS_SECRET_ACCESS_KEY and AWS_SESSION_TOKEN.
type s3Config struct {
	Bucket string `json:"bucket"`
	// Prefix is the directory of the files in the bucket, e.g. "my-app"
	// (default: its root).
	Prefix string `json:"prefix"`
	// Region is the region of the bucket (default: AWS_REGION, else
	// AWS_DEFAULT_REGION, else us-east-1).
	Region string `json:"region"`
	// Endpoint is the URL of a compatible service, e.g.
	// "https://<account>.r2.cloudflarestorage.com" (default:
	// AWS_ENDPOINT_URL_S3, else AWS_ENDPOINT_URL, else Amazon S3).
	Endpoint string `json:"endpoint"`
	// PathStyle names the bucket in the path of URLs rather than in the
	// host name (default: with Endpoint, or a bucket name with dots).
	PathStyle *bool `json:"pathStyle"`
}

func (s *s3Config) validate(name string) error {
	if s.Bucket == "" {
		return errors.New(name + " needs a bucket")
	}
	if s.Endpoint != "" {
		u, err := url.Parse(s.Endpoint)
		switch {
		case err != nil || (u.Scheme != "https" && u.Scheme != "http") || u.Host == "":
			return fmt.Errorf("%s.endpoint %q is not an http(s) URL", name, s.Endpoint)
		case strings.Trim(u.Path, "/") != "" || u.RawQuery != "":
			return fmt.Errorf("%s.endpoint %q has a path: the endpoint is the service's, and bucket names the bucket", name, s.Endpoint)
		}
	}
	return nil
}

// s3Client puts objects into a bucket.
type s3Client struct {
	bucket, prefix, region string
	endpoint               *url.URL
	pathStyle              bool
	accessKey, secretKey   string
	sessionToken           string
}

func newS3Client(s *s3Config) (*s3Client, error) {
	endpoint := cmp.Or(s.Endpoint, os.Getenv("AWS_ENDPOINT_URL_S3"), os.Getenv("AWS_ENDPOINT_URL"))
	if endpoint != s.Endpoint {
		if err := (&s3Config{Bucket: s.Bucket, Endpoint: endpoint}).validate("AWS_ENDPOINT_URL"); err != nil {
			return nil, err
		}
	}
	c := &s3Client{
		bucket:       s.Bucket,
		prefix:       strings.Trim(s.Prefix, "/"),
		region:       cmp.Or(s.Region, os.Getenv("AWS_REGION"), os.Getenv("AWS_DEFAULT_REGION"), "us-east-1"),
		pathStyle:    endpoint != "" || strings.Contains(s.Bucket, "."),
		accessKey:    os.Getenv("AWS_ACCESS_KEY_ID"),
		secretKey:    os.Getenv("AWS_SECRET_ACCESS_KEY"),
		sessionToken: os.Getenv("AWS_SESSION_TOKEN"),
	}
	if c.accessKey == "" || c.secretKey == "" {
		return nil, errors.New("uploading to S3 needs the credentials in AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY")
	}
	if s.PathStyle != nil {
		c.pathStyle = *s.PathStyle
	}
	var err error
	c.endpoint, err = url.Parse(cmp.Or(endpoint, "https://s3."+c.region+".amazonaws.com"))
	return c, err
}

// key returns the key of the file named name.
func (c *s3Client) key(name string) string {
	if c.prefix == "" {
		return name
	}
	return c.prefix + "/" + name
}

// objectURL returns the URL of the object of key.
func (c *s3Client) objectURL(key string) string {
	host, path := c.endpoint.Host, "/"+s3Escape(key)
	if c.pathStyle {
		path = "/" + s3Escape(c.bucket) + path
	} else {
		host = c.bucket + "." + host
	}
	return c.endpoint.Scheme + "://" + host + path
}

// put uploads the file at path to key, with the headers of header.
func (c *s3Client) put(key, path string, header http.Header) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	sum := sha256.New()
	size, err := io.Copy(sum, f)
	if err != nil {
		return err
	}
	_, _, err = c.do(http.MethodPut, key, nil, io.NewSectionReader(f, 0, size), size, hex.EncodeToString(sum.Sum(nil)), header)
	return err
}

// putData uploads data to key, with the headers of header.
func (c *s3Client) putData(key string, data []byte, header http.Header) error {
	sum := sha256.Sum256(data)
	_, _, err := c.do(http.MethodPut, key, nil, bytes.NewReader(data), int64(len(data)), hex.EncodeToString(sum[:]), header)
	return err
}

// putDataIf uploads data to key if the object is still the one of the
// ETag etag, or, for "", if there's none: errConflict when another
// request changed it since (S3, R2 and MinIO's conditional writes).
func (c *s3Client) putDataIf(key string, data []byte, etag string, header http.Header) error {
	h := header.Clone()
	if h == nil {
		h = http.Header{}
	}
	switch etag {
	case untagged: // a service that names no versions: as put
	case "":
		h.Set("If-None-Match", "*")
	default:
		h.Set("If-Match", etag)
	}
	err := c.putData(key, data, h)
	if etag != "" && etag != untagged && errors.Is(err, errNoObject) {
		return fmt.Errorf("%w: %v", errConflict, err) // deleted since
	}
	return err
}

// errNoObject is get's when the bucket has no object of the key.
var errNoObject = errors.New("no such object")

// errConflict is a conditional write's when the object changed since it
// was read: another publish wrote it.
var errConflict = errors.New("changed since it was read")

// get downloads the object of key: errNoObject when there's none.
func (c *s3Client) get(key string) ([]byte, error) {
	b, _, err := c.getTagged(key)
	return b, err
}

// getTagged downloads the object of key, and returns its ETag with it
// (untagged when the service names none).
func (c *s3Client) getTagged(key string) ([]byte, string, error) {
	b, h, err := c.do(http.MethodGet, key, nil, nil, 0, emptySHA256, nil)
	if err != nil {
		return nil, "", err
	}
	return b, cmp.Or(h.Get("ETag"), untagged), nil
}

// untagged is the tag of an object whose ETag a service doesn't say,
// which putDataIf puts unconditionally.
const untagged = "-"

// remove deletes the object of key (none is no error).
func (c *s3Client) remove(key string) error {
	_, _, err := c.do(http.MethodDelete, key, nil, nil, 0, emptySHA256, nil)
	if errors.Is(err, errNoObject) {
		return nil
	}
	return err
}

// An s3Object is an object a listing names.
type s3Object struct {
	Key          string
	LastModified time.Time
}

// list lists the objects whose keys start with prefix (ListObjectsV2,
// page after page), which needs the s3:ListBucket permission.
func (c *s3Client) list(prefix string) ([]s3Object, error) {
	var out []s3Object
	token := ""
	for {
		q := url.Values{"list-type": {"2"}, "prefix": {prefix}}
		if token != "" {
			q.Set("continuation-token", token)
		}
		b, _, err := c.do(http.MethodGet, "", q, nil, 0, emptySHA256, nil)
		if err != nil {
			return nil, err
		}
		var page struct {
			Contents []struct {
				Key          string
				LastModified time.Time
			}
			IsTruncated           bool
			NextContinuationToken string
		}
		if err := xml.Unmarshal(b, &page); err != nil {
			return nil, fmt.Errorf("listing s3://%s/%s: %w", c.bucket, prefix, err)
		}
		for _, o := range page.Contents {
			out = append(out, s3Object{o.Key, o.LastModified})
		}
		if !page.IsTruncated || page.NextContinuationToken == "" {
			return out, nil
		}
		token = page.NextContinuationToken
	}
}

// emptySHA256 is an empty payload's digest.
const emptySHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

// do sends a request for key's object (the bucket's, with query, for
// key ""), signed, with the size bytes of body (of the SHA-256
// payloadHash), and returns a GET's body and the response's headers.
func (c *s3Client) do(method, key string, query url.Values, body io.Reader, size int64, payloadHash string, header http.Header) ([]byte, http.Header, error) {
	if body == nil || size == 0 {
		body = http.NoBody // an empty body of known length
	}
	u := c.objectURL(key)
	if len(query) > 0 {
		u += "?" + s3Query(query)
	}
	req, err := http.NewRequest(method, u, body)
	if err != nil {
		return nil, nil, err
	}
	req.ContentLength = size
	maps.Copy(req.Header, header)
	c.sign(req, payloadHash, time.Now())
	req.Header.Set("User-Agent", "asuka")
	// S3 redirects requests for another region, which must not be followed
	// as a GET.
	client := &http.Client{Timeout: time.Hour, CheckRedirect: func(*http.Request, []*http.Request) error {
		return http.ErrUseLastResponse
	}}
	resp, err := client.Do(req)
	if err != nil {
		return nil, nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode/100 == 2 {
		if method != http.MethodGet {
			return nil, resp.Header, nil
		}
		b, err := io.ReadAll(io.LimitReader(resp.Body, 1<<30))
		return b, resp.Header, err
	}
	what := map[string]string{http.MethodGet: "downloading", http.MethodDelete: "deleting"}[method]
	if what == "" {
		what = "uploading to"
	}
	listing := key == ""
	if listing {
		what, key = "listing", query.Get("prefix")
	}
	// S3 describes errors in XML.
	var e struct{ Code, Message string }
	data, _ := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	xml.Unmarshal(data, &e)
	switch {
	case resp.StatusCode == http.StatusNotFound || e.Code == "NoSuchKey":
		return nil, nil, fmt.Errorf("%s s3://%s/%s: %w", what, c.bucket, key, errNoObject)
	case resp.StatusCode == http.StatusPreconditionFailed || e.Code == "ConditionalRequestConflict":
		return nil, nil, fmt.Errorf("%s s3://%s/%s: %w", what, c.bucket, key, errConflict)
	}
	msg := resp.Status
	if e.Code != "" {
		msg += ": " + e.Code + ": " + e.Message
	}
	if region := resp.Header.Get("X-Amz-Bucket-Region"); region != "" && region != c.region {
		msg += " (the bucket is in " + region + ": set its s3.region)"
	}
	switch {
	case resp.StatusCode == http.StatusForbidden && listing:
		msg += " (listing needs the s3:ListBucket permission)"
	case resp.StatusCode == http.StatusForbidden && method == http.MethodGet:
		msg += " (S3 answers 403 for an object that isn't there without the s3:ListBucket permission)"
	}
	return nil, nil, fmt.Errorf("%s s3://%s/%s: %s", what, c.bucket, key, msg)
}

// sign signs req with AWS Signature Version 4 for S3, its query and
// headers with the host, and a payload of the SHA-256 payloadHash.
func (c *s3Client) sign(req *http.Request, payloadHash string, t time.Time) {
	t = t.UTC()
	date, amzDate := t.Format("20060102"), t.Format("20060102T150405Z")
	req.Header.Set("X-Amz-Date", amzDate)
	req.Header.Set("X-Amz-Content-Sha256", payloadHash)
	if c.sessionToken != "" {
		req.Header.Set("X-Amz-Security-Token", c.sessionToken)
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
	request := req.Method + "\n" + req.URL.EscapedPath() + "\n" + s3Query(req.URL.Query()) + "\n" + canonical.String() + "\n" + signed + "\n" + payloadHash
	scope := date + "/" + c.region + "/s3/aws4_request"
	hash := sha256.Sum256([]byte(request))
	key := hmacSHA256([]byte("AWS4"+c.secretKey), date)
	for _, s := range []string{c.region, "s3", "aws4_request"} {
		key = hmacSHA256(key, s)
	}
	signature := hmacSHA256(key, "AWS4-HMAC-SHA256\n"+amzDate+"\n"+scope+"\n"+hex.EncodeToString(hash[:]))
	req.Header.Set("Authorization", "AWS4-HMAC-SHA256 Credential="+c.accessKey+"/"+scope+
		", SignedHeaders="+signed+", Signature="+hex.EncodeToString(signature))
}

func hmacSHA256(key []byte, data string) []byte {
	h := hmac.New(sha256.New, key)
	h.Write([]byte(data))
	return h.Sum(nil)
}

// s3Query is a query as Signature Version 4 has it canonical: its names
// sorted, names and values escaped as s3Escape does, "/" too.
func s3Query(q url.Values) string {
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

// s3Escape escapes a key for the path of a URL as Signature Version 4
// wants it: every byte but letters, digits, "-._~" and "/".
func s3Escape(s string) string { return escape(s, "-._~/") }

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
