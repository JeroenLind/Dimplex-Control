// Stub of esp_sntp.h for the PC tests: the real header registers a callback that is called after every NTP sync.
#pragma once
#include <sys/time.h>
static inline void sntp_set_time_sync_notification_cb(void (*)(struct timeval *)) {}
