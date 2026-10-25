package main

import (
	"context"
	"os"
	"testing"

	"grpc_server/gen"

	"github.com/matsuridayo/libneko/neko_common"
	"github.com/matsuridayo/libneko/neko_log"
	box "github.com/sagernet/sing-box"
)

func TestStartWithLogDisabled(t *testing.T) {
	// This test owns process-global state and must remain sequential.
	oldInstance := currentInstance()
	if oldInstance != nil {
		t.Fatal("test requires no running instance")
	}
	oldCancel := instance_cancel
	oldMode := neko_common.RunMode
	oldConfigureLog := configureInstanceLog
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
		out, err := s.Stop(ctx, &gen.EmptyReq{})
		stateMu.Lock()
		instance, instance_cancel = oldInstance, oldCancel
		stateMu.Unlock()
		configureInstanceLog = oldConfigureLog
		neko_common.RunMode = oldMode
		if err := os.Chdir(oldDir); err != nil {
			t.Errorf("restore working directory: %v", err)
		}
		n01RequireReply(t, out, err, "")
	})

	neko_common.RunMode = neko_common.RunMode_NekoBox_Core
	setupCore()
	// TestMain normally bypasses this setter for the state tests. Exercise
	// the production logger operation here to catch the disabled-log panic.
	configureInstanceLog = func(i *box.Box) {
		i.SetLogWritter(neko_log.LogWriter)
	}

	// An unexpected panic fails the test; no network listeners are needed.
	out, err := s.Start(ctx, &gen.LoadConfigReq{
		CoreConfig: `{"log":{"disabled":true},"outbounds":[{"type":"direct","tag":"direct"}]}`,
	})
	n01RequireReply(t, out, err, "")
	if currentInstance() == nil {
		t.Fatal("successful Start did not install an instance")
	}

	out, err = s.Stop(ctx, &gen.EmptyReq{})
	n01RequireReply(t, out, err, "")
	if currentInstance() != nil {
		t.Fatal("Stop retained the instance")
	}
}
