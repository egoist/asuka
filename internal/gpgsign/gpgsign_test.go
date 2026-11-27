package gpgsign

import (
	"bytes"
	"crypto"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/ProtonMail/go-crypto/openpgp/packet"
)

var (
	testKeyOnce sync.Once
	testKey     *Key
)

// key is a key made once for the package's tests (RSA 4096 takes a
// moment).
func key(t *testing.T) *Key {
	t.Helper()
	testKeyOnce.Do(func() {
		k, err := Generate("Notes", "dev@example.com", time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC))
		if err != nil {
			t.Fatal(err)
		}
		testKey = k
	})
	if testKey == nil {
		t.Fatal("no key")
	}
	return testKey
}

func TestSignAndVerify(t *testing.T) {
	k := key(t)
	pub, err := k.ArmoredPublic()
	if err != nil {
		t.Fatal(err)
	}
	data := []byte("Origin: Notes\nSuite: stable\n")
	sig, err := k.Sign(data)
	if err != nil {
		t.Fatal(err)
	}
	if err := Verify(pub, data, sig); err != nil {
		t.Errorf("binary: %v", err)
	}
	if err := Verify(pub, append(data, 'x'), sig); err == nil {
		t.Error("a changed file verified")
	}
	// The signature packet: v4, RSA, SHA-256, a binary document's, and no
	// notation (what gpg writes).
	p, err := packet.Read(bytes.NewReader(sig))
	if err != nil {
		t.Fatal(err)
	}
	s := p.(*packet.Signature)
	if s.Version != 4 || s.PubKeyAlgo != packet.PubKeyAlgoRSA || s.Hash != crypto.SHA256 || s.SigType != packet.SigTypeBinary || len(s.Notations) != 0 {
		t.Errorf("the signature: v%d %v %v type %v, %d notations", s.Version, s.PubKeyAlgo, s.Hash, s.SigType, len(s.Notations))
	}
	armored, err := k.SignArmored(data)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.HasPrefix(armored, []byte("-----BEGIN PGP SIGNATURE-----")) {
		t.Errorf("armored: %s", armored)
	}
	if err := Verify(pub, data, armored); err != nil {
		t.Errorf("armored: %v", err)
	}
	clear, err := k.ClearSign(data)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.HasPrefix(clear, []byte("-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA256\n")) {
		t.Errorf("clearsigned: %s", clear)
	}
	text, err := VerifyClearSigned(pub, clear)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(text, bytes.TrimSuffix(data, []byte("\n"))) && !bytes.Equal(text, data) {
		t.Errorf("clearsigned text %q", text)
	}
}

func TestParse(t *testing.T) {
	k := key(t)
	priv, err := k.ArmoredPrivate()
	if err != nil {
		t.Fatal(err)
	}
	got, err := Parse(priv, nil)
	if err != nil {
		t.Fatal(err)
	}
	if got.Fingerprint() != k.Fingerprint() || got.UserID() != "Notes <dev@example.com>" || len(got.KeyID()) != 16 {
		t.Errorf("parsed %s %q %s, want %s", got.Fingerprint(), got.UserID(), got.KeyID(), k.Fingerprint())
	}
	pub, _ := k.Public()
	if _, err := Parse(pub, nil); err == nil || !strings.Contains(err.Error(), "public key") {
		t.Errorf("a public key: %v", err)
	}
	// Encrypted with a passphrase: read with it alone.
	enc, err := Parse(priv, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := enc.Encrypt([]byte("secret")); err != nil {
		t.Fatal(err)
	}
	encrypted, err := enc.ArmoredPrivate()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := Parse(encrypted, nil); err == nil || !strings.Contains(err.Error(), "passphrase") {
		t.Errorf("without its passphrase: %v", err)
	}
	if _, err := Parse(encrypted, []byte("wrong")); err == nil {
		t.Error("a wrong passphrase decrypted it")
	}
	dec, err := Parse(encrypted, []byte("secret"))
	if err != nil {
		t.Fatal(err)
	}
	sig, err := dec.Sign([]byte("x"))
	if err != nil {
		t.Fatal(err)
	}
	if err := Verify(pub, []byte("x"), sig); err != nil {
		t.Error(err)
	}
}
