#!/bin/bash

# Configuration
SHELL_EXEC="./shell"
FAIL_COUNT=0

# Ensure the shell executable exists
if [ ! -x "$SHELL_EXEC" ]; then
    echo "Error: $SHELL_EXEC not found or not executable. Compile it first."
    exit 1
fi

# Helper function to evaluate test outcomes
assert_test() {
    local test_name="$1"
    local exit_code=$2
    if [ $exit_code -eq 0 ]; then
        echo -e "[\e[32mPASS\e[0m] $test_name"
    else
        echo -e "[\e[31mFAIL\e[0m] $test_name"
        ((FAIL_COUNT++))
    fi
}

echo "========================================"
echo "Starting Non-Interactive Shell Autotests"
echo "========================================"

# --- TEST 1: Built-in Command (pwd) ---
expected_pwd=$(pwd)
actual_pwd=$($SHELL_EXEC <<< "pwd")

if [ "$actual_pwd" = "$expected_pwd" ]; then
    assert_test "Built-in 'pwd'" 0
else
    assert_test "Built-in 'pwd'" 1
fi

# --- TEST 2: Basic Command Path Resolution & Execution ---
$SHELL_EXEC <<< "echo hello_test_world" | grep -q "hello_test_world"
assert_test "Path Resolution (echo)" $?

# --- TEST 3: File Redirection In & Out ---
# Create temporary fixture
echo -e "banana\napple\ncherry" > test_input.txt

# Run sorting/redirection through your shell
$SHELL_EXEC <<< "sort < test_input.txt > test_output.txt"

# Check if file matches expected sort
echo -e "apple\nbanana\ncherry" > test_expected.txt
diff test_output.txt test_expected.txt &> /dev/null
assert_test "Redirection (< and >)" $?

# --- TEST 4: Multi-stage Pipeline (3 stages) ---
# Count lines matching 'a' from our input fixture
actual_pipe_out=$($SHELL_EXEC <<< "cat test_input.txt | grep a | wc -l")
# Remove whitespace from output
actual_pipe_out=$(echo $actual_pipe_out | tr -d '[:space:]')

if [ "$actual_pipe_out" = "2" ]; then
    assert_test "Multi-stage Pipeline (cat | grep | wc)" 0
else
    assert_test "Multi-stage Pipeline (cat | grep | wc)" 1
fi

# --- TEST 5: Complex Integration (Redirection + Multi-pipe) ---
$SHELL_EXEC <<< "cat < test_input.txt | grep error | wc -l > integrated_out.txt"
# Since 'error' is not in our input file, the output should be 0
final_val=$(cat integrated_out.txt | tr -d '[:space:]')

if [ "$final_val" = "0" ]; then
    assert_test "Redirection integrated with Pipeline" 0
else
    assert_test "Redirection integrated with Pipeline" 1
fi

# --- CLEANUP FIXTURES ---
rm -f test_input.txt test_output.txt test_expected.txt integrated_out.txt

# --- FINAL RESULTS SUMMARY ---
echo "========================================"
if [ $FAIL_COUNT -eq 0 ]; then
    echo -e "\e[32mALL TESTS PASSED SUCCESSFULLY!\e[0m"
    exit 0
else
    echo -e "\e[31mTEST SUITE FAILED: $FAIL_COUNT test(s) failed.\e[0m"
    exit 1
fi
