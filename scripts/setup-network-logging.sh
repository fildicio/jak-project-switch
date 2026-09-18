#!/bin/bash

# Setup network logging for Switch port
# This script configures the development environment to receive logs from the Switch

echo "Setting up network logging for Jak Project Switch..."

# Get the IP address of the current machine
LOCAL_IP=$(hostname -I | awk '{print $1}')
echo "Local IP address: $LOCAL_IP"

# Create the log host file
echo "$LOCAL_IP" > switch-crash-reports/gk_log_host.txt

echo "Network logging configured!"
echo "Copy switch-crash-reports/gk_log_host.txt to your Switch SD card root"
echo "The Switch will now send logs to $LOCAL_IP"

# Instructions for setting up the TCP listener
echo ""
echo "To receive logs on your development machine:"
echo "1. Run: nc -l -p 51713 (or whatever port is used by the application)"
echo "2. Or use a more sophisticated log receiver if needed"