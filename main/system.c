#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_heap_task_info.h"
#include "config.h"
#include "system.h"
#include "storage.h"
#include "wifi.h"
#include "net_module.h"

#define TAG "-->SYSTEM"  // Logging tag for system module

#define NTP_SYNC_INTERVAL_SEC   (60 * 60 * 24)
#define NTP_SYNC_RETRY_COUNT    5
#define NTP_SYNC_RETRY_MS       2000
#define NTP_VALID_EPOCH_MIN     1577836800  /* 2020-01-01 00:00:00 UTC */

static int time_delta = 0;    //When synchronizing time, the error time between the system and the actual time, in seconds.
static char ntp_sync_flag = 0;  //The flag indicating whether ntp is synchronized.
static RTC_NOINIT_ATTR modeSel_e g_tmpMode = MODE_UNDEFINED;
static RTC_DATA_ATTR time_t g_lastNtpSyncTime = 0;
/**
 * Get the current system mode
 * @return modeSel_e
 */
modeSel_e system_get_mode(void)
{
    return main_mode;
}

/**
 * Synchronize system time with NTP server
 * @param force_sync If true, force synchronization even if NTP synchronization is disabled  
 * @return ESP_OK on success, ESP_FAIL on timeout
 */
esp_err_t system_ntp_time(bool force_sync)
{
    if (!force_sync && !system_is_ntp_sync_enable()) {
        ESP_LOGI(TAG, "NTP synchronization is disabled, skip synchronization");
        return ESP_OK;
    }

    if (!force_sync) {
        time_t now = time(NULL);
        if (g_lastNtpSyncTime != 0 && now >= NTP_VALID_EPOCH_MIN &&
            (now - g_lastNtpSyncTime) < NTP_SYNC_INTERVAL_SEC) {
            ESP_LOGI(TAG, "NTP synchronization is already done today, skip synchronization");
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
                               ESP_SNTP_SERVER_LIST("pool.ntp.org", "ntp.aliyun.com", "time.windows.com"));
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SNTP: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    int retry = 0;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_SYNC_RETRY_MS)) != ESP_OK &&
           ++retry < NTP_SYNC_RETRY_COUNT) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, NTP_SYNC_RETRY_COUNT);
    }

    esp_netif_sntp_deinit();

    if (retry >= NTP_SYNC_RETRY_COUNT) {
        ESP_LOGE(TAG, "Failed to obtain time");
        return ESP_FAIL;
    }

    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "The current time is: %s", asctime(&timeinfo));

    /* Note: Don't sync to RTC immediately - camera may be using I2C bus.
     * RTC will be synced before deep sleep when I2C bus is free. */
    g_lastNtpSyncTime = now;
    ntp_sync_flag = 1;
    return ESP_OK;
}

/**
 * System get time delta.
 */
int system_get_time_delta()
{
    return time_delta;
}

/**
 * System get ntp sync flag.
 */
int system_get_ntp_sync_flag()
{
    return ntp_sync_flag;
}

/**
 * Set system time and timezone
 * @param tAttr Time attributes containing timestamp and timezone
 * @return ESP_OK on success
 */
esp_err_t system_set_time(timeAttr_t *tAttr)
{
    time_t t_of_day = tAttr->ts;
    char buf[64];

    // Get timezone from config if not provided
    if (strlen(tAttr->tz) == 0) {
        cfg_get_timezone(tAttr->tz);
    }
    
    ESP_LOGI(TAG, "set timezone: %s", tAttr->tz);
    if (system_set_timezone(tAttr->tz) == ESP_OK) {
        cfg_set_timezone(tAttr->tz);  // Persist timezone to config
    }

    // Set system time
    struct timeval epoch = {t_of_day, 0};
    settimeofday(&epoch, NULL);
    
    // Log the new time
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t_of_day));
    ESP_LOGI(TAG, "Sync clock to: %s", buf);
    return ESP_OK;
}

/**
 * Get current system time and timezone
 * @param tAttr Output parameter for time attributes
 * @return ESP_OK
 */
esp_err_t system_get_time(timeAttr_t *tAttr)
{
    char *tz = getenv("TZ");
    memset(tAttr, 0, sizeof(timeAttr_t));
    tAttr->ts = time(NULL);  // Get current timestamp
    
    // Copy timezone if set
    if (tz) {
        strncpy(tAttr->tz, tz, sizeof(tAttr->tz));
    }
    return ESP_OK;
}

/**
 * Set system timezone
 * @param tz Timezone string in POSIX format
 * @return ESP_OK
 */
esp_err_t system_set_timezone(const char *tz)
{
    setenv("TZ", tz, 1);  // Set timezone environment variable
    tzset();              // Apply timezone change
    return ESP_OK;
}

/**
 * Get firmware version string
 * @return Version string from app description
 */
const char *system_get_version()
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc->version;
}

/**
 * Reset system configuration and storage
 * Erases all user settings and formats storage
 */
void system_reset()
{
    cfg_user_erase_all();  // Clear all configuration
    storage_clear_jpg_file(); // Clear jpg files
}

/**
 * Restart the system
 */
void system_restart()
{
    esp_restart();  // ESP32 system restart
}

/**
 * Get system restart reason
 * @return Restart reason code
 */
rstReason_e system_restart_reasons()
{
    int reason = esp_reset_reason();
    switch (reason) {
        case ESP_RST_POWERON:
            return RST_POWER_ON;     // Power-on reset
        case ESP_RST_SW:
            return RST_SOFTWARE;     // Software reset
        case ESP_RST_DEEPSLEEP:
            return RST_DEEP_SLEEP;   // Wake from deep sleep
        default:
            ESP_LOGW(TAG, "unknown wakeup reason [%d]", reason);
            return RST_POWER_ON;     // Default to power-on
    }
}

/**
 * Print memory usage information
 * Shows both internal and SPI RAM heap usage
 */
void system_show_meminfo()
{
    ESP_LOGI(TAG, "show meminfo:");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);  // Internal RAM
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);    // SPI RAM
}

/**
 * Print RTOS task information
 * Shows heap summary and per-task stack high water mark, priority, run time and CPU ratio
 */
void system_show_taskinfo()
{
    /* Same heap as xPortGetFreeHeapSize(): internal RAM only, no SPI RAM */
    const uint32_t heap_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    uint32_t total_run_time = 0;
    UBaseType_t task_num = uxTaskGetNumberOfTasks();
    TaskStatus_t *status = malloc(task_num * sizeof(TaskStatus_t));

    if (status == NULL) {
        ESP_LOGE(TAG, "task info: no memory for %u tasks", (unsigned)task_num);
        return;
    }
    /* Returns 0 when a task was created between the count and the snapshot */
    task_num = uxTaskGetSystemState(status, task_num, &total_run_time);
    if (task_num == 0) {
        ESP_LOGE(TAG, "task info: task list changed, retry");
        free(status);
        return;
    }

    /* Sort by task number so tasks are listed in creation order */
    for (UBaseType_t i = 1; i < task_num; i++) {
        TaskStatus_t cur = status[i];
        UBaseType_t j = i;
        for (; j > 0 && status[j - 1].xTaskNumber > cur.xTaskNumber; j--) {
            status[j] = status[j - 1];
        }
        status[j] = cur;
    }

    printf("   ###############################System Status###############################\n");
    printf("   [TotalRam:%u Byte  RemainRam:%u Byte  MinRam:%u Byte  TaskNum:%u  RunTime:%ums]\n",
           (unsigned)heap_caps_get_total_size(heap_caps), (unsigned)heap_caps_get_free_size(heap_caps),
           (unsigned)heap_caps_get_minimum_free_size(heap_caps), (unsigned)task_num,
           (unsigned)(total_run_time / 1000));
    printf("   %-16s  %-15s  %-8s  %-11s  %s\n",
           "TaskName", "MinStack(Byte)", "Priority", "RunTime(ms)", "Ratio(%)");
    for (UBaseType_t i = 0; i < task_num; i++) {
        printf("   %-16s  %-15u  %-8u  %-11u  %.2f %%\n", status[i].pcTaskName,
               (unsigned)status[i].usStackHighWaterMark, (unsigned)status[i].uxCurrentPriority,
               (unsigned)(status[i].ulRunTimeCounter / 1000),
               total_run_time ? (double)status[i].ulRunTimeCounter * 100.0 / total_run_time : 0.0);
    }
#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    printf("   RunTime/Ratio unavailable: CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is off\n");
#else
    /* The run time counter is a 32-bit us tick, it wraps about 71.6 minutes after boot */
    if (esp_timer_get_time() >> 32) {
        printf("   RunTime/Ratio counters have overflowed (uptime > 71.6 min), reboot for accurate values\n");
    }
#endif
    printf("   ###########################################################################\n");

    free(status);
}

/**
 * Print heap usage summary, optionally attributed per owning task
 * @param by_task Also print per-task allocation totals (needs CONFIG_HEAP_TASK_TRACKING)
 */
void system_show_heapinfo(bool by_task)
{
    const uint32_t heap_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    printf("   [Internal] total:%u  free:%u  min:%u  largest:%u\n",
           (unsigned)heap_caps_get_total_size(heap_caps), (unsigned)heap_caps_get_free_size(heap_caps),
           (unsigned)heap_caps_get_minimum_free_size(heap_caps), (unsigned)heap_caps_get_largest_free_block(heap_caps));
    printf("   [SPIRAM]   total:%u  free:%u  min:%u  largest:%u\n",
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

#if !CONFIG_HEAP_TASK_TRACKING
    if (by_task) {
        printf("   per-task heap stats unavailable: CONFIG_HEAP_TASK_TRACKING is off\n");
    }
#else
    if (!by_task) {
        return;
    }

    /* +8 slack: totals also track tasks that allocated before the scheduler */
    size_t capacity = uxTaskGetNumberOfTasks() + 8;
    heap_task_totals_t *totals = malloc(capacity * sizeof(*totals));
    if (totals == NULL) {
        ESP_LOGE(TAG, "heap info: no memory for %u totals", (unsigned)capacity);
        return;
    }
    memset(totals, 0, capacity * sizeof(*totals));

    size_t num_totals = 0;
    heap_task_info_params_t params = {
        /* first-match partitioning: [0] internal, [1] SPIRAM */
        .caps = { MALLOC_CAP_INTERNAL, MALLOC_CAP_SPIRAM, 0, 0 },
        .mask = { MALLOC_CAP_INTERNAL, MALLOC_CAP_SPIRAM, 0, 0 },
        .totals = totals,
        .num_totals = &num_totals,
        .max_totals = capacity,
    };
    heap_caps_get_per_task_info(&params);

    /* Sort by internal usage descending so the biggest consumer comes first */
    for (size_t i = 1; i < num_totals; i++) {
        heap_task_totals_t cur = totals[i];
        size_t j = i;
        for (; j > 0 && totals[j - 1].size[0] < cur.size[0]; j--) {
            totals[j] = totals[j - 1];
        }
        totals[j] = cur;
    }

    printf("   %-16s  %12s  %7s  %12s  %7s\n",
           "TaskName", "Internal(B)", "Blocks", "SPIRAM(B)", "Blocks");
    for (size_t i = 0; i < num_totals; i++) {
        if (totals[i].size[0] == 0 && totals[i].size[1] == 0) {
            continue; /* task owns nothing */
        }
        printf("   %-16s  %12u  %7u  %12u  %7u\n",
               totals[i].task ? pcTaskGetName(totals[i].task) : "Pre-Scheduler",
               (unsigned)totals[i].size[0], (unsigned)totals[i].count[0],
               (unsigned)totals[i].size[1], (unsigned)totals[i].count[1]);
    }
    free(totals);
#endif
}

/**
 * Execute scheduled system tasks
 * Performs time sync, firmware updates, and cloud platform operations
 * based on current platform configuration
 */
void system_schedule_todo()
{
    if (wifi_sta_is_connected() || netModule_is_cat1()) {
        ESP_LOGI(TAG, "NTP Synchronizing");
        if (system_ntp_time(true) == ESP_FAIL) {
            ESP_LOGI(TAG, "NTP Failed");
        }
    }
    sleep_set_last_schedule_time(time(NULL));
    sleep_set_event_bits(SLEEP_SCHEDULE_DONE_BIT);  // Signal completion
}

/**
 * Execute upload tasks
 * Triggers storage upload process for scheduled upload mode
 */
void system_upload_todo()
{
    uploadAttr_t upload;
    esp_err_t ret = cfg_get_upload_attr(&upload);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get upload configuration: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "Upload task - Mode: %d, TimedCount: %d", 
             upload.uploadMode, upload.timedCount);
    
    switch (upload.uploadMode) {
        case 0:
            // Instant upload mode - uploads happen immediately after capture
            ESP_LOGI(TAG, "Instant upload mode - no scheduled action needed");
            break;
            
        case 1:
            // Scheduled upload mode - trigger storage upload
            ESP_LOGI(TAG, "Triggering scheduled storage upload");
            storage_upload_start();
            // Update last upload time
            sleep_set_last_upload_time(time(NULL));
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown upload mode: %d", upload.uploadMode);
            break;
    }
}

esp_err_t system_set_ntp_sync(ntpSync_t *ntp_sync)
{
    cfg_set_ntp_sync(ntp_sync->enable);
    return ESP_OK;
}

esp_err_t system_get_ntp_sync(ntpSync_t *ntp_sync)
{
    cfg_get_ntp_sync(&ntp_sync->enable);
    return ESP_OK;
}

bool system_is_ntp_sync_enable()
{
    ntpSync_t ntp_sync;
    system_get_ntp_sync(&ntp_sync);
    return ntp_sync.enable == 1;
}


/* *
 * Set temporary mode
 * @param mode Mode to set
 */
void system_set_temporary_mode(modeSel_e mode)
{
    g_tmpMode = mode;
}

void system_set_temporary_mode_and_restart(modeSel_e mode)
{
    system_set_temporary_mode(mode);
    system_restart();
}
/**
 * Get temporary mode
 * @return Temporary mode
 */
modeSel_e system_get_temporary_mode(void)
{
    modeSel_e mode = g_tmpMode;
    g_tmpMode = MODE_UNDEFINED;
    return mode;
}
