# Jak Project Switch

This project ports the original trilogy (Jak 1 -> Jak 3) to PC using GOAL, a custom LISP language developed by Naughty Dog.

## Network Logging Configuration

The project supports live network logging via FIX 7s. To enable network logging:

1. Create a file named `gk_log_host.txt` on your SD card
2. Place the IP address of your development machine in this file (e.g., `192.168.1.100`)
3. The system will automatically connect to port 12345 for live logging

### File Location
- **SD Card Path**: `sdmc:/gk_log_host.txt`
- **Example Content**: 
```
192.168.1.100
```

### How It Works
- The system reads the host address from `gk_log_host.txt` during startup
- If a valid IP is found, logs are sent to that machine via TCP on port 12345
- If no file exists or connection fails, logs fall back to SD card logging only