# Known transport limitations

## KI-14: TUIC DATAGRAM loss during QUIC handshake packet coalescing

- **Where:** the core's pinned `sagernet/quic-go` v0.47.0-beta.2
  (`7101329b86a0`), with the existing carried patch, through sing-quic
  `201474163bfe`. The carried patch does not fix this discard branch.
- **Trigger and root cause:** a queued 1,197-byte TUIC fragment occupies a
  1,200-byte QUIC DATAGRAM frame. When the handshake-coalesced packet has only
  1,150 bytes left and contains no ACK, `packetPacker.composeNextPacket` pops
  the frame without transmitting it or notifying the application. The peer's
  16,383-byte limit is not exceeded. A 65,534- or 65,535-byte message then lacks
  fragment 0 of 56 and cannot complete reassembly. Other fragmented sizes can
  also encounter this packet-space condition; it is not a size-specific limit.
- **Platforms and conditions:** reproduced on Linux, including `default_race`,
  `debug_race`, and `debug_low_memory_race`; low-memory CI failures have also
  been reported. The code path is shared across platforms. Earlier macOS
  65,534-byte repetitions passed 200/200 in each of eight tag/race rows; this
  does not establish that macOS or other platforms are immune.
- **User impact:** a UDP datagram can be lost despite successful submission.
  The application must tolerate loss or provide its own retransmission;
  TUIC/QUIC DATAGRAM does not retransmit it. The observed failures had no crash
  or data corruption: incomplete fragment sets are not delivered. A separate
  deterministic fragment-0 loss probe passed 200 times with `-race` at both
  large sizes, checking exact application retransmission and subsequent data
  on the same packet session. These checks establish the observed loss-only
  behavior, not a guarantee against every possible transport defect.
- **Test handling:** only NK04's `tuic_datagram/65534` and `/65535` boundary
  cases wait for the client's QUIC handshake-confirmation event (retirement of
  handshake encryption keys) before submitting data. This removes the known
  transient coalesced-packet budget from the boundary measurement. Each case
  still requires a single successful write and exact echo, including address
  and length, within the unchanged ten-second fixture. No retry, skip, timeout
  acceptance, or closed-pipe tolerance is added. Other sizes and protocols keep
  their original setup and assertions. These two cases no longer measure large
  datagram delivery during the handshake; smaller TUIC cases retain that setup.
- **Why not fixed now:** the owner authorized a test-only adjustment and
  disclosure at 12:06 EST on November 11, 2026. The real fix is deferred to a
  future quic-go upgrade or patch. No production source, dependency pin,
  manifest, carried patch, or CI workflow is changed.
- **Status:** accepted known limitation; transport loss remains unfixed.
- **Evidence:** Linux baseline [42998145808](https://github.com/ad1896git/nekoray/actions/runs/42998145808),
  public-API trace [42999319230](https://github.com/ad1896git/nekoray/actions/runs/42999319230),
  packer probe [42999963940](https://github.com/ad1896git/nekoray/actions/runs/42999963940),
  and recurrence [43064907195, attempt 1](https://github.com/ad1896git/nekoray/actions/runs/43064907195/attempts/1).
  All 58 failures in the completed packer probe had the same 1,200-versus-1,150
  discard; no race, panic, or process timeout was reported. The later successful
  rerun of the recurrence is not a transport fix. Investigation and local
  loss/recovery evidence are retained in `.pipeline/runs/p2-d5-impl.md` and
  `.pipeline/runs/p2-d5-opt2-impl.md` in the owner workspace.
