# NK04 final-pin acceptance

`nk04-security.yml` requires all eight rows to pass: default, `debug`,
`with_low_memory`, and both tags, each with and without `-race`. Every row runs
in `go/cmd/nekobox_core` with its checked-in replacements. It uses Go 1.22.12,
sing 0.4.3, sing-box `2f78dbe673e2`, libneko `45e350171b7b`, sing-quic
`201474163bfe`, and quic-go 0.47.0-beta.2 at `7101329b86a0`, with the existing
congestion-lock patch verified by reverse application. No manifest is rewritten.

Each row requires exactly **21 top-level tests, 49 test nodes, and 41 leaves**.
The existing 15 in-process tests cover protocol/authentication/obfuscation,
TCP-only mode, concurrent UDP, reconnect, and real Hysteria2 port hopping.
The hopping relay binds ephemeral IPv4 loopback sockets and observes a changed
client source after the production five-second hop interval, then verifies TCP
and UDP traffic. The new tests add:

| Test | Acceptance |
| --- | --- |
| MatrixStreamLimit | Admit 1,024 live TUIC streams, reject stream 1,025 with StreamLimitReachedError, and exchange data through the first and last admitted streams. |
| MatrixPacketBoundaries | Hysteria2 (4,096) and TUIC datagram/stream (65,535): zero, one, maximum minus one, maximum, and maximum plus one. Accepted packets echo bytes and destination; overflow returns the exact typed limit and leaves TCP usable. Each size owns an independent fixture/context. |
| MatrixPacketCancelTimeout | A TUIC UDP-stream read returns closed pipe on explicit cancellation or deadline; subsequent UDP/TCP traffic succeeds. |
| MatrixAuthTimeoutReconnect | A peer sending no authentication is remotely closed for authentication timeout; no application handler is called, and a new valid client exchanges UDP/TCP on the same listener. |
| MatrixMalformedReconnect | Invalid version, unknown command, and truncated authentication each require remote application close and the corresponding parser error before an independent three-second deadline. No application state is created or data forwarded; established TCP/UDP sessions and fresh-client authentication remain usable. |
| MatrixUDPStreamPeerCloseReconnect | Establish packet and TCP sessions, observe remote or local transport close on TCP, require the packet read to unblock without independent context cancellation, then reconnect and verify a distinct accepted peer and UDP/TCP traffic. |

The packet echo fixture unwraps the timer wrapper through sing's public
`common.Cast` to use the transport's datagram API. This avoids the buffer API's
empty-buffer EOF semantics and the default pooled buffer's smaller capacity.
It retains the wrapper/context cleanup. It sends no external traffic.

The three malformed-input leaves use a **60-second authentication timeout**,
longer than both their three-second rejection deadline and ten-second fixture
context. Authentication expiry cannot satisfy these leaves. Fixture expiry,
local close, transport/idle timeout, a different parser error, and a silent drop
all fail. The fixture records the production parser's rejection cause and counts
TCP/UDP handler entries before any forwarding. Each malformed peer must add
zero handler calls. The exact established TCP connection and UDP session echo
again after rejection without creating replacement handlers; a fresh valid
client then proves the listener and original credentials still work. These are
checks of observable application/session state, not private implementation
fields. Panics, races and hangs remain failures under the runner's process and
test deadlines.

Mutation validation must change parser behavior, not invert a test assertion:
in a disposable copy of pinned sing-quic, replace each `handleUniStream` error
return for unknown version, unknown command and truncated authentication with
`return nil`, one at a time and together. Point only an untracked probe modfile
at that copy. Each corresponding `MatrixMalformedReconnect` leaf must fail at
the rejection deadline; the combined mutant must fail all three. Restore the
copy afterward and retain the exact commands, source diff, module graph and
raw output. The required matrix itself always uses the unchanged final pins.

All sockets and destinations are loopback; credentials and ephemeral TLS keys
exist only inside the test process. Dependency downloads precede execution;
the runner sets GOPROXY=off and GOSUMDB=off. It clears inherited Go/runtime/race
controls and saved GOENV settings. Each row compiles a new binary, records its
actual exit status and raw output, then converts the completed output with
`test2json`. Exact run/pass inventory, lifecycle, package completion, lossless
conversion (excluding Go's NAME control records), and raw failure diagnostics
must all agree. Build failures, signals, timeouts, missing/extra/duplicate tests,
skips, assertion failures, panics, and race reports fail acceptance. Rows are
never retried automatically. Process-group deadlines bound compilation and tests.
`test_nk04_matrix.py` challenges the result contract with missing/extra tests,
false success, duplicates, reordered lifecycles, failures, and race diagnostics.

There are **no XFAILs in the final-pin core rows**. The owner-waived congestion
race belongs to unpatched standalone quic-go 0.46, whereas these rows verify
patched 0.47 and sing 0.4.3. The module graph is saved in each output directory;
a race here is a failure. macOS wildcard/loopback SAME_PORT conflicts may be
classified only with direct socket ownership evidence for that invocation;
this fixture binds IPv4 loopback explicitly and contains no broad socket-error
allowance. Any unexplained setup failure is red. Historical standalone and
macOS environmental evidence belongs in the implementation evidence ledger.

Existing fixed external-peer tests remain gated separately by `external-interop`
in `tests.yml`, using the pinned container digests there. They are not part of
these hermetic in-process rows and are not counted as passes in this inventory.

Run from the repository root after `bash libs/get_source.sh`. Set absolute,
fresh GOPATH, GOCACHE and GOMODCACHE directories beneath your work directory.
Download dependencies with GOTOOLCHAIN=local, GOWORK=off and GOENV=off:

```sh
# Use an absolute NK04_DOWNLOAD_MOD path outside the tracked source files.
(cd go/cmd/nekobox_core && cp go.mod "$NK04_DOWNLOAD_MOD" &&
 cp go.sum "${NK04_DOWNLOAD_MOD%.mod}.sum" &&
 go mod download -modfile="$NK04_DOWNLOAD_MOD" all)
python3 -m unittest discover -s tests -p test_nk04_matrix.py -v
python3 tests/run_nk04_matrix.py --go /absolute/path/to/go1.22.12 --output /absolute/evidence/path
```

Use `--row debug_low_memory_race` for one required row. On macOS the runner
uses external linking for Go 1.22 loader compatibility; race checking stays on.
Retain all raw, JSON, build, module, and result files for review.
