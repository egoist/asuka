// Package gpgsign signs Linux packages and their repositories with an
// OpenPGP key, as gpg does for rpm --addsign, apt's Release files and
// dnf's repomd.xml: detached signatures (binary or armored) and
// clearsigned text. Keys are made and read here too (asuka linux keygen),
// in Go (github.com/ProtonMail/go-crypto): no gpg needed.
package gpgsign

import (
	"bytes"
	"crypto"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/ProtonMail/go-crypto/openpgp"
	"github.com/ProtonMail/go-crypto/openpgp/armor"
	"github.com/ProtonMail/go-crypto/openpgp/clearsign"
	"github.com/ProtonMail/go-crypto/openpgp/packet"
)

// A Key is an OpenPGP key that signs: its primary key, a secret one,
// decrypted.
type Key struct {
	e *openpgp.Entity
}

// config is how signatures are made: SHA-256 (rpm, apt and dnf refuse
// SHA-1), and no salt notation (what gpg writes, which every rpm reads).
func config(now time.Time) *packet.Config {
	no := false
	return &packet.Config{
		DefaultHash:                           crypto.SHA256,
		Time:                                  func() time.Time { return now },
		NonDeterministicSignaturesViaNotation: &no,
	}
}

// Generate makes a key for packages: RSA 4096 (what every rpm, apt and
// dnf verify), signing with its primary key, for the user ID "name
// <email>", made at now.
func Generate(name, email string, now time.Time) (*Key, error) {
	c := config(now)
	c.Algorithm = packet.PubKeyAlgoRSA
	c.RSABits = 4096
	e, err := openpgp.NewEntity(name, "", email, c)
	if err != nil {
		return nil, err
	}
	return &Key{e}, nil
}

// Parse reads an armored (or binary) secret key, decrypting it with
// passphrase when it's encrypted.
func Parse(data, passphrase []byte) (*Key, error) {
	var list openpgp.EntityList
	var err error
	if bytes.Contains(data, []byte("-----BEGIN PGP")) {
		list, err = openpgp.ReadArmoredKeyRing(bytes.NewReader(data))
	} else {
		list, err = openpgp.ReadKeyRing(bytes.NewReader(data))
	}
	if err != nil {
		return nil, fmt.Errorf("reading the OpenPGP key: %w", err)
	}
	if len(list) == 0 {
		return nil, errors.New("reading the OpenPGP key: there's none")
	}
	e := list[0]
	if e.PrivateKey == nil {
		return nil, errors.New("the OpenPGP key is a public key: packages are signed with the secret key")
	}
	if e.PrivateKey.Encrypted {
		if len(passphrase) == 0 {
			return nil, errors.New("the OpenPGP key is encrypted: its passphrase is needed")
		}
		if err := e.DecryptPrivateKeys(passphrase); err != nil {
			return nil, fmt.Errorf("decrypting the OpenPGP key: %w", err)
		}
	}
	if !e.PrimaryKey.CanSign() {
		return nil, errors.New("the OpenPGP key can't sign")
	}
	return &Key{e}, nil
}

// Encrypt protects the secret key with passphrase, as it's written.
func (k *Key) Encrypt(passphrase []byte) error {
	return k.e.EncryptPrivateKeys(passphrase, nil)
}

// ArmoredPrivate is the secret key, armored.
func (k *Key) ArmoredPrivate() ([]byte, error) {
	var b bytes.Buffer
	w, err := armor.Encode(&b, openpgp.PrivateKeyType, nil)
	if err != nil {
		return nil, err
	}
	if err := k.e.SerializePrivateWithoutSigning(w, nil); err != nil {
		return nil, err
	}
	if err := w.Close(); err != nil {
		return nil, err
	}
	b.WriteByte('\n')
	return b.Bytes(), nil
}

// Public is the public key, binary (what /etc/apt/keyrings holds).
func (k *Key) Public() ([]byte, error) {
	var b bytes.Buffer
	if err := k.e.Serialize(&b); err != nil {
		return nil, err
	}
	return b.Bytes(), nil
}

// ArmoredPublic is the public key, armored (what rpm --import and dnf's
// gpgkey take).
func (k *Key) ArmoredPublic() ([]byte, error) {
	var b bytes.Buffer
	w, err := armor.Encode(&b, openpgp.PublicKeyType, nil)
	if err != nil {
		return nil, err
	}
	if err := k.e.Serialize(w); err != nil {
		return nil, err
	}
	if err := w.Close(); err != nil {
		return nil, err
	}
	b.WriteByte('\n')
	return b.Bytes(), nil
}

// ParsePublic is the fingerprint of a public key (armored or binary).
func ParsePublic(data []byte) (string, error) {
	var list openpgp.EntityList
	var err error
	if bytes.Contains(data, []byte("-----BEGIN PGP")) {
		list, err = openpgp.ReadArmoredKeyRing(bytes.NewReader(data))
	} else {
		list, err = openpgp.ReadKeyRing(bytes.NewReader(data))
	}
	if err != nil {
		return "", err
	}
	if len(list) == 0 {
		return "", errors.New("no key")
	}
	return (&Key{list[0]}).Fingerprint(), nil
}

// Fingerprint is the key's, in upper case hex.
func (k *Key) Fingerprint() string {
	return strings.ToUpper(fmt.Sprintf("%x", k.e.PrimaryKey.Fingerprint))
}

// KeyID is the key's long ID, in lower case hex (rpm names its imported
// key gpg-pubkey-<last 8 of it>).
func (k *Key) KeyID() string { return fmt.Sprintf("%016x", k.e.PrimaryKey.KeyId) }

// UserID is the key's primary user ID ("Name <email>").
func (k *Key) UserID() string {
	if id := k.e.PrimaryIdentity(); id != nil {
		return id.Name
	}
	return ""
}

// DSA reports whether the key is a DSA key: an RPM keeps its signature
// in a tag of its own.
func (k *Key) DSA() bool { return k.e.PrimaryKey.PubKeyAlgo == packet.PubKeyAlgoDSA }

// Sign is a detached binary signature of data (a signature packet: what
// an RPM's header signature holds).
func (k *Key) Sign(data []byte) ([]byte, error) {
	var b bytes.Buffer
	if err := openpgp.DetachSign(&b, k.e, bytes.NewReader(data), config(time.Now())); err != nil {
		return nil, err
	}
	return b.Bytes(), nil
}

// SignArmored is a detached armored signature of data (Release.gpg,
// repomd.xml.asc).
func (k *Key) SignArmored(data []byte) ([]byte, error) {
	var b bytes.Buffer
	if err := openpgp.ArmoredDetachSign(&b, k.e, bytes.NewReader(data), config(time.Now())); err != nil {
		return nil, err
	}
	b.WriteByte('\n')
	return b.Bytes(), nil
}

// SignArmoredText is a detached armored signature of text, in text mode
// (a canonical text document's: what appimagetool --sign makes of an
// AppImage's digest).
func (k *Key) SignArmoredText(text []byte) ([]byte, error) {
	var b bytes.Buffer
	if err := openpgp.ArmoredDetachSignText(&b, k.e, bytes.NewReader(text), config(time.Now())); err != nil {
		return nil, err
	}
	b.WriteByte('\n')
	return b.Bytes(), nil
}

// ClearSign is text clearsigned (InRelease).
func (k *Key) ClearSign(text []byte) ([]byte, error) {
	var b bytes.Buffer
	w, err := clearsign.Encode(&b, k.e.PrivateKey, config(time.Now()))
	if err != nil {
		return nil, err
	}
	if _, err := w.Write(text); err != nil {
		return nil, err
	}
	if err := w.Close(); err != nil {
		return nil, err
	}
	b.WriteByte('\n')
	return b.Bytes(), nil
}

// Verify checks a detached signature (binary or armored) of data with the
// public key pub (binary or armored): for tests and asuka's own checks.
func Verify(pub, data, sig []byte) error {
	var ring openpgp.EntityList
	var err error
	if bytes.Contains(pub, []byte("-----BEGIN PGP")) {
		ring, err = openpgp.ReadArmoredKeyRing(bytes.NewReader(pub))
	} else {
		ring, err = openpgp.ReadKeyRing(bytes.NewReader(pub))
	}
	if err != nil {
		return err
	}
	if bytes.HasPrefix(bytes.TrimSpace(sig), []byte("-----BEGIN PGP")) {
		_, err = openpgp.CheckArmoredDetachedSignature(ring, bytes.NewReader(data), bytes.NewReader(sig), nil)
	} else {
		_, err = openpgp.CheckDetachedSignature(ring, bytes.NewReader(data), bytes.NewReader(sig), nil)
	}
	return err
}

// VerifyClearSigned checks clearsigned text with pub, and returns the
// text signed.
func VerifyClearSigned(pub, signed []byte) ([]byte, error) {
	ring, err := openpgp.ReadArmoredKeyRing(bytes.NewReader(pub))
	if err != nil {
		if ring, err = openpgp.ReadKeyRing(bytes.NewReader(pub)); err != nil {
			return nil, err
		}
	}
	b, _ := clearsign.Decode(signed)
	if b == nil {
		return nil, errors.New("not clearsigned")
	}
	if _, err := b.VerifySignature(ring, nil); err != nil {
		return nil, err
	}
	return b.Plaintext, nil
}
