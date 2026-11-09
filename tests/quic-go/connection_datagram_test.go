package quic

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"math/big"
	"testing"
	"time"

	"github.com/sagernet/quic-go/internal/protocol"
	"github.com/sagernet/quic-go/internal/wire"
)

func TestSendDatagramPayloadBounds(t *testing.T) {
	for _, tc := range []struct {
		peer, path, want int
	}{
		{2, 1200, 0},
		{3, 1200, 1},
		{63, 1200, 61},
		{64, 1200, 62},
		{65, 1200, 63},
		{66, 1200, 63}, // A two-byte length field would exceed the frame limit.
		{67, 1200, 64},
		{16383, 32, 32},
		{64, 0, 0},
		{64, 31, 31},
		{64, 62, 62},
	} {
		t.Run(fmt.Sprintf("peer=%d/path=%d", tc.peer, tc.path), func(t *testing.T) {
			for _, size := range []int{0, tc.want, tc.want + 1, 255, 4096} {
				t.Run(fmt.Sprintf("size=%d", size), func(t *testing.T) {
					c := &connection{
						peerParams:    &wire.TransportParameters{MaxDatagramFrameSize: protocol.ByteCount(tc.peer)},
						version:       protocol.Version1,
						datagramQueue: newDatagramQueue(func() {}, nil),
					}
					c.maxPayloadSizeEstimate.Store(uint32(tc.path))
					payload := bytes.Repeat([]byte{0x42}, size)
					err := c.SendDatagram(payload)
					if size > tc.want {
						var tooLarge *DatagramTooLargeError
						if !errors.As(err, &tooLarge) || tooLarge.MaxDatagramPayloadSize != int64(tc.want) {
							t.Fatalf("SendDatagram(%d) = %v; want DatagramTooLargeError with maximum %d", size, err, tc.want)
						}
						if c.datagramQueue.Peek() != nil {
							t.Fatal("oversize datagram enqueued")
						}
						return
					}
					if err != nil {
						t.Fatal(err)
					}
					frame := c.datagramQueue.Peek()
					if frame == nil || !bytes.Equal(frame.Data, payload) || frame.Length(c.version) > protocol.ByteCount(tc.peer) {
						t.Fatal("queued frame violates payload or peer bound")
					}
					if size > 0 {
						payload[0] ^= 0xff
						if frame.Data[0] != 0x42 {
							t.Fatal("queued payload aliases caller memory")
						}
					}
				})
			}
		})
	}
}

func TestSendDatagramLowPeerLimitRoundTrip(t *testing.T) {
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	template := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(time.Hour),
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, public, private)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	listener, err := ListenAddr("127.0.0.1:0", &tls.Config{
		Certificates: []tls.Certificate{{Certificate: [][]byte{der}, PrivateKey: private}},
		NextProtos:   []string{"datagram-limit-test"},
	}, &Config{EnableDatagrams: true, MaxDatagramFrameSize: 64})
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	client, err := DialAddr(ctx, listener.Addr().String(), &tls.Config{
		InsecureSkipVerify: true, // Ephemeral certificate on loopback only.
		NextProtos:         []string{"datagram-limit-test"},
	}, &Config{EnableDatagrams: true, MaxDatagramFrameSize: 64})
	if err != nil {
		t.Fatal(err)
	}
	defer client.CloseWithError(0, "test complete")
	server, err := listener.Accept(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer server.CloseWithError(0, "test complete")
	for _, pair := range []struct {
		name             string
		sender, receiver Connection
	}{
		{"client", client, server},
		{"server", server, client},
	} {
		t.Run(pair.name, func(t *testing.T) {
			for _, size := range []int{63, 255, 4096} {
				var tooLarge *DatagramTooLargeError
				err := pair.sender.SendDatagram(make([]byte, size))
				if !errors.As(err, &tooLarge) || tooLarge.MaxDatagramPayloadSize != 62 {
					t.Fatalf("SendDatagram(%d) = %v; want maximum 62", size, err)
				}
				payload := bytes.Repeat([]byte{0x42}, 62)
				if err := pair.sender.SendDatagram(payload); err != nil {
					t.Fatal(err)
				}
				got, err := pair.receiver.ReceiveDatagram(ctx)
				if err != nil || !bytes.Equal(got, payload) {
					t.Fatalf("datagram after rejected send: payload=%x err=%v", got, err)
				}
				if client.Context().Err() != nil || server.Context().Err() != nil {
					t.Fatal("connection closed after rejected send")
				}
			}
		})
	}
}
