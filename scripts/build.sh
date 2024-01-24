#!/bin/bash

function usage() {
    echo "usage: scripts/test.sh [-s|--snap-samples]" >&2
    exit 255
}

SNAP_SAMPLES="OFF"

for arg in "$@"; do
    shift
    case "${arg}" in
        '--help'|'-h') usage ;;
        '--snap-samples'|'-s') SNAP_SAMPLES="ON" ;;
    esac
done

set -xe

# Globals
SCRIPT_DIR=$(dirname $(readlink -f $0))
ROOT_DIR=${SCRIPT_DIR}/../
BUILD_DIR=${ROOT_DIR}/build
BIN_DIR=${ROOT_DIR}/bin
CMAKE=${BIN_DIR}/bin/cmake

. ${SCRIPT_DIR}/submodule_check.sh

mkdir -p $BUILD_DIR
cd $BUILD_DIR

$CMAKE -DSNAPSHOT_SAMPLES=${SNAP_SAMPLES} -DCMAKE_BUILD_TYPE=Release ..
make -j `nproc`
