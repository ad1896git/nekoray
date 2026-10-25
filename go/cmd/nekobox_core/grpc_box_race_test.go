package main

import (
	"context"
	"fmt"
	"math/rand"
	"net/http"
	"net/http/httptest"
	"os"
	"sync"
	"testing"
	"time"

	"grpc_server/gen"

	"github.com/matsuridayo/libneko/neko_common"
	box "github.com/sagernet/sing-box"
)

// The pinned sing-box SetLogWritter mutates its logger after Create has
// launched background goroutines. Its race with the Darwin network monitor
// also occurs in the unmodified, sequential TestN01StartStop. Isolate that
// unrelated logger operation for this package's state tests; keep real box
// creation, startup, cancellation, Close, and all handlers under the detector.
// This deliberately skips SetLogWritter for every package test, including
// TestN01StartStop; its source and lifecycle assertions remain unchanged.
// NEKOBOX_TEST_LEGACY_LOGGER=1 restores the setter to reproduce that separate
// dependency race. Production always uses the real setter.
func TestMain(m *testing.M) {
	oldConfigureLog := configureInstanceLog
	if os.Getenv("NEKOBOX_TEST_LEGACY_LOGGER") != "1" {
		configureInstanceLog = func(*box.Box) {}
	}
	code := m.Run()
	configureInstanceLog = oldConfigureLog
	os.Exit(code)
}

func TestConcurrentStartStopQuery(t *testing.T) {
	// Like TestN01StartStop, this test owns process-global state sequentially.
	if currentInstance() != nil {
		t.Fatal("test requires no running instance")
	}
	oldCancel := instance_cancel
	oldMode := neko_common.RunMode
	oldDir, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Chdir(t.TempDir()); err != nil {
		t.Fatal(err)
	}
	s := &server{}
	ctx := context.Background()
	t.Cleanup(func() {
		reply, err := s.Stop(ctx, &gen.EmptyReq{})
		n01RequireReply(t, reply, err, "")
		stateMu.Lock()
		instance = nil
		instance_cancel = oldCancel
		stateMu.Unlock()
		neko_common.RunMode = oldMode
		if err := os.Chdir(oldDir); err != nil {
			t.Errorf("restore working directory: %v", err)
		}
	})
	neko_common.RunMode = neko_common.RunMode_NekoBox_Core
	setupCore()

	// All traffic stays on loopback. The shared listener address is exclusive:
	// overlapping successful starts cannot both own it, and final Stop must
	// release it. This also exposes orphaned listeners after a lost instance.
	port := n01AvailablePort(t)
	address := fmt.Sprintf("127.0.0.1:%d", port)
	config := fmt.Sprintf(`{"log":{"level":"error"},"inbounds":[{"type":"mixed","tag":"in","listen":"127.0.0.1","listen_port":%d}],"outbounds":[{"type":"direct","tag":"direct"}]}`, port)
	origin := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))
	defer origin.Close()

	observe := func() error {
		// This checks the published fields between lifecycle transitions. It
		// does not independently count boxes while Create or Close is running;
		// the exclusive listener and final rebind provide the resource checks.
		// Do not replace instance_cancel or otherwise mutate the observed state.
		startStopMu.Lock()
		defer startStopMu.Unlock()
		i := currentInstance()
		stateMu.RLock()
		consistent := i == instance && (i == nil || instance_cancel != nil)
		stateMu.RUnlock()
		if !consistent {
			return fmt.Errorf("inconsistent instance snapshot or missing cancellation")
		}
		return nil
	}

	const workers = 8
	results := make(chan error, workers)
	var wg sync.WaitGroup
	deadline := time.Now().Add(2 * time.Second)
	for worker := 0; worker < workers; worker++ {
		wg.Add(1)
		go func(worker int) {
			defer wg.Done()
			var failure error
			defer func() {
				if p := recover(); p != nil {
					failure = fmt.Errorf("worker %d panicked: %v", worker, p)
				}
				results <- failure
			}()
			rng := rand.New(rand.NewSource(int64(worker)))
			for step := worker; time.Now().Before(deadline); step++ {
				switch step % 4 {
				case 0:
					reply, err := s.Start(ctx, &gen.LoadConfigReq{CoreConfig: config, StatsOutbounds: []string{"direct"}})
					if err != nil || reply == nil || (reply.Error != "" && reply.Error != "instance already started") {
						failure = fmt.Errorf("Start: reply=%v error=%v", reply, err)
					}
				case 1:
					reply, err := s.Stop(ctx, &gen.EmptyReq{})
					if err != nil || reply == nil || reply.Error != "" {
						failure = fmt.Errorf("Stop: reply=%v error=%v", reply, err)
					}
				case 2:
					reply, err := s.QueryStats(ctx, &gen.QueryStatsReq{Tag: "direct", Direct: "uplink"})
					if err != nil || reply == nil || reply.Traffic < 0 {
						failure = fmt.Errorf("QueryStats: reply=%v error=%v", reply, err)
					}
				case 3:
					// A real loopback request exercises routing while Stop can close the box.
					reply, err := s.Test(ctx, &gen.TestReq{Mode: gen.TestMode_UrlTest, Url: origin.URL, Timeout: 50})
					if err != nil || reply == nil {
						failure = fmt.Errorf("Test: reply=%v error=%v", reply, err)
					}
				}
				if failure == nil {
					failure = observe()
				}
				if failure != nil {
					return
				}
				time.Sleep(time.Duration(1+rng.Intn(4)) * time.Millisecond)
			}
		}(worker)
	}
	wg.Wait()
	close(results)
	for err := range results {
		if err != nil {
			t.Error(err)
		}
	}
	reply, err := s.Stop(ctx, &gen.EmptyReq{})
	n01RequireReply(t, reply, err, "")
	if currentInstance() != nil {
		t.Error("final Stop retained an instance")
	}
	n01RequirePortReleased(t, address)
	stats, err := s.QueryStats(ctx, &gen.QueryStatsReq{Tag: "direct", Direct: "uplink"})
	if err != nil || stats == nil || stats.Traffic != 0 {
		t.Errorf("idle QueryStats: reply=%v error=%v", stats, err)
	}
	probe, err := s.Test(ctx, &gen.TestReq{Mode: gen.TestMode_UrlTest, Url: origin.URL, Timeout: 50})
	if err != nil || probe == nil || probe.Ms != 0 || probe.Error != "" {
		t.Errorf("idle Test: reply=%v error=%v", probe, err)
	}
}

func TestFullTestCreationError(t *testing.T) {
	full, err := (&server{}).Test(context.Background(), &gen.TestReq{
		Mode:   gen.TestMode_FullTest,
		Config: &gen.LoadConfigReq{CoreConfig: "{not json"},
	})
	if err != nil || full == nil || full.Error == "" {
		t.Fatalf("invalid FullTest: reply=%v transport error=%v", full, err)
	}
}
