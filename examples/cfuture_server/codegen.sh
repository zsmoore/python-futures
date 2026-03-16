#!/usr/bin/env bash
# Generate gRPC Python stubs from the shared proto file.
# Run from the cfuture_server/ directory after activating your venv.
set -euo pipefail

PROTO_DIR="$(cd "$(dirname "$0")/../proto" && pwd)"
OUT_DIR="$(cd "$(dirname "$0")/cfuture_server/generated" && pwd)"

python -m grpc_tools.protoc \
    -I"$PROTO_DIR" \
    --python_out="$OUT_DIR" \
    --grpc_python_out="$OUT_DIR" \
    "$PROTO_DIR/worker.proto"

# grpcio-tools generates a bare 'import worker_pb2' which breaks when the
# module is inside a package.  Fix it to a package-relative import.
sed -i '' \
    's/^import worker_pb2 as worker__pb2$/from cfuture_server.generated import worker_pb2 as worker__pb2/' \
    "$OUT_DIR/worker_pb2_grpc.py"

echo "Stubs written to $OUT_DIR"
