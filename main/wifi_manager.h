#pragma once

// Initialize WiFi - provisioning if needed, STA mode if already provisioned
void wifi_manager_start(void);

// Erase WiFi credentials and reboot into provisioning mode
void wifi_manager_reset(void);