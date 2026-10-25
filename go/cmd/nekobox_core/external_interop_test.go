package main

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gofrs/uuid/v5"
	"github.com/sagernet/quic-go"
	"github.com/sagernet/sing-quic/hysteria"
	"github.com/sagernet/sing-quic/hysteria2"
	"github.com/sagernet/sing-quic/tuic"
	"github.com/sagernet/sing/common/logger"
	M "github.com/sagernet/sing/common/metadata"
	"golang.org/x/net/proxy"
)

const (
	nk04ExternalHysteriaImage   = "tobyxdd/hysteria@sha256:4c8c92ba4b9b405c9473d8776b613e4d776141a3fa26ae5084a19c44b49f1e35"
	nk04ExternalHysteria2Image  = "tobyxdd/hysteria@sha256:92b06917b2abb940e90ea6dc9215c1b1ceea69fa7edc1374daf3cde36e422740"
	nk04ExternalTUICServerImage = "kilvn/tuic-server@sha256:15c73b7b8d0f6947fde6cf3c4ce14b3735236c306e27c00412530a710f783c61"
	nk04ExternalTUICClientImage = "kilvn/tuic-client@sha256:4c469c277548170c48c9100554f3efa56df9bf9599a78ea5e28acc5fa6988abc"
	nk04ExternalXPlus           = "fuck me till the daylight"
	nk04ExternalSalamander      = "cry_me_a_r1ver"
	nk04ExternalUUID            = "FE35D05B-8803-45C4-BAE6-723AD2CD5D3D"
)

// Copied from sing-box/test/config/hysteria-client.json:1-12; only listener addresses/ports change.
const nk04ExternalHysteriaClient = `{
  "server": "127.0.0.1:{{server}}",
  "auth_str": "password",
  "obfs": "fuck me till the daylight",
  "up_mbps": 100,
  "down_mbps": 100,
  "socks5": {
    "listen": "127.0.0.1:{{client}}"
  },
  "server_name": "example.org",
  "ca": "/etc/hysteria/ca.pem"
}`

// Copied from sing-box/test/config/hysteria-server.json:1-9; only listener addresses/ports change.
const nk04ExternalHysteriaServer = `{
  "listen": "127.0.0.1:{{server}}",
  "cert": "/etc/hysteria/cert.pem",
  "key": "/etc/hysteria/key.pem",
  "auth_str": "password",
  "obfs": "fuck me till the daylight",
  "up_mbps": 100,
  "down_mbps": 100
}`

// Copied from sing-box/test/config/hysteria2-client.yml:1-11; only listener addresses/ports change.
const nk04ExternalHysteria2Client = `server: 127.0.0.1:{{server}}
auth: password
socks5:
  listen: 127.0.0.1:{{client}}
tls:
  sni: example.org
  ca: /etc/hysteria/ca.pem
obfs:
  type: salamander
  salamander:
    password: cry_me_a_r1ver`

// Copied from sing-box/test/config/hysteria2-server.yml:1-12; only listener addresses/ports change.
const nk04ExternalHysteria2Server = `listen: 127.0.0.1:{{server}}
auth:
  type: password
  password: password
tls:
  sni: example.org
  cert: /etc/hysteria/cert.pem
  key: /etc/hysteria/key.pem
obfs:
  type: salamander
  salamander:
    password: cry_me_a_r1ver`

// Copied from sing-box/test/config/tuic-client.json:1-16; only listener addresses/ports change.
const nk04ExternalTuicClient = `{
  "relay": {
    "server": "example.org:{{server}}",
    "ip": "127.0.0.1",
    "uuid": "FE35D05B-8803-45C4-BAE6-723AD2CD5D3D",
    "password": "tuic",
    "certificates": [
      "/etc/tuic/ca.pem"
    ]
  },
  "local": {
    "server": "127.0.0.1:{{client}}",
    "max_packet_size": 65535
  },
  "log_level": "debug"
}`

// Copied from sing-box/test/config/tuic-server.json:1-10; only listener addresses/ports change.
const nk04ExternalTuicServer = `{
    "server": "127.0.0.1:{{server}}",
    "users": {
        "FE35D05B-8803-45C4-BAE6-723AD2CD5D3D": "tuic"
    },
    "certificate":  "/etc/tuic/cert.pem",
    "private_key": "/etc/tuic/key.pem",
    "max_external_packet_size": 65535,
    "log_level": "debug"
}`

func nk04ExternalGate(t *testing.T) {
	t.Helper()
	if os.Getenv("NK04_EXTERNAL") != "1" {
		t.Skip("set NK04_EXTERNAL=1 to run against Docker peers")
	}
}

func nk04ExternalPort(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	nk04OK(t, err)
	port := l.Addr().(*net.TCPAddr).Port
	nk04OK(t, l.Close())
	// The peers cannot inherit a bound socket or report a dynamically bound port.
	// Closing before Docker binds leaves a small race, acceptable in sequential,
	// isolated CI tests; collisions fail the bounded readiness/round-trip checks.
	return port
}

func nk04ExternalCertificate(t *testing.T) (string, tls.Certificate, *x509.CertPool) {
	t.Helper()
	pub, key, err := ed25519.GenerateKey(rand.Reader)
	nk04OK(t, err)
	cert := &x509.Certificate{SerialNumber: big.NewInt(1), DNSNames: []string{"example.org"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour),
		IsCA: true, BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}
	der, err := x509.CreateCertificate(rand.Reader, cert, cert, pub, key)
	nk04OK(t, err)
	keyDER, err := x509.MarshalPKCS8PrivateKey(key)
	nk04OK(t, err)
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})
	dir := t.TempDir()
	nk04OK(t, os.WriteFile(filepath.Join(dir, "cert.pem"), certPEM, 0644))
	// Containers may run as a non-root user. This is an ephemeral test-only key.
	nk04OK(t, os.WriteFile(filepath.Join(dir, "key.pem"), keyPEM, 0644))
	nk04OK(t, os.WriteFile(filepath.Join(dir, "ca.pem"), certPEM, 0644))
	pair, err := tls.X509KeyPair(certPEM, keyPEM)
	nk04OK(t, err)
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(certPEM) {
		t.Fatal("invalid generated CA")
	}
	return dir, pair, roots
}

// Arguments and mount destinations follow hysteria_test.go:118-126,130-141,
// hysteria2_test.go:137-145,149-160 and tuic_test.go:131-138,142-152.
// docker_test.go:38-99 uses host networking and bind-mounted files; these
// peers do not use stdin. Keep the image entrypoints (especially TUIC's).
func nk04ExternalContainer(t *testing.T, protocol, role, dir string, server, client int) {
	t.Helper()
	var image, config, internal string
	var command []string
	switch protocol {
	case "hysteria1":
		image, internal = nk04ExternalHysteriaImage, "/etc/hysteria"
		config = nk04ExternalHysteriaServer
		if role == "client" {
			config = nk04ExternalHysteriaClient
		}
		command = []string{"-c", internal + "/config.json", role}
	case "hysteria2":
		image, internal = nk04ExternalHysteria2Image, "/etc/hysteria"
		config = nk04ExternalHysteria2Server
		if role == "client" {
			config = nk04ExternalHysteria2Client
		}
		command = []string{role, "-c", internal + "/config.yml", "--disable-update-check", "--log-level", "debug"}
	case "tuic":
		image, internal, config = nk04ExternalTUICServerImage, "/etc/tuic", nk04ExternalTuicServer
		if role == "client" {
			image, config = nk04ExternalTUICClientImage, nk04ExternalTuicClient
		}
	default:
		t.Fatalf("unknown protocol %q", protocol)
	}
	config = strings.NewReplacer("{{server}}", fmt.Sprint(server), "{{client}}", fmt.Sprint(client)).Replace(config)
	filename := "config.json"
	if protocol == "hysteria2" {
		filename = "config.yml"
	}
	path := filepath.Join(dir, role+"-"+filename)
	nk04OK(t, os.WriteFile(path, []byte(config), 0644))
	name := fmt.Sprintf("nk04-%s-%s-%d", t.Name(), role, time.Now().UnixNano())
	args := []string{"run", "--network", "host", "--rm", "--name", name}
	for _, file := range []string{"cert.pem", "key.pem", "ca.pem"} {
		args = append(args, "--mount", "type=bind,src="+filepath.Join(dir, file)+",dst="+internal+"/"+file+",readonly")
	}
	args = append(args, "--mount", "type=bind,src="+path+",dst="+internal+"/"+filename+",readonly", image)
	args = append(args, command...)
	logFile, err := os.Create(filepath.Join(dir, role+".log"))
	nk04OK(t, err)
	cmd := exec.Command("docker", args...)
	cmd.Stdout, cmd.Stderr = logFile, logFile
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		output, err := exec.CommandContext(ctx, "docker", "rm", "-f", name).CombinedOutput()
		if err != nil {
			t.Logf("docker cleanup: %v: %s", err, output)
		}
		if cmd.Process != nil {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
		}
		_ = logFile.Close()
		if t.Failed() {
			data, _ := os.ReadFile(logFile.Name())
			t.Logf("%s peer logs:\n%s", role, data)
		}
	})
	nk04OK(t, cmd.Start())
}

func nk04ExternalReady(t *testing.T, address string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	for {
		conn, err := (&net.Dialer{Timeout: 200 * time.Millisecond}).DialContext(ctx, "tcp", address)
		if err == nil {
			_ = conn.Close()
			return
		}
		select {
		case <-ctx.Done():
			t.Fatalf("peer TCP readiness %s: %v", address, err)
		case <-ticker.C:
		}
	}
}

func nk04ExternalEcho(t *testing.T) string {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	nk04OK(t, err)
	t.Cleanup(func() { _ = l.Close() })
	go func() {
		for {
			c, err := l.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				_ = c.SetDeadline(time.Now().Add(10 * time.Second))
				_, _ = io.Copy(c, c)
			}()
		}
	}()
	return l.Addr().String()
}

type nk04ExternalRelay struct{ nk04Echo }

func (nk04ExternalRelay) NewConnection(ctx context.Context, c net.Conn, metadata M.Metadata) error {
	defer c.Close()
	if !metadata.Destination.Addr.IsLoopback() {
		return fmt.Errorf("non-loopback target: %s", metadata.Destination)
	}
	upstream, err := (&net.Dialer{}).DialContext(ctx, "tcp", metadata.Destination.String())
	if err != nil {
		return err
	}
	defer upstream.Close()
	stop := context.AfterFunc(ctx, func() { _ = c.Close(); _ = upstream.Close() })
	defer stop()
	go func() { _, _ = io.Copy(upstream, c); _ = upstream.Close() }()
	_, err = io.Copy(c, upstream)
	return err
}

func nk04ExternalServer(t *testing.T, protocol string, pair tls.Certificate) int {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	config := &nk04TLS{config: &tls.Config{Certificates: []tls.Certificate{pair}}, accepted: make(chan quic.Connection, 8)}
	var service nk04Service
	var err error
	switch protocol {
	case "hysteria1":
		s, e := hysteria.NewService[string](hysteria.ServiceOptions{Context: ctx, Logger: logger.NOP(), TLSConfig: config, Handler: nk04ExternalRelay{}, XPlusPassword: nk04ExternalXPlus, SendBPS: 12500000, ReceiveBPS: 12500000})
		err = e
		if e == nil {
			s.UpdateUsers([]string{"user"}, []string{"password"})
			service = s
		}
	case "hysteria2":
		s, e := hysteria2.NewService[string](hysteria2.ServiceOptions{Context: ctx, Logger: logger.NOP(), TLSConfig: config, Handler: nk04ExternalRelay{}, SalamanderPassword: nk04ExternalSalamander})
		err = e
		if e == nil {
			s.UpdateUsers([]string{"user"}, []string{"password"})
			service = s
		}
	case "tuic":
		s, e := tuic.NewService[string](tuic.ServiceOptions{Context: ctx, Logger: logger.NOP(), TLSConfig: config, Handler: nk04ExternalRelay{}, AuthTimeout: 3 * time.Second})
		err = e
		if e == nil {
			s.UpdateUsers([]string{"user"}, [][16]byte{uuid.Must(uuid.FromString(nk04ExternalUUID))}, []string{"tuic"})
			service = s
		}
	}
	nk04OK(t, err)
	p := nk04Socket(t, false)
	nk04OK(t, service.Start(p))
	t.Cleanup(func() { cancel(); _ = service.Close() })
	return p.LocalAddr().(*net.UDPAddr).Port
}

func nk04ExternalClient(t *testing.T, ctx context.Context, protocol string, port int, roots *x509.CertPool, password string) nk04Client {
	t.Helper()
	config := &nk04TLS{config: &tls.Config{RootCAs: roots, ServerName: "example.org"}}
	address := M.ParseSocksaddr(fmt.Sprintf("127.0.0.1:%d", port))
	var c nk04Client
	var err error
	switch protocol {
	case "hysteria1":
		c, err = hysteria.NewClient(hysteria.ClientOptions{Context: ctx, Dialer: &nk04Dialer{}, Logger: logger.NOP(), TLSConfig: config, ServerAddress: address, Password: password, XPlusPassword: nk04ExternalXPlus, SendBPS: 12500000, ReceiveBPS: 12500000})
	case "hysteria2":
		c, err = hysteria2.NewClient(hysteria2.ClientOptions{Context: ctx, Dialer: &nk04Dialer{}, Logger: logger.NOP(), TLSConfig: config, ServerAddress: address, Password: password, SalamanderPassword: nk04ExternalSalamander})
	case "tuic":
		c, err = tuic.NewClient(tuic.ClientOptions{Context: ctx, Dialer: &nk04Dialer{}, TLSConfig: config, ServerAddress: address, Password: password, UUID: uuid.Must(uuid.FromString(nk04ExternalUUID))})
	}
	nk04OK(t, err)
	t.Cleanup(func() { _ = c.CloseWithError(net.ErrClosed) })
	return c
}

func nk04ExternalSOCKS(ctx context.Context, address, target string) error {
	d, err := proxy.SOCKS5("tcp", address, nil, &net.Dialer{})
	if err != nil {
		return err
	}
	conn, err := d.(proxy.ContextDialer).DialContext(ctx, "tcp", target)
	if err != nil {
		return err
	}
	defer conn.Close()
	return nk04Stream(ctx, conn)
}

func nk04ExternalRun(t *testing.T, protocol, direction string) {
	t.Helper()
	nk04ExternalGate(t)
	dir, pair, roots := nk04ExternalCertificate(t)
	target := nk04ExternalEcho(t)
	server, client := 0, nk04ExternalPort(t)
	if direction == "inbound" {
		server = nk04ExternalServer(t, protocol, pair)
	} else {
		server = nk04ExternalPort(t)
		nk04ExternalContainer(t, protocol, "server", dir, server, client)
	}
	// QUIC servers expose UDP only. Poll the companion peer client's TCP SOCKS
	// port, then require an authenticated round trip to prove server readiness.
	nk04ExternalContainer(t, protocol, "client", dir, server, client)
	address := fmt.Sprintf("127.0.0.1:%d", client)
	nk04ExternalReady(t, address)
	ready, cancelReady := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancelReady()
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	for {
		attempt, cancel := context.WithTimeout(ready, 2*time.Second)
		err := nk04ExternalSOCKS(attempt, address, target)
		cancel()
		if err == nil {
			break
		}
		select {
		case <-ready.Done():
			t.Fatalf("authenticated peer readiness: %v", err)
		case <-ticker.C:
		}
	}
	if direction == "inbound" {
		return
	}
	password := "password"
	if protocol == "tuic" {
		password = "tuic"
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	good := nk04ExternalClient(t, ctx, protocol, server, roots, password)
	conn, err := good.DialConn(ctx, M.ParseSocksaddr(target))
	nk04OK(t, err)
	defer conn.Close()
	nk04OK(t, nk04Stream(ctx, conn))
	_ = conn.Close()
	if direction != "wrong-password" {
		return
	}
	reject, cancelReject := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancelReject()
	bad := nk04ExternalClient(t, reject, protocol, server, roots, "wrong-password")
	result := make(chan error, 1)
	go func() {
		conn, err := bad.DialConn(reject, M.ParseSocksaddr(target))
		if err == nil {
			defer conn.Close()
			err = nk04Stream(reject, conn)
		}
		result <- err
	}()
	select {
	case err := <-result:
		if err == nil {
			t.Fatal("wrong password completed TCP round trip")
		}
		var timeout net.Error
		if reject.Err() != nil || errors.Is(err, context.DeadlineExceeded) || (errors.As(err, &timeout) && timeout.Timeout()) {
			t.Fatalf("authentication hung until timeout: %v", err)
		}
		t.Logf("credentials rejected: %v", err)
	case <-reject.Done():
		t.Fatal("wrong-password authentication hung for 8s")
	}
}

func TestNK04ExternalHysteria1Outbound(t *testing.T) { nk04ExternalRun(t, "hysteria1", "outbound") }

func TestNK04ExternalHysteria1Inbound(t *testing.T) { nk04ExternalRun(t, "hysteria1", "inbound") }

func TestNK04ExternalHysteria1WrongPassword(t *testing.T) {
	nk04ExternalRun(t, "hysteria1", "wrong-password")
}

func TestNK04ExternalHysteria2Outbound(t *testing.T) { nk04ExternalRun(t, "hysteria2", "outbound") }

func TestNK04ExternalHysteria2Inbound(t *testing.T) { nk04ExternalRun(t, "hysteria2", "inbound") }

func TestNK04ExternalHysteria2WrongPassword(t *testing.T) {
	nk04ExternalRun(t, "hysteria2", "wrong-password")
}

func TestNK04ExternalTUICOutbound(t *testing.T) { nk04ExternalRun(t, "tuic", "outbound") }

func TestNK04ExternalTUICInbound(t *testing.T) { nk04ExternalRun(t, "tuic", "inbound") }

func TestNK04ExternalTUICWrongPassword(t *testing.T) { nk04ExternalRun(t, "tuic", "wrong-password") }
