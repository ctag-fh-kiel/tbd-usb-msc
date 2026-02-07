#!/bin/zsh

# Benchmark file copy performance
# Usage: ./benchmark_copy.sh <size_in_MB> <destination>

if [ $# -ne 2 ]; then
    echo "Usage: $0 <size_in_MB> <destination>"
    echo "Example: $0 100 /path/to/destination"
    exit 1
fi

SIZE_MB=$1
DESTINATION=$2

# Validate size is a number
if ! [[ "$SIZE_MB" =~ ^[0-9]+$ ]]; then
    echo "Error: Size must be a positive integer"
    exit 1
fi

# Validate destination exists
if [ ! -d "$DESTINATION" ]; then
    echo "Error: Destination directory does not exist: $DESTINATION"
    exit 1
fi

# Create temporary file name
TEMP_FILE="/tmp/benchmark_source_${SIZE_MB}MB_$$.bin"
DEST_FILE="${DESTINATION}/benchmark_copy_${SIZE_MB}MB_$$.bin"

echo "Creating ${SIZE_MB}MB dummy file..."
# Create dummy file using dd (macOS uses uppercase M for megabytes)
dd if=/dev/zero of="$TEMP_FILE" bs=1M count=$SIZE_MB 2>/dev/null

if [ $? -ne 0 ]; then
    echo "Error: Failed to create dummy file"
    exit 1
fi

echo "Copying file to destination..."
# Clear file system cache before copying (best effort)
sync

# Measure copy time with high precision
# Try gdate first (GNU coreutils), then fall back to perl for sub-second precision
if command -v gdate &> /dev/null; then
    START_TIME=$(gdate +%s.%N)
    cp "$TEMP_FILE" "$DEST_FILE"
    sync  # Ensure write is complete
    END_TIME=$(gdate +%s.%N)
    DURATION=$(echo "$END_TIME - $START_TIME" | bc)
elif command -v perl &> /dev/null; then
    START_TIME=$(perl -MTime::HiRes=time -e 'print time')
    cp "$TEMP_FILE" "$DEST_FILE"
    sync  # Ensure write is complete
    END_TIME=$(perl -MTime::HiRes=time -e 'print time')
    DURATION=$(echo "$END_TIME - $START_TIME" | bc)
else
    # Fallback to basic seconds (less precise)
    START_TIME=$(date +%s)
    cp "$TEMP_FILE" "$DEST_FILE"
    sync  # Ensure write is complete
    END_TIME=$(date +%s)
    DURATION=$((END_TIME - START_TIME))
    if [ $DURATION -eq 0 ]; then
        DURATION=1
    fi
fi

# Calculate speed in MB/s
if command -v bc &> /dev/null; then
    SPEED=$(echo "scale=2; $SIZE_MB / $DURATION" | bc)
else
    # Fallback without bc (less precise)
    SPEED=$((SIZE_MB / DURATION))
fi

echo ""
echo "===== Benchmark Results ====="
echo "File size:    ${SIZE_MB} MB"
echo "Duration:     ${DURATION} seconds"
echo "Speed:        ${SPEED} MB/s"
echo "============================="

# Cleanup
echo ""
echo "Cleaning up temporary files..."
rm -f "$TEMP_FILE"
rm -f "$DEST_FILE"

echo "Done!"

