//go:build e2e

package e2e

import (
	"bytes"
	"crypto/ecdsa"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"testing"
)

// TestPasskeyWeb drives platform/passkey in headless Chrome with DevTools'
// virtual authenticator (a platform one, with user verification): the app
// registers a passkey with a relying party's options and signs in with it,
// the relying party (here, as a server would) checking each answer: the
// client data's type, challenge and origin, the authenticator data's
// relying party and flags, the passkey's public key (SPKI) and its
// signature. A second registration, its options excluding the first
// passkey, is the package's exists.
func TestPasskeyWeb(t *testing.T) {
	browser := chrome()
	if browser == "" {
		t.Skip("no Chrome (ASUKA_CHROME)")
	}
	served := webServer(t, "e2e/testdata/passkey")
	u, _ := url.Parse(served)
	// localhost: a relying party's id is a domain, not an address.
	pageURL := "http://localhost:" + u.Port() + "/"
	origin := strings.TrimSuffix(pageURL, "/")
	rp := newTestRP(t, "localhost", origin)
	c := launchCDP(t, browser)
	var created struct{ TargetID string }
	if err := c.call("", "Target.createTarget", map[string]any{"url": "about:blank"}, &created, ""); err != nil {
		t.Fatal(err)
	}
	page := c.attach(t, created.TargetID)
	for _, m := range []struct {
		method string
		params any
	}{
		{"WebAuthn.enable", map[string]any{}},
		{"WebAuthn.addVirtualAuthenticator", map[string]any{"options": map[string]any{
			"protocol": "ctap2", "transport": "internal", "hasResidentKey": true, "hasUserVerification": true,
			"isUserVerified": true, "automaticPresenceSimulation": true,
		}}},
		{"Page.enable", map[string]any{}},
		{"Page.navigate", map[string]any{"url": pageURL}},
	} {
		var res json.RawMessage
		if err := c.call(page, m.method, m.params, &res, ""); err != nil {
			t.Fatal(err)
		}
	}
	s := &webScreen{t: t, url: served}
	showsLine(t, s, "status: ready")
	s.command("type " + rp.URL)
	s.tap("Register")
	showsLine(t, s, `status: registered 200 {"ok":true}`)
	s.tap("Sign in")
	showsLine(t, s, `status: signed in 200 {"ok":true,"user":"ana"}`)
	s.tap("Register")
	showsLine(t, s, "status: passkey: there's a passkey for this account already")
}

// testRP is a relying party of one user, which checks what a client
// sends as a WebAuthn server does (ES256 passkeys, no attestation).
type testRP struct {
	*httptest.Server
	t             *testing.T
	rpID, origin  string
	mu            sync.Mutex
	challenge     []byte
	credID        []byte
	key           *ecdsa.PublicKey
	userID        []byte
	lastSignCount uint32
}

var b64u = base64.RawURLEncoding

func newTestRP(t *testing.T, rpID, origin string) *testRP {
	rp := &testRP{t: t, rpID: rpID, origin: origin, userID: []byte("user-ana")}
	rp.Server = httptest.NewServer(http.HandlerFunc(rp.serve))
	t.Cleanup(rp.Close)
	return rp
}

func (rp *testRP) serve(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.Header().Set("Access-Control-Allow-Headers", "*")
	if r.Method == http.MethodOptions {
		return
	}
	rp.mu.Lock()
	defer rp.mu.Unlock()
	body, _ := io.ReadAll(r.Body)
	var out any
	var err error
	switch r.URL.Path {
	case "/register/begin":
		out = rp.begin(true)
	case "/login/begin":
		out = rp.begin(false)
	case "/register/finish":
		err = rp.finishRegistration(body)
		out = map[string]any{"ok": true}
	case "/login/finish":
		err = rp.finishSignIn(body)
		out = map[string]any{"ok": true, "user": "ana"}
	default:
		http.NotFound(w, r)
		return
	}
	if err != nil {
		rp.t.Logf("%s: %v\n%s", r.URL.Path, err, body)
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	json.NewEncoder(w).Encode(out)
}

// begin is the options a server sends: registration's wrapped in
// publicKey (as go-webauthn's), excluding the passkey it has; sign-in's
// discoverable (no allowCredentials).
func (rp *testRP) begin(register bool) any {
	rp.challenge = make([]byte, 32)
	rand.Read(rp.challenge)
	if !register {
		return map[string]any{"challenge": b64u.EncodeToString(rp.challenge), "rpId": rp.rpID, "userVerification": "required", "timeout": 60000}
	}
	exclude := []any{}
	if rp.credID != nil {
		exclude = append(exclude, map[string]any{"type": "public-key", "id": b64u.EncodeToString(rp.credID)})
	}
	return map[string]any{"publicKey": map[string]any{
		"rp":                     map[string]any{"id": rp.rpID, "name": "Passkey Test"},
		"user":                   map[string]any{"id": b64u.EncodeToString(rp.userID), "name": "ana", "displayName": "Ana"},
		"challenge":              b64u.EncodeToString(rp.challenge),
		"pubKeyCredParams":       []any{map[string]any{"type": "public-key", "alg": -7}},
		"excludeCredentials":     exclude,
		"authenticatorSelection": map[string]any{"residentKey": "required", "userVerification": "required"},
		"attestation":            "none",
		"timeout":                60000,
	}}
}

type credentialJSON struct {
	ID       string `json:"id"`
	RawID    string `json:"rawId"`
	Type     string `json:"type"`
	Response struct {
		ClientDataJSON     string   `json:"clientDataJSON"`
		AttestationObject  string   `json:"attestationObject"`
		AuthenticatorData  string   `json:"authenticatorData"`
		PublicKey          string   `json:"publicKey"`
		PublicKeyAlgorithm int      `json:"publicKeyAlgorithm"`
		Transports         []string `json:"transports"`
		Signature          string   `json:"signature"`
		UserHandle         string   `json:"userHandle"`
	} `json:"response"`
	ClientExtensionResults map[string]any `json:"clientExtensionResults"`
}

// check decodes a credential's JSON and checks what both answers have:
// its id, the client data and the authenticator data.
func (rp *testRP) check(body []byte, typ string) (*credentialJSON, []byte, []byte, error) {
	var c credentialJSON
	if err := json.Unmarshal(body, &c); err != nil {
		return nil, nil, nil, err
	}
	if c.Type != "public-key" || c.ID == "" || c.ID != c.RawID || c.ClientExtensionResults == nil {
		return nil, nil, nil, fmt.Errorf("the credential's id, rawId, type, clientExtensionResults: %+v", c)
	}
	cd, err := b64u.DecodeString(c.Response.ClientDataJSON)
	if err != nil {
		return nil, nil, nil, err
	}
	var client struct{ Type, Challenge, Origin string }
	json.Unmarshal(cd, &client)
	if client.Type != typ || client.Challenge != b64u.EncodeToString(rp.challenge) || client.Origin != rp.origin {
		return nil, nil, nil, fmt.Errorf("client data %s", cd)
	}
	data, err := b64u.DecodeString(c.Response.AuthenticatorData)
	if err != nil || len(data) < 37 {
		return nil, nil, nil, fmt.Errorf("authenticator data %q: %v", c.Response.AuthenticatorData, err)
	}
	hash := sha256.Sum256([]byte(rp.rpID))
	if !bytes.Equal(data[:32], hash[:]) || data[32]&0x05 != 0x05 { // UP, UV
		return nil, nil, nil, fmt.Errorf("authenticator data's rp or flags: %x", data[:33])
	}
	return &c, cd, data, nil
}

func (rp *testRP) finishRegistration(body []byte) error {
	c, _, data, err := rp.check(body, "webauthn.create")
	if err != nil {
		return err
	}
	if c.Response.AttestationObject == "" || c.Response.PublicKeyAlgorithm != -7 || data[32]&0x40 == 0 {
		return fmt.Errorf("attestation object, algorithm %d, attested data %x", c.Response.PublicKeyAlgorithm, data[32])
	}
	spki, err := b64u.DecodeString(c.Response.PublicKey)
	if err != nil {
		return err
	}
	k, err := x509.ParsePKIXPublicKey(spki)
	if err != nil {
		return err
	}
	key, ok := k.(*ecdsa.PublicKey)
	if !ok {
		return errors.New("not an ECDSA key")
	}
	rp.credID, _ = b64u.DecodeString(c.RawID)
	rp.key = key
	return nil
}

func (rp *testRP) finishSignIn(body []byte) error {
	c, cd, data, err := rp.check(body, "webauthn.get")
	if err != nil {
		return err
	}
	if id, _ := b64u.DecodeString(c.RawID); rp.key == nil || !bytes.Equal(id, rp.credID) {
		return errors.New("not the passkey registered")
	}
	if h, _ := b64u.DecodeString(c.Response.UserHandle); !bytes.Equal(h, rp.userID) {
		return fmt.Errorf("user handle %q", c.Response.UserHandle)
	}
	sig, err := b64u.DecodeString(c.Response.Signature)
	if err != nil {
		return err
	}
	cdHash := sha256.Sum256(cd)
	signed := sha256.Sum256(append(append([]byte{}, data...), cdHash[:]...))
	if !ecdsa.VerifyASN1(rp.key, signed[:], sig) {
		return errors.New("the signature doesn't verify")
	}
	return nil
}
