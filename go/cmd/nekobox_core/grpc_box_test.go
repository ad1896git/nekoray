package main

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"testing"
	"time"

	"grpc_server/gen"

	"github.com/matsuridayo/libneko/neko_common"
)

func TestN01StartStop(t *testing.T) {
	// These tests exercise process-global core state and must remain sequential.
	if instance != nil {
		t.Fatal("test requires no running instance")
	}

	oldCancel := instance_cancel
	oldMode := neko_common.RunMode
	oldDir, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}

	testDir := t.TempDir()
	if err := os.Chdir(testDir); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		instance = nil
		instance_cancel = oldCancel
		neko_common.RunMode = oldMode
		if err := os.Chdir(oldDir); err != nil {
			t.Errorf("restore working directory: %v", err)
		}
	})

	neko_common.RunMode = neko_common.RunMode_NekoBox_Core
	setupCore()

	for _, duplicate := range []bool{false, true} {
		name := "normal"
		if duplicate {
			name = "duplicate"
		}
		t.Run(name, func(t *testing.T) {
			if instance != nil {
				t.Fatal("previous case left a running instance")
			}

			s := &server{}
			ctx := context.Background()

			reply, rpcErr := s.Stop(ctx, &gen.EmptyReq{})
			n01RequireReply(t, reply, rpcErr, "")

			origin := httptest.NewServer(http.HandlerFunc(
				func(w http.ResponseWriter, r *http.Request) {
					if r.URL.Path != "/hello.txt" {
						http.NotFound(w, r)
						return
					}
					_, _ = io.WriteString(w, "hello-from-test")
				},
			))
			defer origin.Close()

			port := n01AvailablePort(t)
			address := fmt.Sprintf("127.0.0.1:%d", port)
			config := fmt.Sprintf(
				`{"log":{"level":"warn"},"inbounds":[{"type":"mixed","tag":"in","listen":"127.0.0.1","listen_port":%d}],"outbounds":[{"type":"direct","tag":"direct"}]}`,
				port,
			)

			transport := &http.Transport{
				Proxy: http.ProxyURL(&url.URL{
					Scheme: "socks5",
					Host:   address,
				}),
				DisableKeepAlives: true,
			}
			defer transport.CloseIdleConnections()
			client := &http.Client{
				Transport: transport,
				Timeout:   5 * time.Second,
			}

			// The second cycle proves that the same listener address can
			// be started again after Stop and an invalid configuration.
			for cycle := 0; cycle < 2; cycle++ {
				reply, rpcErr = s.Start(ctx, &gen.LoadConfigReq{
					CoreConfig: config,
				})
				n01RequireReply(t, reply, rpcErr, "")

				owned := instance
				cancel := instance_cancel
				if owned == nil || cancel == nil {
					t.Fatal("successful Start did not install instance and cancel")
				}

				closed := false
				t.Cleanup(func() {
					// Keep the original reference so the test also cleans
					// up when the regression clears the global reference.
					if !closed {
						cancel()
						if err := owned.Close(); err != nil {
							t.Logf("cleanup Close: %v", err)
						}
					}
					if instance == owned {
						instance = nil
					}
				})

				cancelCalls := 0
				instance_cancel = func() {
					cancelCalls++
					cancel()
				}

				n01RequireTraffic(t, client, origin.URL+"/hello.txt")

				if duplicate {
					// Both valid and invalid duplicate requests must be
					// rejected before attempting to create a new instance.
					for _, duplicateConfig := range []string{
						config,
						"{not json",
					} {
						reply, rpcErr = s.Start(ctx, &gen.LoadConfigReq{
							CoreConfig: duplicateConfig,
						})
						n01RequireReply(
							t, reply, rpcErr, "instance already started",
						)
						if instance != owned {
							t.Fatal("duplicate Start replaced or cleared the running instance")
						}
						if cancelCalls != 0 {
							t.Fatal("duplicate Start canceled the running instance")
						}
						n01RequireTraffic(t, client, origin.URL+"/hello.txt")
					}
				}

				reply, rpcErr = s.Stop(ctx, &gen.EmptyReq{})
				n01RequireReply(t, reply, rpcErr, "")
				if instance != nil {
					t.Fatal("Stop retained the instance")
				}
				if cancelCalls != 1 {
					t.Fatalf("Stop cancellation calls = %d, want 1", cancelCalls)
				}

				n01RequirePortReleased(t, address)
				closed = true

				reply, rpcErr = s.Stop(ctx, &gen.EmptyReq{})
				n01RequireReply(t, reply, rpcErr, "")
				if cancelCalls != 1 {
					t.Fatal("idle Stop canceled the previous instance again")
				}

				reply, rpcErr = s.Start(ctx, &gen.LoadConfigReq{
					CoreConfig: "{not json",
				})
				if rpcErr != nil || reply == nil || reply.Error == "" {
					t.Fatalf(
						"invalid Start: reply=%v transport error=%v",
						reply, rpcErr,
					)
				}
				if instance != nil {
					t.Fatal("invalid Start installed an instance")
				}

				connections, err := s.ListConnections(ctx, &gen.EmptyReq{})
				if err != nil || connections == nil {
					t.Fatalf(
						"server unavailable after invalid Start: reply=%v error=%v",
						connections, err,
					)
				}
			}
		})
	}
}

func n01RequireReply(
	t *testing.T,
	reply *gen.ErrorResp,
	rpcErr error,
	want string,
) {
	t.Helper()
	if rpcErr != nil {
		t.Fatalf("unexpected transport error: %v", rpcErr)
	}
	if reply == nil {
		t.Fatal("nil response")
	}
	if reply.Error != want {
		t.Fatalf("response error = %q, want %q", reply.Error, want)
	}
}

func n01AvailablePort(t *testing.T) int {
	t.Helper()
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	return port
}

func n01RequireTraffic(t *testing.T, client *http.Client, target string) {
	t.Helper()
	response, err := client.Get(target)
	if err != nil {
		t.Fatalf("HTTP through SOCKS5: %v", err)
	}
	defer response.Body.Close()

	body, err := io.ReadAll(response.Body)
	if err != nil {
		t.Fatalf("read proxied response: %v", err)
	}
	if response.StatusCode != http.StatusOK || string(body) != "hello-from-test" {
		t.Fatalf(
			"proxied response: status=%d body=%q",
			response.StatusCode, body,
		)
	}
}

func n01RequirePortReleased(t *testing.T, address string) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for {
		connection, err := net.DialTimeout("tcp4", address, 100*time.Millisecond)
		if err != nil {
			break
		}
		_ = connection.Close()
		if time.Now().After(deadline) {
			t.Fatalf("listener still accepts connections after Stop: %s", address)
		}
		time.Sleep(20 * time.Millisecond)
	}

	// A failed connection alone is insufficient: prove the address can
	// actually be bound by a new listener.
	listener, err := net.Listen("tcp4", address)
	if err != nil {
		t.Fatalf("cannot rebind address after Stop: %v", err)
	}
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
}
