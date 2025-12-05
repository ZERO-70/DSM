#!/bin/bash
# Test Read/Write DSM with Ownership Transfer
# This script runs master and worker nodes to test ownership transfer

set -e

cd "$(dirname "$0")/.."

export LD_LIBRARY_PATH=./bin:$LD_LIBRARY_PATH

echo "========================================="
echo "  Read/Write DSM Ownership Transfer Test"
echo "========================================="

# Clean up any stale processes
cleanup() {
    echo "Cleaning up..."
    kill $MASTER_PID 2>/dev/null || true
    kill $WORKER_PID 2>/dev/null || true
    rm -f /tmp/master_out.txt /tmp/worker_out.txt
}
trap cleanup EXIT

echo ""
echo "Starting Master node on port 8080..."
# Start master and send commands via stdin
(
    sleep 5  # Wait for worker to connect
    echo "7"  # Run test 7 (Read/Write DSM)
    sleep 5  # Wait for test to complete
    echo "q"  # Quit
) | timeout 30 ./bin/test_dsm 8080 10 > /tmp/master_out.txt 2>&1 &
MASTER_PID=$!

sleep 2  # Give master time to start

echo "Starting Worker node on port 8081..."
# Start worker and send commands via stdin
(
    sleep 3  # Wait a bit for master setup
    echo "7"  # Run test 7 (Read/Write DSM)
    sleep 5  # Wait for test to complete  
    echo "q"  # Quit
) | timeout 30 ./bin/test_dsm 8081 10 > /tmp/worker_out.txt 2>&1 &
WORKER_PID=$!

echo ""
echo "Waiting for tests to complete..."
wait $MASTER_PID || true
wait $WORKER_PID || true

echo ""
echo "========================================="
echo "  MASTER OUTPUT"
echo "========================================="
cat /tmp/master_out.txt || echo "(no output)"

echo ""
echo "========================================="
echo "  WORKER OUTPUT"
echo "========================================="
cat /tmp/worker_out.txt || echo "(no output)"

echo ""
echo "========================================="
echo "  TEST COMPLETE"
echo "========================================="
