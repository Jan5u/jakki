#!/bin/bash
set -e

IMAGE_NAME="jakki-builder"
CONTAINER_NAME="jakki-build-container"

echo "Building Docker image..."
docker build -t "$IMAGE_NAME" .

echo "Extracting binary from Docker container..."
mkdir -p build/release
docker create --name "$CONTAINER_NAME" "$IMAGE_NAME" >/dev/null 2>&1 || true

if docker cp "$CONTAINER_NAME:/src/build/release/jakki" build/release/jakki; then
  echo "Binary extracted to: build/release/jakki"
  chmod +x build/release/jakki
else
  echo "Failed to extract binary"
  docker rm "$CONTAINER_NAME" >/dev/null 2>&1 || true
  exit 1
fi

docker rm "$CONTAINER_NAME" >/dev/null 2>&1 || true

echo "Build completed successfully!"
