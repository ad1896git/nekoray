package main

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/sagernet/quic-go"
	qtls "github.com/sagernet/sing-quic"
	"github.com/sagernet/sing-quic/hysteria"
	"github.com/sagernet/sing-quic/hysteria2"
	"github.com/sagernet/sing-quic/tuic"
	"github.com/sagernet/sing/common/buf"
	"github.com/sagernet/sing/common/logger"
	M "github.com/sagernet/sing/common/metadata"
	N "github.com/sagernet/sing/common/network"
	aTLS "github.com/sagernet/sing/common/tls"
)

// Keep these tests sequential: other tests in this package own core globals.
// All destinations are synthetic loopback addresses; handlers echo in process.
// No sing-box test module, external resolver, or upstream service is involved.
type nk04TLS struct {
	aTLS.ServerConfig // Unused stream-TLS methods must never be called by QUIC.
	config            *tls.Config
	accepted          chan quic.Connection
}

func (c *nk04TLS) Config() (*tls.Config, error) { return c.config, nil }
func (c *nk04TLS) NextProtos() []string         { return c.config.NextProtos }
func (c *nk04TLS) SetNextProtos(p []string)     { c.config.NextProtos = p }
func (c *nk04TLS) ConfigureHTTP3()              {}
func (c *nk04TLS) Listen(p net.PacketConn, cfg *quic.Config) (qtls.Listener, error) {
	l, err := quic.Listen(p, c.config, cfg)
	if err != nil {
		return nil, err
	}
	return &nk04Listener{Listener: l, accepted: c.accepted}, nil
}
func (c *nk04TLS) ListenEarly(p net.PacketConn, cfg *quic.Config) (qtls.EarlyListener, error) {
	return quic.ListenEarly(p, c.config, cfg)
}

type nk04Listener struct {
	*quic.Listener
	accepted chan quic.Connection
}

func (l *nk04Listener) Accept(ctx context.Context) (quic.Connection, error) {
	c, err := l.Listener.Accept(ctx)
	if err == nil {
		select {
		case l.accepted <- c:
		case <-ctx.Done():
			_ = c.CloseWithError(0, "test canceled")
		}
	}
	return c, err
}

type nk04Dialer struct{ hops chan struct{} }

func (d *nk04Dialer) DialContext(ctx context.Context, network string, dst M.Socksaddr) (net.Conn, error) {
	if !dst.Addr.IsLoopback() {
		return nil, fmt.Errorf("non-loopback destination: %s", dst)
	}
	return (&net.Dialer{LocalAddr: &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)}}).DialContext(ctx, network, dst.String())
}
func (d *nk04Dialer) ListenPacket(ctx context.Context, _ M.Socksaddr) (net.PacketConn, error) {
	p, err := (&net.ListenConfig{}).ListenPacket(ctx, "udp4", "127.0.0.1:0")
	if err == nil && d.hops != nil {
		select {
		case d.hops <- struct{}{}:
		default:
		}
	}
	return p, err
}

type nk04Echo struct{}

func (nk04Echo) NewConnection(ctx context.Context, c net.Conn, _ M.Metadata) error {
	defer c.Close()
	deadline, _ := ctx.Deadline()
	if err := c.SetDeadline(deadline); err != nil {
		return err
	}
	_, err := io.Copy(c, c)
	return err
}
func (nk04Echo) NewPacketConnection(ctx context.Context, c N.PacketConn, _ M.Metadata) error {
	defer c.Close()
	// sing-quic packet connections do not implement deadlines. Close interrupts
	// ReadPacket when the bounded fixture context expires or is canceled.
	stop := context.AfterFunc(ctx, func() { _ = c.Close() })
	defer stop()
	for {
		b := buf.NewPacket()
		dst, err := c.ReadPacket(b)
		if err != nil {
			b.Release()
			return err
		}
		if err := c.WritePacket(b, dst); err != nil {
			return err
		} // consumes b
	}
}

type nk04Client interface {
	DialConn(context.Context, M.Socksaddr) (net.Conn, error)
	CloseWithError(error) error
}
type nk04PacketClient interface {
	ListenPacket(context.Context) (net.PacketConn, error)
}
type nk04Service interface {
	Start(net.PacketConn) error
	Close() error
}
type nk04Fixture struct {
	ctx       context.Context
	address   M.Socksaddr
	clientTLS *nk04TLS
	accepted  chan quic.Connection
	dialer    *nk04Dialer
	protocol  string
}

func nk04Server(t *testing.T, protocol, obfs string, udpDisabled bool) *nk04Fixture {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	t.Cleanup(cancel)
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	cert := &x509.Certificate{SerialNumber: big.NewInt(1), NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour)}
	der, err := x509.CreateCertificate(rand.Reader, cert, cert, public, private)
	if err != nil {
		t.Fatal(err)
	}
	alpn := "h3"
	if protocol == "hysteria1" {
		alpn = hysteria.DefaultALPN
	}
	if protocol == "tuic" {
		alpn = "tuic"
	}
	accepted := make(chan quic.Connection, 8)
	serverTLS := &nk04TLS{config: &tls.Config{Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: private}}, NextProtos: []string{alpn}}, accepted: accepted}
	f := &nk04Fixture{ctx: ctx, protocol: protocol, accepted: accepted, dialer: &nk04Dialer{},
		clientTLS: &nk04TLS{config: &tls.Config{InsecureSkipVerify: true, NextProtos: []string{alpn}}}, // Ephemeral loopback certificate.
	}
	var service nk04Service
	switch protocol {
	case "hysteria2":
		s, e := hysteria2.NewService[string](hysteria2.ServiceOptions{Context: ctx, Logger: logger.NOP(), TLSConfig: serverTLS, Handler: nk04Echo{}, SalamanderPassword: obfs, UDPDisabled: udpDisabled, UDPTimeout: 8 * time.Second})
		err = e
		if e == nil {
			s.UpdateUsers([]string{"user"}, []string{"secret"})
			service = s
		}
	case "hysteria1":
		s, e := hysteria.NewService[string](hysteria.ServiceOptions{Context: ctx, Logger: logger.NOP(), TLSConfig: serverTLS, Handler: nk04Echo{}, SendBPS: hysteria.MinSpeedBPS, ReceiveBPS: hysteria.MinSpeedBPS, UDPTimeout: 8 * time.Second})
		err = e
		if e == nil {
			s.UpdateUsers([]string{"user"}, []string{"secret"})
			service = s
		}
	case "tuic":
		s, e := tuic.NewService[string](tuic.ServiceOptions{Context: ctx, Logger: logger.NOP(), TLSConfig: serverTLS, Handler: nk04Echo{}, AuthTimeout: 2 * time.Second, UDPTimeout: 8 * time.Second})
		err = e
		if e == nil {
			s.UpdateUsers([]string{"user"}, [][16]byte{{1}}, []string{"secret"})
			service = s
		}
	default:
		t.Fatalf("unknown protocol %q", protocol)
	}
	if err != nil {
		t.Fatal(err)
	}
	p := nk04Socket(t, false)
	f.address = M.SocksaddrFromNet(p.LocalAddr())
	if err := service.Start(p); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		cancel()
		_ = service.Close()
		for {
			select {
			case c := <-accepted:
				_ = c.CloseWithError(0, "test cleanup")
			default:
				return
			}
		}
	})
	return f
}

func nk04Socket(t *testing.T, hopping bool) net.PacketConn {
	t.Helper()
	p, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		if hopping {
			t.Skipf("cannot bind loopback hop port 127.0.0.1:0: %v", err)
		}
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = p.Close() })
	return p
}

func (f *nk04Fixture) client(t *testing.T, password, obfs, hopPorts string, uuid [16]byte, udpStream bool) nk04Client {
	t.Helper()
	var c nk04Client
	var err error
	switch f.protocol {
	case "hysteria2":
		c, err = hysteria2.NewClient(hysteria2.ClientOptions{Context: f.ctx, Dialer: f.dialer, Logger: logger.NOP(), ServerAddress: f.address, TLSConfig: f.clientTLS, Password: password, SalamanderPassword: obfs, HopPorts: hopPorts, HopInterval: 5})
	case "hysteria1":
		c, err = hysteria.NewClient(hysteria.ClientOptions{Context: f.ctx, Dialer: f.dialer, Logger: logger.NOP(), ServerAddress: f.address, TLSConfig: f.clientTLS, Password: password, SendBPS: hysteria.MinSpeedBPS, ReceiveBPS: hysteria.MinSpeedBPS})
	case "tuic":
		c, err = tuic.NewClient(tuic.ClientOptions{Context: f.ctx, Dialer: f.dialer, ServerAddress: f.address, TLSConfig: f.clientTLS, Password: password, UUID: uuid, UDPStream: udpStream})
	}
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = c.CloseWithError(net.ErrClosed) })
	return c
}

func nk04TCP(ctx context.Context, c nk04Client) error {
	conn, err := c.DialConn(ctx, M.ParseSocksaddr("127.0.0.1:12345"))
	if err != nil {
		return err
	}
	defer conn.Close()
	return nk04Stream(ctx, conn)
}
func nk04Stream(ctx context.Context, conn net.Conn) error {
	deadline, _ := ctx.Deadline()
	if err := conn.SetDeadline(deadline); err != nil {
		return err
	}
	payload := []byte("NK04 loopback TCP echo")
	if _, err := conn.Write(payload); err != nil {
		return err
	}
	reply := make([]byte, len(payload))
	if _, err := io.ReadFull(conn, reply); err != nil {
		return err
	}
	if !bytes.Equal(reply, payload) {
		return fmt.Errorf("TCP echo = %q, want %q", reply, payload)
	}
	return nil
}
func nk04UDP(ctx context.Context, c nk04Client) error {
	p, err := c.(nk04PacketClient).ListenPacket(ctx)
	if err != nil {
		return err
	}
	defer p.Close()
	// ReadFrom has no deadline support; cancellation closes the packet session.
	stop := context.AfterFunc(ctx, func() { _ = p.Close() })
	defer stop()
	dst := M.ParseSocksaddr("127.0.0.1:12345")
	payload := []byte("NK04 loopback UDP echo")
	result := make(chan error, 1)
	go func() {
		reply := make([]byte, 128)
		n, addr, err := p.ReadFrom(reply)
		if err == nil && (!bytes.Equal(reply[:n], payload) || addr.String() != dst.String()) {
			err = fmt.Errorf("UDP echo = %q from %v", reply[:n], addr)
		}
		result <- err
	}()
	// QUIC datagrams are unreliable, including on loopback when the burst
	// temporarily fills a receive queue. Retry the sentinel within the deadline.
	retry := time.NewTicker(50 * time.Millisecond)
	defer retry.Stop()
	for {
		if _, err := p.WriteTo(payload, dst); err != nil {
			return err
		}
		select {
		case err := <-result:
			return err
		case <-ctx.Done():
			return ctx.Err()
		case <-retry.C:
		}
	}
}
func nk04OK(t *testing.T, err error) {
	t.Helper()
	if err != nil {
		t.Fatal(err)
	}
}
func nk04Rejected(t *testing.T, ctx context.Context, c nk04Client) {
	t.Helper()
	err := nk04TCP(ctx, c) // TUIC authenticates asynchronously; DialConn alone is insufficient.
	if err == nil {
		t.Fatal("invalid credentials completed a TCP round trip")
	}
	var timeout net.Error
	if ctx.Err() != nil || errors.Is(err, context.DeadlineExceeded) || (errors.As(err, &timeout) && timeout.Timeout()) {
		t.Fatalf("authentication hung until timeout: %v", err)
	}
	t.Logf("credentials rejected: %v", err)
}

func TestNK04Hysteria2TCP(t *testing.T) {
	f := nk04Server(t, "hysteria2", "", false)
	nk04OK(t, nk04TCP(f.ctx, f.client(t, "secret", "", "", [16]byte{}, false)))
}
func TestNK04Hysteria2UDP(t *testing.T) {
	f := nk04Server(t, "hysteria2", "", false)
	nk04OK(t, nk04UDP(f.ctx, f.client(t, "secret", "", "", [16]byte{}, false)))
}
func TestNK04Hysteria2WrongPassword(t *testing.T) {
	f := nk04Server(t, "hysteria2", "", false)
	nk04Rejected(t, f.ctx, f.client(t, "wrong", "", "", [16]byte{}, false))
}
func TestNK04Hysteria2Salamander(t *testing.T) {
	for _, obfs := range []string{"", "obfs-secret"} {
		name := "off"
		if obfs != "" {
			name = "on"
		}
		t.Run(name, func(t *testing.T) {
			f := nk04Server(t, "hysteria2", obfs, false)
			c := f.client(t, "secret", obfs, "", [16]byte{}, false)
			nk04OK(t, nk04TCP(f.ctx, c))
			nk04OK(t, nk04UDP(f.ctx, c))
		})
	}
	t.Run("wrong_password", func(t *testing.T) {
		f := nk04Server(t, "hysteria2", "obfs-secret", false)
		c := f.client(t, "secret", "wrong-obfs", "", [16]byte{}, false)
		ctx, cancel := context.WithTimeout(f.ctx, 2*time.Second)
		defer cancel()
		// Wrong obfuscation is silently discarded before QUIC; a bounded handshake
		// timeout is the expected rejection, unlike an application auth failure.
		if err := nk04TCP(ctx, c); err == nil {
			t.Fatal("wrong obfuscation password accepted")
		} else {
			t.Logf("wrong obfuscation rejected: %v", err)
		}
		select {
		case <-f.accepted:
			t.Fatal("wrong obfuscation reached QUIC accept")
		default:
		}
	})
}
func TestNK04Hysteria2TCPOnly(t *testing.T) {
	f := nk04Server(t, "hysteria2", "", true)
	c := f.client(t, "secret", "", "", [16]byte{}, false)
	nk04OK(t, nk04TCP(f.ctx, c))
	p, err := c.(nk04PacketClient).ListenPacket(f.ctx)
	if p != nil {
		_ = p.Close()
	}
	if err == nil || f.ctx.Err() != nil {
		t.Fatalf("UDP on TCP-only server: %v (context %v)", err, f.ctx.Err())
	}
	nk04OK(t, nk04TCP(f.ctx, c))
}
func TestNK04Hysteria2Reconnect(t *testing.T) {
	f := nk04Server(t, "hysteria2", "", false)
	c := f.client(t, "secret", "", "", [16]byte{}, false)
	conn, err := c.DialConn(f.ctx, M.ParseSocksaddr("127.0.0.1:12345"))
	nk04OK(t, err)
	defer conn.Close()
	nk04OK(t, nk04Stream(f.ctx, conn))
	var first quic.Connection
	select {
	case first = <-f.accepted:
	case <-f.ctx.Done():
		t.Fatal("server did not accept first connection")
	}
	nk04OK(t, first.CloseWithError(0, "NK04 server disconnect"))
	// Wait for the remote close to reach the client, without a timing sleep.
	deadline, _ := f.ctx.Deadline()
	nk04OK(t, conn.SetReadDeadline(deadline))
	if _, err := conn.Read(make([]byte, 1)); err == nil {
		t.Fatal("closed QUIC connection remained readable")
	}
	// The stream can observe the close before the client drops its cached connection.
	ticker := time.NewTicker(25 * time.Millisecond)
	defer ticker.Stop()
	for attempts := 1; ; attempts++ {
		err = nk04TCP(f.ctx, c)
		t.Logf("reconnect attempt %d: %v", attempts, err)
		if err == nil {
			break
		}
		select {
		case <-f.ctx.Done():
			t.Fatalf("reconnect failed after %d attempts: %v", attempts, err)
		case <-ticker.C:
		}
	}
	select {
	case next := <-f.accepted:
		if next == first {
			t.Fatal("reused closed connection")
		}
	case <-f.ctx.Done():
		t.Fatal("server did not accept reconnection")
	}
}
func TestNK04Hysteria1TCP(t *testing.T) {
	f := nk04Server(t, "hysteria1", "", false)
	nk04OK(t, nk04TCP(f.ctx, f.client(t, "secret", "", "", [16]byte{}, false)))
}
func TestNK04Hysteria1WrongAuth(t *testing.T) {
	f := nk04Server(t, "hysteria1", "", false)
	nk04Rejected(t, f.ctx, f.client(t, "wrong", "", "", [16]byte{}, false))
}
func TestNK04TUICTCP(t *testing.T) {
	f := nk04Server(t, "tuic", "", false)
	nk04OK(t, nk04TCP(f.ctx, f.client(t, "secret", "", "", [16]byte{1}, false)))
}
func TestNK04TUICWrongPassword(t *testing.T) {
	f := nk04Server(t, "tuic", "", false)
	nk04Rejected(t, f.ctx, f.client(t, "wrong", "", "", [16]byte{1}, false))
}
func TestNK04TUICUnknownUUID(t *testing.T) {
	f := nk04Server(t, "tuic", "", false)
	nk04Rejected(t, f.ctx, f.client(t, "secret", "", "", [16]byte{2}, false))
}
func TestNK04TUICUDPStream(t *testing.T) {
	f := nk04Server(t, "tuic", "", false)
	nk04OK(t, nk04UDP(f.ctx, f.client(t, "secret", "", "", [16]byte{1}, true)))
}
func TestNK04TUICUDPDatagram(t *testing.T) {
	f := nk04Server(t, "tuic", "", false)
	nk04OK(t, nk04UDP(f.ctx, f.client(t, "secret", "", "", [16]byte{1}, false)))
}

func TestNK04Hysteria2ConcurrentUDP(t *testing.T) {
	f := nk04Server(t, "hysteria2", "", false)
	c := f.client(t, "secret", "", "", [16]byte{}, false)
	// Keep a stream alive to prove the post-burst traffic uses the same QUIC connection.
	conn, err := c.DialConn(f.ctx, M.ParseSocksaddr("127.0.0.1:12345"))
	nk04OK(t, err)
	defer conn.Close()
	nk04OK(t, nk04Stream(f.ctx, conn))
	p, err := c.(nk04PacketClient).ListenPacket(f.ctx)
	nk04OK(t, err)
	defer p.Close()
	const sends = 200 // Below sing-quic dev's default 1024 incoming-stream budget (F02).
	start := make(chan struct{})
	results := make(chan error, sends)
	var wg sync.WaitGroup
	for i := 0; i < sends; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			select {
			case <-start:
			case <-f.ctx.Done():
				results <- f.ctx.Err()
				return
			}
			_, err := p.WriteTo([]byte(fmt.Sprintf("burst-%03d", i)), M.ParseSocksaddr("127.0.0.1:12345"))
			results <- err
		}(i)
	}
	done := make(chan struct{})
	go func() { wg.Wait(); close(done) }()
	close(start)
	select {
	case <-done:
	case <-f.ctx.Done():
		t.Fatal("200 concurrent UDP sends hung")
	}
	for i := 0; i < sends; i++ {
		nk04OK(t, <-results)
	}
	// Datagrams may be dropped during the burst. A fresh packet session avoids
	// confusing queued burst replies with the sentinel, on the same QUIC connection.
	nk04OK(t, nk04UDP(f.ctx, c))
	nk04OK(t, nk04Stream(f.ctx, conn))
	select {
	case <-f.accepted:
	case <-f.ctx.Done():
		t.Fatal("missing accepted connection")
	}
	select {
	case <-f.accepted:
		t.Fatal("burst forced a new QUIC connection")
	default:
	}
}

// Relay two ephemeral hop ports through one upstream socket, preserving QUIC's
// server-side peer address when udphop rotates its local socket and target port.
func nk04HopRelay(t *testing.T, f *nk04Fixture, fronts []net.PacketConn) (string, <-chan string) {
	t.Helper()
	upstream := nk04Socket(t, true)
	changed := make(chan string, 8)
	deadline, _ := f.ctx.Deadline()
	for _, p := range append([]net.PacketConn{upstream}, fronts...) {
		nk04OK(t, p.SetDeadline(deadline))
	}
	var mu sync.Mutex
	var current net.PacketConn
	var peer net.Addr
	var wg sync.WaitGroup
	for _, front := range fronts {
		wg.Add(1)
		go func(front net.PacketConn) {
			defer wg.Done()
			b := make([]byte, 65535)
			for {
				n, addr, err := front.ReadFrom(b)
				if err != nil {
					return
				}
				mu.Lock()
				if peer == nil || peer.String() != addr.String() {
					select {
					case changed <- addr.String():
					default:
					}
				}
				current, peer = front, addr
				mu.Unlock()
				if _, err := upstream.WriteTo(b[:n], f.address.UDPAddr()); err != nil {
					return
				}
			}
		}(front)
	}
	wg.Add(1)
	go func() {
		defer wg.Done()
		b := make([]byte, 65535)
		for {
			n, _, err := upstream.ReadFrom(b)
			if err != nil {
				return
			}
			mu.Lock()
			front, addr := current, peer
			mu.Unlock()
			if front != nil {
				if _, err := front.WriteTo(b[:n], addr); err != nil {
					return
				}
			}
		}
	}()
	t.Cleanup(func() {
		for _, p := range fronts {
			_ = p.Close()
		}
		_ = upstream.Close()
		done := make(chan struct{})
		go func() { wg.Wait(); close(done) }()
		select {
		case <-done:
		case <-time.After(time.Second):
			t.Error("hop relay workers did not exit")
		}
	})
	a := fronts[0].LocalAddr().(*net.UDPAddr).Port
	b := fronts[1].LocalAddr().(*net.UDPAddr).Port
	// Singleton ranges honor the requirement that every listener binds :0;
	// choosing a contiguous fixed range would require rebinding a nonzero port.
	return fmt.Sprintf("%d-%d,%d-%d", a, a, b, b), changed
}
func TestNK04Hysteria2PortHopping(t *testing.T) {
	// Bind the hop listeners first so a host that forbids them reports the
	// requested, reason-bearing skip rather than failing during server setup.
	fronts := []net.PacketConn{nk04Socket(t, true), nk04Socket(t, true)}
	f := nk04Server(t, "hysteria2", "", false)
	ports, changed := nk04HopRelay(t, f, fronts)
	f.dialer.hops = make(chan struct{}, 4)
	// ClientOptions.HopPorts invokes sing-quic/udphop, rather than constructing
	// a separate test transport. ServerAddress is deliberately outside the range.
	c := f.client(t, "secret", "", ports, [16]byte{}, false)
	nk04OK(t, nk04TCP(f.ctx, c))
	var firstPeer string
	select {
	case firstPeer = <-changed:
	case <-f.ctx.Done():
		t.Fatal("traffic did not reach the loopback hop range")
	}
	select {
	case <-f.dialer.hops:
	case <-f.ctx.Done():
		t.Fatal("udphop did not create its first socket")
	}
	select {
	case <-f.dialer.hops:
	case <-f.ctx.Done():
		t.Fatal("udphop did not rotate its socket at the 5s interval")
	}
	// ListenPacket announces creation just before udphop installs the socket.
	// Round trips provide progress until the relay observes the new source.
	for rotated := false; !rotated; {
		nk04OK(t, nk04TCP(f.ctx, c))
		select {
		case peer := <-changed:
			rotated = peer != firstPeer
		case <-f.ctx.Done():
			t.Fatal("hop socket was created but traffic did not rotate")
		default:
		}
	}
	nk04OK(t, nk04UDP(f.ctx, c))
}
