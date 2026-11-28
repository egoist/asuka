package testserver

import (
	"crypto"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"time"
)

// PKI is a test authority's certificates, made fresh each run, for the
// clients' TLS settings (std/http's and std/websocket's tls): a root
// (CA), an intermediate it signs, and the leaves that signs, their
// chains the leaf then the intermediate.
type PKI struct {
	// CA signed the rest; OtherCA signed none of them.
	CA, OtherCA string
	// The server's, for localhost, 127.0.0.1, ::1 and the hosts
	// NewPKI was given.
	ServerCert, ServerKey string
	// A client's ("Asuka Test Client"), its key P-256's: PKCS#8, and
	// the same key as SEC 1's EC PRIVATE KEY.
	ClientCert, ClientKey, ClientKeySEC1 string
	// Another client's ("Asuka Test RSA Client"), its key RSA's, PKCS#1.
	RSAClientCert, RSAClientKey string

	server tls.Certificate
	pool   *x509.CertPool
}

// NewPKI makes the certificates, the server's for hosts (names or IP
// addresses) too: the address a VM reaches this computer at.
func NewPKI(hosts ...string) (*PKI, error) {
	p := &PKI{pool: x509.NewCertPool()}
	from, until := time.Now().Add(-time.Hour), time.Now().Add(24*time.Hour)
	root, rootKey, err := authority("Asuka Test HTTP Root CA", nil, nil, from, until)
	if err != nil {
		return nil, err
	}
	inter, interKey, err := authority("Asuka Test HTTP Intermediate CA", root, rootKey, from, until)
	if err != nil {
		return nil, err
	}
	other, _, err := authority("Asuka Other HTTP Root CA", nil, nil, from, until)
	if err != nil {
		return nil, err
	}
	p.pool.AddCert(root)
	p.CA, p.OtherCA = pemOf("CERTIFICATE", root.Raw), pemOf("CERTIFICATE", other.Raw)
	chain := func(leaf *x509.Certificate) string {
		return pemOf("CERTIFICATE", leaf.Raw) + pemOf("CERTIFICATE", inter.Raw)
	}

	names, ips := []string{"localhost"}, []net.IP{net.ParseIP("127.0.0.1"), net.ParseIP("::1")}
	for _, h := range hosts {
		if ip := net.ParseIP(h); ip != nil {
			ips = append(ips, ip)
		} else {
			names = append(names, h)
		}
	}
	sk, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	server, err := sign(&x509.Certificate{
		Subject:     pkix.Name{CommonName: "localhost"},
		DNSNames:    names,
		IPAddresses: ips,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}, sk, inter, interKey, from, until)
	if err != nil {
		return nil, err
	}
	p.ServerCert, p.ServerKey = chain(server), pkcs8(sk)
	p.server = tls.Certificate{Certificate: [][]byte{server.Raw, inter.Raw}, PrivateKey: sk}

	ck, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	client, err := sign(&x509.Certificate{Subject: pkix.Name{CommonName: "Asuka Test Client"}, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}}, ck, inter, interKey, from, until)
	if err != nil {
		return nil, err
	}
	sec1, _ := x509.MarshalECPrivateKey(ck)
	p.ClientCert, p.ClientKey, p.ClientKeySEC1 = chain(client), pkcs8(ck), pemOf("EC PRIVATE KEY", sec1)

	rk, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return nil, err
	}
	rc, err := sign(&x509.Certificate{Subject: pkix.Name{CommonName: "Asuka Test RSA Client"}, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}}, rk, inter, interKey, from, until)
	if err != nil {
		return nil, err
	}
	p.RSAClientCert, p.RSAClientKey = chain(rc), pemOf("RSA PRIVATE KEY", x509.MarshalPKCS1PrivateKey(rk))
	return p, nil
}

// ServerConfig is the server's: its chain, and a client's certificate
// asked for and checked against CA when one's given (/mtls answers 403
// without).
func (p *PKI) ServerConfig() *tls.Config {
	return &tls.Config{Certificates: []tls.Certificate{p.server}, ClientAuth: tls.VerifyClientCertIfGiven, ClientCAs: p.pool}
}

// Files are the PEM files WriteFiles writes, by name.
func (p *PKI) Files() map[string]string {
	return map[string]string{
		"ca.pem": p.CA, "other-ca.pem": p.OtherCA,
		"client.pem": p.ClientCert, "client.key": p.ClientKey, "client-sec1.key": p.ClientKeySEC1,
		"rsa-client.pem": p.RSAClientCert, "rsa-client.key": p.RSAClientKey,
	}
}

// WriteFiles writes the clients' PEM files into dir (Files' names).
func (p *PKI) WriteFiles(dir string) error {
	for name, text := range p.Files() {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(text), 0o644); err != nil {
			return err
		}
	}
	return nil
}

// StartTLS serves h over HTTPS on the loopback, with p's certificate.
func StartTLS(h http.Handler, p *PKI) *httptest.Server {
	s := httptest.NewUnstartedServer(h)
	s.TLS = p.ServerConfig()
	s.StartTLS()
	return s
}

func authority(name string, parent *x509.Certificate, parentKey crypto.Signer, from, until time.Time) (*x509.Certificate, crypto.Signer, error) {
	k, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	t := &x509.Certificate{Subject: pkix.Name{CommonName: name}, IsCA: true, BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageCRLSign}
	if parent == nil {
		c, err := sign(t, k, t, k, from, until)
		return c, k, err
	}
	c, err := sign(t, k, parent, parentKey, from, until)
	return c, k, err
}

func sign(t *x509.Certificate, key crypto.Signer, parent *x509.Certificate, parentKey crypto.Signer, from, until time.Time) (*x509.Certificate, error) {
	serial, _ := rand.Int(rand.Reader, big.NewInt(1<<62))
	t.SerialNumber, t.NotBefore, t.NotAfter = serial, from, until
	if !t.IsCA {
		t.KeyUsage = x509.KeyUsageDigitalSignature
		if _, ok := key.(*rsa.PrivateKey); ok {
			t.KeyUsage |= x509.KeyUsageKeyEncipherment
		}
	}
	der, err := x509.CreateCertificate(rand.Reader, t, parent, key.Public(), parentKey)
	if err != nil {
		return nil, err
	}
	return x509.ParseCertificate(der)
}

func pkcs8(k crypto.Signer) string {
	der, _ := x509.MarshalPKCS8PrivateKey(k)
	return pemOf("PRIVATE KEY", der)
}

func pemOf(kind string, der []byte) string {
	return string(pem.EncodeToMemory(&pem.Block{Type: kind, Bytes: der}))
}

// mtls answers who the client is, from the certificate it showed: 403
// without one.
func mtls(w http.ResponseWriter, r *http.Request) {
	if r.TLS == nil || len(r.TLS.PeerCertificates) == 0 {
		http.Error(w, "no client certificate", http.StatusForbidden)
		return
	}
	w.Write([]byte("client: " + r.TLS.PeerCertificates[0].Subject.CommonName))
}
