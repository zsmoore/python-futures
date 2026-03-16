#!/usr/bin/env bash
# Generate gRPC Python stubs from the shared proto file.
# Run from the asyncio_server/ directory after activating your venv.
set -euo pipefail

PROTO_DIR="$(cd "$(dirname "$0")/../proto" && pwd)"
OUT_DIR="$(cd "$(dirname "$0")/asyncio_server/generated" && pwd)"

python -m grpc_tools.protoc \
    -I"$PROTO_DIR" \
    --python_out="$OUT_DIR" \
    --grpc_python_out="$OUT_DIR" \
    "$PROTO_DIR/worker.proto"

echo "Stubs written to $OUT_DIR"
