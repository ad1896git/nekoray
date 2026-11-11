package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/sagernet/quic-go"
	"github.com/sagernet/quic-go/logging"
	qtls "github.com/sagernet/sing-quic"
	"github.com/sagernet/sing-quic/tuic"
	M "github.com/sagernet/sing/common/metadata"
)

// These tests execute in the core module with its real dependency replacements.
// The fixture permits only loopback dials and never resolves a hostname.
func TestNK04MatrixStreamLimit(t *testing.T) {
	f := nk04Server(t, "tuic", "", false)
	c := f.client(t, "secret", "", "", [16]byte{1}, true)
	const limit = 1024
	streams := make([]net.Conn, 0, limit)
	t.Cleanup(func() {
		for _, stream := range streams {
			_ = stream.Close()
		}
	})
	for i := 0; i < limit; i++ {
		stream, err := c.DialConn(f.ctx, M.ParseSocksaddr("127.0.0.1:12345"))
		if err != nil {
			t.Fatalf("stream %d of %d: %v", i+1, limit, err)
		}
		streams = append(streams, stream)
	}
	stream, err := c.DialConn(f.ctx, M.ParseSocksaddr("127.0.0.1:12345"))
	if stream != nil {
		_ = stream.Close()
	}
	var reached *quic.StreamLimitReachedError
	if !errors.As(err, &reached) { // mutation: stream-limit
		t.Fatalf("stream 1025: %v, want StreamLimitReachedError", err)
	}
	// At the cap, both ends of the admitted range still carry application data.
	nk04OK(t, nk04Stream(f.ctx, streams[0]))
	nk04OK(t, nk04Stream(f.ctx, streams[limit-1]))
}

func TestNK04MatrixPacketBoundaries(t *testing.T) {
	for _, mode := range []struct {
		name, protocol string
		stream         bool
		max            int
	}{
		{"hysteria2", "hysteria2", false, 4096},
		{"tuic_datagram", "tuic", false, 65535},
		{"tuic_stream", "tuic", true, 65535},
	} {
		t.Run(mode.name, func(t *testing.T) {
			for _, size := range []int{0, 1, mode.max - 1, mode.max, mode.max + 1} {
				t.Run(fmt.Sprint(size), func(t *testing.T) {
					f := nk04Server(t, mode.protocol, "", false)
					var c nk04Client
					if mode.name == "tuic_datagram" && (size == 65534 || size == 65535) {
						// KI-14: the pinned packer can discard a 1200-byte frame
						// in a 1150-byte handshake-coalesced budget. Test these
						// boundaries after confirmation, with no loss tolerance.
						c = nk04ConfirmedTUICClient(t, f)
					} else {
						c = f.client(t, "secret", "", "", [16]byte{1}, mode.stream)
					}
					p, err := c.(nk04PacketClient).ListenPacket(f.ctx)
					nk04OK(t, err)
					defer p.Close()
					stop := context.AfterFunc(f.ctx, func() { _ = p.Close() })
					defer stop()
					payload := bytes.Repeat([]byte{0xa5}, size)
					dst := M.ParseSocksaddr("127.0.0.1:12345")
					n, err := p.WriteTo(payload, dst)
					if size > mode.max {
						var large *quic.DatagramTooLargeError
						if n != 0 || !errors.As(err, &large) || large.MaxDatagramPayloadSize != int64(mode.max) { // mutation: packet-limit
							t.Fatalf("oversize: n=%d err=%v, want limit %d", n, err, mode.max)
						}
						nk04OK(t, nk04TCP(f.ctx, c))
						return
					}
					nk04OK(t, err)
					if n != size {
						t.Fatalf("write = %d, want %d", n, size)
					}
					reply := make([]byte, mode.max+1)
					n, addr, err := p.ReadFrom(reply)
					nk04OK(t, err)
					if !bytes.Equal(reply[:n], payload) || addr.String() != dst.String() {
						t.Fatalf("echo size=%d from %v, want size=%d from %v", n, addr, size, dst)
					}
				})
			}
		})
	}
}

// Only the non-0-RTT TUIC boundary fixture uses this adapter's Dial method.
// Embed the unused QUIC TLS methods as nk04TLS does for stream TLS methods.
type nk04UnusedQUICMethods = qtls.Config

type nk04ConfirmedTLS struct {
	*nk04TLS
	nk04UnusedQUICMethods
}

func (c *nk04ConfirmedTLS) Dial(ctx context.Context, socket net.PacketConn, addr net.Addr, config *quic.Config) (quic.Connection, error) {
	confirmed := make(chan struct{})
	config = config.Clone()
	previous := config.Tracer
	config.Tracer = func(ctx context.Context, perspective logging.Perspective, id quic.ConnectionID) *logging.ConnectionTracer {
		gate := &logging.ConnectionTracer{DroppedEncryptionLevel: func(level logging.EncryptionLevel) {
			if level == logging.EncryptionHandshake {
				// In the pinned quic-go, this runs in handleHandshakeConfirmed.
				// The event loop sets handshakeConfirmed before it can pack data.
				close(confirmed)
			}
		}}
		if previous != nil {
			if tracer := previous(ctx, perspective, id); tracer != nil {
				return logging.NewMultiplexedConnectionTracer(tracer, gate)
			}
		}
		return gate
	}
	conn, err := quic.Dial(ctx, socket, addr, c.config, config)
	if err != nil {
		return nil, err
	}
	select {
	case <-confirmed:
		return conn, nil
	case <-conn.Context().Done():
		err = context.Cause(conn.Context())
	case <-ctx.Done():
		err = ctx.Err()
	}
	_ = conn.CloseWithError(0, "boundary handshake confirmation failed")
	return nil, fmt.Errorf("boundary handshake confirmation: %w", err)
}

func nk04ConfirmedTUICClient(t *testing.T, f *nk04Fixture) nk04Client {
	t.Helper()
	c, err := tuic.NewClient(tuic.ClientOptions{Context: f.ctx, Dialer: f.dialer,
		ServerAddress: f.address, TLSConfig: &nk04ConfirmedTLS{nk04TLS: f.clientTLS},
		Password: "secret", UUID: [16]byte{1}, UDPStream: false})
	nk04OK(t, err)
	t.Cleanup(func() { _ = c.CloseWithError(net.ErrClosed) })
	return c
}

func TestNK04MatrixPacketCancelTimeout(t *testing.T) {
	for _, timeout := range []bool{false, true} {
		t.Run(fmt.Sprintf("timeout_%v", timeout), func(t *testing.T) {
			f := nk04Server(t, "tuic", "", false)
			c := f.client(t, "secret", "", "", [16]byte{1}, true)
			nk04OK(t, nk04UDP(f.ctx, c))
			ctx, cancel := context.WithCancel(f.ctx)
			if timeout {
				cancel()
				ctx, cancel = context.WithTimeout(f.ctx, 100*time.Millisecond)
			}
			defer cancel()
			p, err := c.(nk04PacketClient).ListenPacket(ctx)
			nk04OK(t, err)
			defer p.Close()
			read := make(chan error, 1)
			go func() {
				_, _, err := p.ReadFrom(make([]byte, 1))
				read <- err
			}()
			if !timeout {
				cancel()
			}
			select {
			case err := <-read:
				if !errors.Is(err, io.ErrClosedPipe) { // mutation: packet-cancel
					t.Fatalf("canceled packet read = %v, want closed pipe", err)
				}
			case <-f.ctx.Done():
				t.Fatal("packet read did not unblock")
			}
			if timeout && !errors.Is(ctx.Err(), context.DeadlineExceeded) {
				t.Fatalf("context = %v, want deadline exceeded", ctx.Err())
			}
			nk04OK(t, nk04UDP(f.ctx, c))
			nk04OK(t, nk04TCP(f.ctx, c))
		})
	}
}

func nk04RawTUIC(t *testing.T, f *nk04Fixture) quic.Connection {
	t.Helper()
	c, err := quic.Dial(f.ctx, nk04Socket(t, false), f.address.UDPAddr(), f.clientTLS.config, &quic.Config{EnableDatagrams: true})
	nk04OK(t, err)
	t.Cleanup(func() { _ = c.CloseWithError(0, "test cleanup") })
	return c
}

func nk04RequireRemoteRejection(t *testing.T, f *nk04Fixture, c quic.Connection, reason string) {
	t.Helper()
	// Independent of both the 10-second fixture and 60-second malformed-case
	// authentication timer. Neither context cleanup nor idle expiry is rejection.
	timer := time.NewTimer(3 * time.Second)
	defer timer.Stop()
	select {
	case <-c.Context().Done():
		var app *quic.ApplicationError
		if !errors.As(context.Cause(c.Context()), &app) || !app.Remote || app.ErrorCode != 0 || f.ctx.Err() != nil {
			t.Fatalf("peer rejection = %v, fixture = %v, want remote parser application close", context.Cause(c.Context()), f.ctx.Err())
		}
	case <-timer.C:
		t.Fatal("peer was not rejected before independent rejection deadline")
	case <-f.ctx.Done():
		t.Fatal("fixture expired before peer rejection")
	}
	select {
	case message := <-f.parserLog:
		if !strings.Contains(message, reason) {
			t.Fatalf("rejection cause = %q, want %q", message, reason)
		}
	case <-timer.C:
		t.Fatal("no parser rejection cause before deadline")
	case <-f.ctx.Done():
		t.Fatal("fixture expired before parser rejection cause")
	}
}

func TestNK04MatrixAuthTimeoutReconnect(t *testing.T) {
	f := nk04Server(t, "tuic", "", false)
	c := nk04RawTUIC(t, f)
	nk04RequireRemoteRejection(t, f, c, "authentication timeout")
	if f.echo.tcp.Load() != 0 || f.echo.udp.Load() != 0 {
		t.Fatal("unauthenticated peer reached application handlers")
	}
	good := f.client(t, "secret", "", "", [16]byte{1}, true)
	nk04OK(t, nk04UDP(f.ctx, good))
	nk04OK(t, nk04TCP(f.ctx, good))
}

func TestNK04MatrixMalformedReconnect(t *testing.T) {
	for _, test := range []struct {
		name, reason string
		wire         []byte
	}{
		{"invalid_version", "handle uni stream: unknown version 255", []byte{0xff, 0xff}},
		{"unknown_command", "handle uni stream: unknown command 255", []byte{tuic.Version, 0xff}},
		{"truncated_auth", "handle uni stream: authentication: read request", []byte{tuic.Version, tuic.CommandAuthenticate, 1}},
	} {
		t.Run(test.name, func(t *testing.T) {
			// Auth expiry cannot supply a passing close, even at fixture expiry.
			f := nk04ServerWithAuthTimeout(t, "tuic", "", false, time.Minute)
			good := f.client(t, "secret", "", "", [16]byte{1}, true)
			stream, err := good.DialConn(f.ctx, M.ParseSocksaddr("127.0.0.1:12345"))
			nk04OK(t, err)
			defer stream.Close()
			nk04OK(t, nk04Stream(f.ctx, stream))
			packet, err := good.(nk04PacketClient).ListenPacket(f.ctx)
			nk04OK(t, err)
			defer packet.Close()
			stop := context.AfterFunc(f.ctx, func() { _ = packet.Close() })
			defer stop()
			echoPacket := func() {
				payload := []byte("NK04 unchanged UDP session")
				dst := M.ParseSocksaddr("127.0.0.1:12345")
				n, err := packet.WriteTo(payload, dst)
				nk04OK(t, err)
				if n != len(payload) {
					t.Fatalf("packet write = %d, want %d", n, len(payload))
				}
				reply := make([]byte, 128)
				n, addr, err := packet.ReadFrom(reply)
				nk04OK(t, err)
				if !bytes.Equal(reply[:n], payload) || addr.String() != dst.String() {
					t.Fatal("existing packet session changed")
				}
			}
			echoPacket()
			assertHandlers := func(want int64) {
				t.Helper()
				if tcp, udp := f.echo.tcp.Load(), f.echo.udp.Load(); tcp != want || udp != want {
					t.Fatalf("handler calls TCP/UDP = %d/%d, want %d/%d; malformed peer must create no application state or forward data", tcp, udp, want, want)
				}
			}
			assertHandlers(1)
			c := nk04RawTUIC(t, f)
			s, err := c.OpenUniStream()
			nk04OK(t, err)
			_, err = s.Write(test.wire)
			nk04OK(t, err)
			nk04OK(t, s.Close())
			nk04RequireRemoteRejection(t, f, c, test.reason)
			assertHandlers(1)
			// Preserve established TCP/UDP sessions, the listener and credentials.
			// Reusing the exact connections catches state damage masked by reconnect.
			nk04OK(t, nk04Stream(f.ctx, stream))
			echoPacket()
			assertHandlers(1)
			fresh := f.client(t, "secret", "", "", [16]byte{1}, true)
			nk04OK(t, nk04UDP(f.ctx, fresh))
			nk04OK(t, nk04TCP(f.ctx, fresh))
			assertHandlers(2)
		})
	}
}

func TestNK04MatrixUDPStreamPeerCloseReconnect(t *testing.T) {
	for _, remote := range []bool{true, false} {
		name := "peer_close"
		if !remote {
			name = "local_client_close"
		}
		t.Run(name, func(t *testing.T) {
			f := nk04Server(t, "tuic", "", false)
			c := f.client(t, "secret", "", "", [16]byte{1}, true)
			ctx, cancel := context.WithCancel(f.ctx)
			defer cancel()
			p, err := c.(nk04PacketClient).ListenPacket(ctx)
			nk04OK(t, err)
			defer p.Close()
			dst := M.ParseSocksaddr("127.0.0.1:12345")
			payload := []byte("established packet session")
			_, err = p.WriteTo(payload, dst)
			nk04OK(t, err)
			reply := make([]byte, 128)
			n, addr, err := p.ReadFrom(reply)
			nk04OK(t, err)
			if !bytes.Equal(reply[:n], payload) || addr.String() != dst.String() {
				t.Fatal("packet session was not established")
			}
			stream, err := c.DialConn(f.ctx, dst)
			nk04OK(t, err)
			defer stream.Close()
			nk04OK(t, nk04Stream(f.ctx, stream))
			var peer quic.Connection
			select {
			case peer = <-f.accepted:
			case <-f.ctx.Done():
				t.Fatal("no accepted peer")
			}
			if remote {
				nk04OK(t, peer.CloseWithError(0, "NK04 peer close"))
			} else {
				nk04OK(t, c.CloseWithError(io.ErrClosedPipe))
			}
			// First observe the transport close on an established TCP stream.
			_, err = stream.Read(make([]byte, 1))
			if err == nil || f.ctx.Err() != nil {
				t.Fatalf("transport close not observed: %v", err)
			}
			read := make(chan error, 1)
			go func() {
				_, _, err := p.ReadFrom(reply)
				read <- err
			}()
			select {
			case err := <-read:
				if !errors.Is(err, io.ErrClosedPipe) { // mutation: peer-close
					t.Fatalf("closed transport read: %v, want closed pipe", err)
				}
			case <-time.After(time.Second):
				cancel()
				<-read // Join after cancellation before reporting the regression.
				t.Fatal("packet read stayed blocked after observed transport close")
			}
			// Packet closure follows the client's connection-close notification.
			nk04OK(t, nk04UDP(f.ctx, c))
			nk04OK(t, nk04TCP(f.ctx, c))
			select {
			case next := <-f.accepted:
				if next == peer {
					t.Fatal("reused closed peer")
				}
			case <-f.ctx.Done():
				t.Fatal("no new peer after reconnect")
			}
		})
	}
}
