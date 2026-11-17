#!/usr/bin/env bash
# MKFSTOOL for buildfs when SOURCE_DATE_EPOCH is set (scripts/build_frontend.py): mklittlefs from the PlatformIO
# package, with fixed_time.c preloaded so the root directory stamp is SOURCE_DATE_EPOCH (TIN-7545).
exec env LD_PRELOAD="${SOFTPROXY_FIXED_TIME_SO:?}" mklittlefs "$@"
