#!/bin/bash
# Mini-UnionFS Test Suite
# Tests layer visibility, Copy-on-Write, and whiteout mechanisms

FUSE_BINARY="./mini_unionfs"
TEST_DIR="./unionfs_test_env"
LOWER_DIR="$TEST_DIR/lower"
UPPER_DIR="$TEST_DIR/upper"
MOUNT_DIR="$TEST_DIR/mnt"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=0
FAILED=0

# Cleanup function
cleanup() {
    echo ""
    echo "Cleaning up..."
    fusermount -u "$MOUNT_DIR" 2>/dev/null || umount "$MOUNT_DIR" 2>/dev/null
    sleep 1
    rm -rf "$TEST_DIR"
}

# Handle interrupts
trap cleanup EXIT

echo "========================================"
echo "Mini-UnionFS Test Suite"
echo "========================================"
echo ""

# Check if binary exists
if [ ! -f "$FUSE_BINARY" ]; then
    echo -e "${RED}Error: $FUSE_BINARY not found. Run 'make' first.${NC}"
    exit 1
fi

# Setup test environment
echo "Setting up test environment..."
rm -rf "$TEST_DIR"
mkdir -p "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR"

# Create test files in lower directory
echo "base_only_content" > "$LOWER_DIR/base.txt"
echo "to_be_deleted" > "$LOWER_DIR/delete_me.txt"
echo "lower_version" > "$LOWER_DIR/override.txt"
mkdir -p "$LOWER_DIR/subdir"
echo "nested_file_content" > "$LOWER_DIR/subdir/nested.txt"

# Create a file in upper that overrides lower
echo "upper_version" > "$UPPER_DIR/override.txt"

echo "Starting Mini-UnionFS..."
$FUSE_BINARY "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 2

# Check if mount succeeded
if ! mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
    if [ ! -f "$MOUNT_DIR/base.txt" ]; then
        echo -e "${RED}Error: Failed to mount filesystem${NC}"
        exit 1
    fi
fi

echo ""
echo "Running tests..."
echo ""

# Test 1: Layer Visibility - Lower file visible
echo -n "Test 1: Lower layer file visible... "
if grep -q "base_only_content" "$MOUNT_DIR/base.txt" 2>/dev/null; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    ((FAILED++))
fi

# Test 2: Layer Override - Upper takes precedence
echo -n "Test 2: Upper layer overrides lower... "
if grep -q "upper_version" "$MOUNT_DIR/override.txt" 2>/dev/null; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    ((FAILED++))
fi

# Test 3: Directory listing shows merged view
echo -n "Test 3: Directory listing shows merged view... "
LS_OUTPUT=$(ls "$MOUNT_DIR" 2>/dev/null)
if echo "$LS_OUTPUT" | grep -q "base.txt" && \
   echo "$LS_OUTPUT" | grep -q "override.txt" && \
   echo "$LS_OUTPUT" | grep -q "delete_me.txt" && \
   echo "$LS_OUTPUT" | grep -q "subdir"; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    echo "  Got: $LS_OUTPUT"
    ((FAILED++))
fi

# Test 4: Copy-on-Write
echo -n "Test 4: Copy-on-Write... "
echo "modified_content" >> "$MOUNT_DIR/base.txt" 2>/dev/null
if [ $(grep -c "modified_content" "$MOUNT_DIR/base.txt" 2>/dev/null) -eq 1 ] && \
   [ $(grep -c "modified_content" "$UPPER_DIR/base.txt" 2>/dev/null) -eq 1 ] && \
   [ $(grep -c "modified_content" "$LOWER_DIR/base.txt" 2>/dev/null) -eq 0 ]; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    echo "  Mount content: $(cat "$MOUNT_DIR/base.txt" 2>/dev/null)"
    echo "  Upper content: $(cat "$UPPER_DIR/base.txt" 2>/dev/null)"
    echo "  Lower content: $(cat "$LOWER_DIR/base.txt" 2>/dev/null)"
    ((FAILED++))
fi

# Test 5: Whiteout mechanism
echo -n "Test 5: Whiteout mechanism... "
rm "$MOUNT_DIR/delete_me.txt" 2>/dev/null
if [ ! -f "$MOUNT_DIR/delete_me.txt" ] && \
   [ -f "$LOWER_DIR/delete_me.txt" ] && \
   [ -f "$UPPER_DIR/.wh.delete_me.txt" ]; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    echo "  Mount file exists: $(test -f "$MOUNT_DIR/delete_me.txt" && echo yes || echo no)"
    echo "  Lower file exists: $(test -f "$LOWER_DIR/delete_me.txt" && echo yes || echo no)"
    echo "  Whiteout exists: $(test -f "$UPPER_DIR/.wh.delete_me.txt" && echo yes || echo no)"
    ((FAILED++))
fi

# Test 6: Create new file
echo -n "Test 6: Create new file... "
echo "new_file_content" > "$MOUNT_DIR/new_file.txt" 2>/dev/null
if [ -f "$MOUNT_DIR/new_file.txt" ] && \
   [ -f "$UPPER_DIR/new_file.txt" ] && \
   [ ! -f "$LOWER_DIR/new_file.txt" ]; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    ((FAILED++))
fi

# Test 7: Create directory
echo -n "Test 7: Create directory... "
mkdir "$MOUNT_DIR/new_dir" 2>/dev/null
if [ -d "$MOUNT_DIR/new_dir" ] && \
   [ -d "$UPPER_DIR/new_dir" ]; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    ((FAILED++))
fi

# Test 8: Nested file access
echo -n "Test 8: Nested file access... "
if grep -q "nested_file_content" "$MOUNT_DIR/subdir/nested.txt" 2>/dev/null; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    ((FAILED++))
fi

# Test 9: Whiteout hides file from listing
echo -n "Test 9: Whiteout hides file from listing... "
LS_OUTPUT=$(ls "$MOUNT_DIR" 2>/dev/null)
if ! echo "$LS_OUTPUT" | grep -q "delete_me.txt"; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    ((FAILED++))
fi

# Test 10: Remove directory from lower layer
echo -n "Test 10: Remove directory (whiteout)... "
rm -r "$MOUNT_DIR/subdir" 2>/dev/null
if [ ! -d "$MOUNT_DIR/subdir" ] && \
   [ -d "$LOWER_DIR/subdir" ] && \
   [ -f "$UPPER_DIR/.wh.subdir" ]; then
    echo -e "${GREEN}PASSED${NC}"
    ((PASSED++))
else
    echo -e "${RED}FAILED${NC}"
    echo "  Mount dir exists: $(test -d "$MOUNT_DIR/subdir" && echo yes || echo no)"
    echo "  Lower dir exists: $(test -d "$LOWER_DIR/subdir" && echo yes || echo no)"
    echo "  Whiteout exists: $(test -f "$UPPER_DIR/.wh.subdir" && echo yes || echo no)"
    ((FAILED++))
fi

# Summary
echo ""
echo "========================================"
echo "Test Results: ${PASSED} passed, ${FAILED} failed"
echo "========================================"

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed.${NC}"
    exit 1
fi
