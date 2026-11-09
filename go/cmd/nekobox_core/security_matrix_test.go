package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"testing"
	"time"

	"github.com/sagernet/quic-go"
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
					c := f.client(t, "secret", "", "", [16]byte{1}, mode.stream)
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

func TestNK04MatrixMalformedTimeoutReconnect(t *testing.T) {
	for _, wire := range [][]byte{nil, {0xff, 0xff}, {tuic.Version, 0xff}, {tuic.Version, tuic.CommandAuthenticate, 1}} {
		t.Run(fmt.Sprintf("wire_%x", wire), func(t *testing.T) {
			f := nk04Server(t, "tuic", "", false)
			c := nk04RawTUIC(t, f)
			if wire != nil {
				s, err := c.OpenUniStream()
				nk04OK(t, err)
				_, err = s.Write(wire)
				nk04OK(t, err)
				nk04OK(t, s.Close())
			}
			select {
			case <-c.Context().Done():
				var app *quic.ApplicationError
				if !errors.As(context.Cause(c.Context()), &app) || !app.Remote { // mutation: malformed
					t.Fatalf("peer rejection = %v, want remote application close", context.Cause(c.Context()))
				}
			case <-f.ctx.Done():
				t.Fatal("malformed/unauthenticated peer was not closed")
			}
			good := f.client(t, "secret", "", "", [16]byte{1}, true)
			nk04OK(t, nk04UDP(f.ctx, good))
			nk04OK(t, nk04TCP(f.ctx, good))
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
