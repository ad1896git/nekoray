package quic

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"io"
	"math/big"
	"sync/atomic"
	"testing"
	"time"

	"github.com/sagernet/quic-go/logging"
)

type datagramSessionCache struct {
	tls.ClientSessionCache
	ready chan struct{}
}

func (c *datagramSessionCache) Put(k string, s *tls.ClientSessionState) {
	c.ClientSessionCache.Put(k, s)
	select {
	case c.ready <- struct{}{}:
	default:
	}
}

func TestSendDatagramRejectedEarlyQueue(t *testing.T) {
	t.Run("oversize255", func(t *testing.T) { rejectedEarlyQueue(t, 255) })
	t.Run("fits62", func(t *testing.T) { rejectedEarlyQueue(t, 62) })
}

func rejectedEarlyQueue(t *testing.T, payloadSize int) {
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	pub, key, e := ed25519.GenerateKey(rand.Reader)
	if e != nil {
		t.Fatal(e)
	}
	cert := &x509.Certificate{SerialNumber: big.NewInt(2), NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(time.Hour)}
	der, e := x509.CreateCertificate(rand.Reader, cert, cert, pub, key)
	if e != nil {
		t.Fatal(e)
	}
	serverTLS := &tls.Config{Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: key}}, NextProtos: []string{"datagram-resumption"}}
	cache := &datagramSessionCache{ClientSessionCache: tls.NewLRUClientSessionCache(4), ready: make(chan struct{}, 10)}
	clientTLS := &tls.Config{InsecureSkipVerify: true, ServerName: "localhost", NextProtos: []string{"datagram-resumption"}, ClientSessionCache: cache}
	var peerLimit atomic.Int64
	peerLimit.Store(16383)
	cfg := &Config{EnableDatagrams: true, Allow0RTT: true}
	cfg.GetConfigForClient = func(*ClientHelloInfo) (*Config, error) {
		return &Config{EnableDatagrams: true, Allow0RTT: true, MaxDatagramFrameSize: peerLimit.Load()}, nil
	}
	l, e := ListenAddr("127.0.0.1:0", serverTLS, cfg)
	if e != nil {
		t.Fatal(e)
	}
	defer l.Close()
	first, e := DialAddr(ctx, l.Addr().String(), clientTLS, &Config{EnableDatagrams: true})
	if e != nil {
		t.Fatal(e)
	}
	firstServer, e := l.Accept(ctx)
	if e != nil {
		t.Fatal(e)
	}
	select {
	case <-cache.ready:
	case <-ctx.Done():
		t.Fatal("no session ticket")
	}
	first.CloseWithError(0, "resume")
	firstServer.CloseWithError(0, "resume")
	peerLimit.Store(64)
	// Gate the TP callback immediately before the restored params are replaced.
	// An independent application goroutine uses public SendDatagram while the
	// network goroutine is paused. No production code or params are modified.
	ready := make(chan struct{})
	requestSend := make(chan struct{})
	sendResult := make(chan error)
	var second EarlyConnection
	var sentErr error
	var oldLimit int64
	var restored bool
	clientCfg := &Config{EnableDatagrams: true, Tracer: func(context.Context, logging.Perspective, ConnectionID) *logging.ConnectionTracer {
		return &logging.ConnectionTracer{ReceivedTransportParameters: func(*logging.TransportParameters) {
			<-ready
			c := second.(*connection)
			restored = c.peerParams != nil
			if restored {
				oldLimit = int64(c.peerParams.MaxDatagramFrameSize)
			}
			requestSend <- struct{}{}
			sentErr = <-sendResult
		}}
	}}
	second, e = DialAddrEarly(ctx, l.Addr().String(), clientTLS, clientCfg)
	if e != nil {
		close(ready)
		t.Fatal(e)
	}
	go func() {
		<-requestSend
		sendResult <- func() error {
			if err := second.SendDatagram(make([]byte, payloadSize)); err != nil {
				return err
			}
			return second.SendDatagram(bytes.Repeat([]byte{0x42}, 62))
		}()
	}()
	close(ready)
	defer second.CloseWithError(0, "done")
	server, e := l.Accept(ctx)
	if e != nil {
		t.Fatal(e)
	}
	defer server.CloseWithError(0, "done")
	select {
	case <-second.HandshakeComplete():
	case <-ctx.Done():
		t.Fatal(ctx.Err())
	}
	t.Logf("restored=%t oldLimit=%d sendErr=%v used0RTT=%t", restored, oldLimit, sentErr, second.ConnectionState().Used0RTT)
	if !restored || oldLimit != 16383 || second.ConnectionState().Used0RTT {
		t.Fatal("did not exercise rejected resumption")
	}
	if sentErr != nil {
		t.Fatalf("failed to queue using restored limit: %v", sentErr)
	}
	next, e := second.NextConnection(ctx)
	if e != nil {
		t.Fatal(e)
	}
	stream, e := next.OpenUniStreamSync(ctx)
	if e != nil {
		t.Fatal(e)
	}
	stream.SetWriteDeadline(time.Now().Add(time.Second))
	if _, e = stream.Write([]byte("survive")); e != nil {
		t.Fatal(e)
	}
	stream.Close()
	rs, e := server.AcceptUniStream(ctx)
	if e != nil {
		t.Fatal(e)
	}
	rs.SetReadDeadline(time.Now().Add(time.Second))
	b, e := io.ReadAll(rs)
	if e != nil || string(b) != "survive" {
		t.Fatalf("stream %q %v", b, e)
	}
	// Verify that dropping an oversize head preserves a valid frame behind it.
	if payloadSize == 62 {
		data, err := server.ReceiveDatagram(ctx)
		if err != nil || !bytes.Equal(data, make([]byte, 62)) {
			t.Fatalf("first valid datagram %x: %v", data, err)
		}
	}
	data, err := server.ReceiveDatagram(ctx)
	if err != nil || !bytes.Equal(data, bytes.Repeat([]byte{0x42}, 62)) {
		t.Fatalf("valid queued tail %x: %v", data, err)
	}
	short, cancelShort := context.WithTimeout(ctx, 100*time.Millisecond)
	defer cancelShort()
	data, err = server.ReceiveDatagram(short)
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("unexpected extra datagram %x: %v", data, err)
	}
	if server.Context().Err() != nil || next.Context().Err() != nil {
		t.Fatalf("connection died after rejected early data: recv=%v server=%v client=%v", err, context.Cause(server.Context()), context.Cause(next.Context()))
	}
}
