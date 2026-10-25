#!/bin/bash
set -e

source libs/env_deploy.sh
ENV_NEKORAY=1
source libs/get_source_env.sh
pushd ..

####

if [ ! -d "sing-box" ]; then
  git clone --no-checkout "${NKR_SRC_BASE:-$NKR_SRC_FORK}/sing-box.git"
fi
pushd sing-box
git checkout "$COMMIT_SING_BOX"

popd

####

if [ ! -d "sing-quic" ]; then
  git clone --no-checkout "${NKR_SRC_BASE:-$NKR_SRC_FORK}/sing-quic.git"
fi
pushd sing-quic
git checkout "$COMMIT_SING_QUIC"

popd

####

if [ ! -d "libneko" ]; then
  git clone --no-checkout "$NKR_SRC_UPSTREAM/libneko.git"
fi
pushd libneko
git checkout "$COMMIT_LIBNEKO"

popd

####

popd
