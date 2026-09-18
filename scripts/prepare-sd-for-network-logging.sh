#!/bin/bash

# Script to prepare SD card for network logging in Jak Project Switch
# This script helps automate the setup process

echo "Preparing SD card for network logging..."

# Get current directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKING_DIR="$(dirname "$SCRIPT_DIR")"

# Check if gk_log_host.txt exists
if [ ! -f "$WORKING_DIR/switch-crash-reports/gk_log_host.txt" ]; then
    echo "Creating gk_log_host.txt file..."
    # Create with a placeholder IP - you'll need to update this
    echo "192.168.1.100" > "$WORKING_DIR/switch-crash-reports/gk_log_host.txt"
fi

echo ""
echo "=== Network Logging Setup Instructions ==="
echo ""
echo "1. Find your development machine's IP address:"
echo "   On Mac: ifconfig | grep 'inet ' | grep -v 127.0.0.1"
echo ""
echo "2. Update the gk_log_host.txt file with your actual IP address:"
echo "   Edit $WORKING_DIR/switch-crash-reports/gk_log_host.txt"
echo ""
echo "3. Copy this file to your Switch SD card root directory:"
echo "   - Insert SD card into your computer"
echo "   - Copy gk_log_host.txt to the root of the SD card"
echo ""
echo "4. Set up TCP listener on development machine:"
echo "   nc -l -p 51713"
echo ""
echo "5. Start the game and monitor logs in real-time!"
echo ""
echo "=== Important Notes ==="
echo "- The file must be named exactly 'gk_log_host.txt'"
echo "- The file should contain only the IP address (no extra characters)"
echo "- Make sure your firewall allows connections on port 51713"
echo "- The Switch will automatically detect this file and start sending logs"

echo ""
echo "Setup complete. Please follow the instructions above."