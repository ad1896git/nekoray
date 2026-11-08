# When updating these three pins, also update their pseudo-version replacements
# and checksums in go/cmd/nekobox_core/go.{mod,sum}; build_core.sh checks the pair.
export COMMIT_SING_BOX="8bd0e6be17a44ccfcb55c7812f1b389f1b3b80c1"
export COMMIT_SING_QUIC="9c9fdac85d4bfbfa0336ea1c1154b384c84c259d"
export COMMIT_LIBNEKO="45e350171b7b623860dd60ebce61a8e7c78de87e"

# sing-box, sing-quic and libneko pins are commits on the ad1896git forks.
export NKR_SRC_FORK="https://github.com/ad1896git"
export NKR_SRC_LIBNEKO="https://github.com/ad1896git"

export COMMIT_QUIC_GO="7101329b86a0c801e643fc8ccf730ec89be10663"
export NKR_SRC_QUIC_GO="https://github.com/sagernet"
