package main

import (
	"context"
	"net"
	"net/http"
	"sync"

	"github.com/matsuridayo/libneko/neko_common"
	"github.com/matsuridayo/libneko/neko_log"
	box "github.com/sagernet/sing-box"
	"github.com/sagernet/sing-box/boxapi"
	boxmain "github.com/sagernet/sing-box/cmd/sing-box"
)

// startStopMu serializes the complete instance lifecycle. stateMu protects only
// the published fields; release it before calling into the core.
var startStopMu sync.Mutex
var stateMu sync.RWMutex

var instance *box.Box
var instance_cancel context.CancelFunc

func currentInstance() *box.Box {
	stateMu.RLock()
	defer stateMu.RUnlock()
	return instance
}

// configureInstanceLog is separate so state tests can isolate the legacy
// sing-box logger setter, which mutates a field read by background loggers.
var configureInstanceLog = func(i *box.Box) {
	i.SetLogWritter(neko_log.LogWriter)
}

func setupCore() {
	boxmain.SetDisableColor(true)
	//
	neko_log.SetupLog(50*1024, "./neko.log")
	//
	neko_common.GetCurrentInstance = func() interface{} {
		return currentInstance()
	}
	neko_common.DialContext = func(ctx context.Context, specifiedInstance interface{}, network, addr string) (net.Conn, error) {
		if i, ok := specifiedInstance.(*box.Box); ok {
			return boxapi.DialContext(ctx, i, network, addr)
		}
		if i := currentInstance(); i != nil {
			return boxapi.DialContext(ctx, i, network, addr)
		}
		return neko_common.DialContextSystem(ctx, network, addr)
	}
	neko_common.DialUDP = func(ctx context.Context, specifiedInstance interface{}) (net.PacketConn, error) {
		if i, ok := specifiedInstance.(*box.Box); ok {
			return boxapi.DialUDP(ctx, i)
		}
		if i := currentInstance(); i != nil {
			return boxapi.DialUDP(ctx, i)
		}
		return neko_common.DialUDPSystem(ctx)
	}
	neko_common.CreateProxyHttpClient = func(specifiedInstance interface{}) *http.Client {
		if i, ok := specifiedInstance.(*box.Box); ok {
			return boxapi.CreateProxyHttpClient(i)
		}
		i := currentInstance()
		return boxapi.CreateProxyHttpClient(i)
	}
}
