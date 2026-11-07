# When updating these three pins, also update their pseudo-version replacements
# and checksums in go/cmd/nekobox_core/go.{mod,sum}; build_core.sh checks the pair.
export COMMIT_SING_BOX="8bd0e6be17a44ccfcb55c7812f1b389f1b3b80c1"
export COMMIT_SING_QUIC="9c9fdac85d4bfbfa0336ea1c1154b384c84c259d"
export COMMIT_LIBNEKO="1c47a3af71990a7b2192e03292b4d246c308ef0b"

# sing-box and sing-quic pins are commits on the ad1896git forks; libneko is upstream.
export NKR_SRC_FORK="https://github.com/ad1896git"
export NKR_SRC_UPSTREAM="https://github.com/MatsuriDayo"

export COMMIT_QUIC_GO="7101329b86a0c801e643fc8ccf730ec89be10663"
export NKR_SRC_QUIC_GO="https://github.com/sagernet"
