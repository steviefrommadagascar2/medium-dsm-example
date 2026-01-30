#!/bin/bash
# Simple DSM test script

set -e

echo "=== Starting DSM Test ==="

# Cleanup
pkill -9 dsm 2>/dev/null || true
rm -f /tmp/dsm_*.log /tmp/dsm_region_*.dat

# Start server in background
echo "Starting server..."
./dsm --server --port 9010 --clients 1 -v > /tmp/dsm_server.log 2>&1 &
SERVER_PID=$!
echo "Server PID: $SERVER_PID"

# Wait for server to start
sleep 2

# Check if server is running
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "Server failed to start!"
    cat /tmp/dsm_server.log
    exit 1
fi

# Start client
echo "Starting client..."
./dsm --client --host 127.0.0.1 --port 9010 -v > /tmp/dsm_client.log 2>&1 &
CLIENT_PID=$!
echo "Client PID: $CLIENT_PID"

# Wait for completion (max 60 seconds)
echo "Waiting for completion..."
for i in {1..60}; do
    if ! kill -0 $CLIENT_PID 2>/dev/null; then
        echo "Client finished"
        break
    fi
    sleep 1
done

# Give server time to finish
sleep 2

# Kill any remaining processes
kill $SERVER_PID 2>/dev/null || true
kill $CLIENT_PID 2>/dev/null || true

# Show results
echo ""
echo "=== SERVER LOG ==="
cat /tmp/dsm_server.log

echo ""
echo "=== CLIENT LOG ==="
cat /tmp/dsm_client.log

echo ""
echo "=== Test Complete ==="
