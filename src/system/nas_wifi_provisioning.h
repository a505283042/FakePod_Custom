#pragma once
#include "esp_err.h"
#include <stddef.h>
// Physical-button, 180-second, single-use PIN-protected AES-GCM provisioning.
// Device shows a 6-digit code; at most 3 connection attempts are accepted.
esp_err_t nas_wifi_provisioning_arm(char code[7]);
void nas_wifi_provisioning_cancel();
