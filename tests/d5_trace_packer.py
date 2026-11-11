"""Disposable diagnostic instrumentation, not a transport change."""
from pathlib import Path

for path in (Path('../quic-go/packet_packer.go'), Path('../quic-go/ech/packet_packer.go')):
    source = path.read_text()
    for old, reason in (
        ('if size > p.maxDatagramFrameSize {', 'peer_limit'),
        ('} else if !hasAck {', 'packet_space'),
        ('if p.peekTimes > DatagramFrameMaxPeekTimes {', 'peek_limit'),
    ):
        assert source.count(old) == 1, (path, old)
        source = source.replace(old, old + '\n' +
            '\t\t\tfmt.Printf("D5_PACKER_DROP reason=' + reason +
            ' frame=%d maxFrame=%d plLen=%d hasAck=%t peerLimit=%d payload=%x\\n", '+
            'size, maxFrameSize, pl.length, hasAck, p.maxDatagramFrameSize, f.Data[:10])')
    path.write_text(source)
