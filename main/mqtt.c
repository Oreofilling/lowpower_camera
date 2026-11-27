/**
 * MQTT Client Implementation
 */
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_tls_crypto.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "storage.h"
#include "config.h"
#include "system.h"
#include "s2j.h"
#include "misc.h"
#include "mqtt.h"
#include "debug.h"
#include "utils.h"
#include "push.h"
#include "onboarding.h"

#define MQTT_CONNECT_BIT BIT(2)
#define MQTT_DISCONNECT_BIT BIT(3)
#define MQTT_PUBLISHED_BIT BIT(4)
#define MQTT_PUBLISHED_TIMEOUT_MS (20000)

#define TAG "-->MQTT"

typedef struct mdMqtt {
    EventGroupHandle_t eventGroup;
    mqttAttr_t mqtt;
    void *client;
    bool isConnected;
    SemaphoreHandle_t mutex;
    void *sendBuf;
    size_t sendBufSize;
    esp_mqtt_client_config_t cfg;
    bool useOnboardingDefault;
} mdMqtt_t;

static mdMqtt_t g_MQ = {0};
static esp_timer_handle_t s_restart_timer;

static void mqtt_restart_timer_cb(void *arg)
{
    (void)arg;
    if (sleep_is_entering_deep_sleep()) {
        ESP_LOGI(TAG, "System is entering deep sleep, skipping restart");
        return;
    }
    mqtt_restart();
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event, void *handler_args)
{
    mdMqtt_t *mqtt = (mdMqtt_t *)handler_args;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            if (mqtt->useOnboardingDefault) {
                char topic[ONBOARDING_TOPIC_MAX_LEN] = {0};
                if (onboarding_get_default_topic(topic, sizeof(topic)) == ESP_OK) {
                    ESP_LOGI(TAG, "Subscribing to default topic: %s", topic);
                    int msg_id = esp_mqtt_client_subscribe(mqtt->client, topic, 0);
                    if (msg_id < 0) {
                        ESP_LOGE(TAG, "Failed to subscribe to default topic");
                    } else {
                        ESP_LOGI(TAG, "Default topic subscription msg_id=%d", msg_id);
                    }
                }
            } else {
                ESP_LOGI(TAG, "Custom broker, skip onboarding topic subscribe");
                storage_upload_start();
                push_ready();
            }
            mqtt->isConnected = true;
            xEventGroupClearBits(mqtt->eventGroup, MQTT_DISCONNECT_BIT);
            xEventGroupSetBits(mqtt->eventGroup, MQTT_CONNECT_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            xEventGroupClearBits(mqtt->eventGroup, MQTT_CONNECT_BIT);
            xEventGroupSetBits(mqtt->eventGroup, MQTT_DISCONNECT_BIT);
            mqtt->isConnected = false;
            storage_upload_stop();
            push_exit();
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            storage_upload_start();
            push_ready();
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            xEventGroupSetBits(mqtt->eventGroup, MQTT_PUBLISHED_BIT);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            push_exit();
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA, topic=%.*s", event->topic_len, event->topic);
            if (onboarding_is_default_topic(event->topic, event->topic_len)) {
                ESP_LOGI(TAG, "Received config from default topic (%d bytes)", event->data_len);
                bool mqtt_changed = false;
                if (onboarding_handle_config(event->data, event->data_len, &mqtt_changed) == ESP_OK) {
                    if (mqtt_changed) {
                        ESP_LOGI(TAG, "MQTT config changed, restarting with new broker");
                        esp_timer_start_once(s_restart_timer, 1);
                    } else {
                        ESP_LOGI(TAG, "Config applied, no MQTT change needed");
                    }
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    mqtt_event_handler_cb(event_data, handler_args);
}

char *push_build_json_payload(queueNode_t *node)
{
    esp_err_t res = ESP_OK;
    size_t picSize;
    deviceInfo_t device;
    char *snapType = NULL;
    char time_str[32];
    char upload_time_str[32];
    char header[] = "data:image/jpeg;base64,";

    switch (node->type) {
        case SNAP_ALARMIN:
            snapType = "Alarm in";
            break;
        case SNAP_PIR:
            snapType = "PIR";
            break;
        case SNAP_BUTTON:
            snapType = "Button";
            break;
        case SNAP_TIMER:
            snapType = "Timer";
            break;
        default:
            snapType = "Unknown";
            break;
    }

    memcpy((char *)g_MQ.sendBuf, header, strlen(header));
    size_t header_len = strlen(header);
    size_t available_size = g_MQ.sendBufSize - header_len;

    size_t required_size = ((node->len + 2) / 3) * 4;
    if (required_size > available_size) {
        ESP_LOGE(TAG, "Buffer too small: required=%zu, available=%zu", required_size, available_size);
        return NULL;
    }

    res = esp_crypto_base64_encode(g_MQ.sendBuf + header_len, available_size, &picSize, node->data, node->len);
    if (res < 0) {
        ESP_LOGE(TAG, "base64_encode failed");
        return NULL;
    }

    cfg_get_device_info(&device);
    time_t t = node->pts / 1000;
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&t));
    time_t upload_t = time(NULL);
    strftime(upload_time_str, sizeof(upload_time_str), "%Y-%m-%d %H:%M:%S", localtime(&upload_t));

    cJSON *json = cJSON_CreateObject();
    cJSON *subJson = cJSON_CreateObject();
    cJSON_AddStringToObject(subJson, "devName", device.name);
    cJSON_AddStringToObject(subJson, "devMac", device.mac);
    cJSON_AddStringToObject(subJson, "devSn", device.sn);
    cJSON_AddStringToObject(subJson, "hwVersion", device.hardVersion);
    cJSON_AddStringToObject(subJson, "fwVersion", device.softVersion);
    cJSON_AddNumberToObject(subJson, "battery", misc_get_battery_voltage_rate());
    cJSON_AddNumberToObject(subJson, "batteryVoltage", misc_get_battery_voltage());
    cJSON_AddStringToObject(subJson, "snapType", snapType);
    cJSON_AddStringToObject(subJson, "localtime", time_str);
    cJSON_AddStringToObject(subJson, "uploadtime", upload_time_str);
    cJSON_AddNumberToObject(subJson, "imageSize", picSize + strlen(header));
    cJSON_AddStringToObject(subJson, "image", (char *)g_MQ.sendBuf);
    cJSON_AddNumberToObject(json, "ts", node->pts);
    cJSON_AddItemToObject(json, "values", subJson);

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return str;
}

esp_err_t mqtt_publish_node(queueNode_t *node)
{
    mqttAttr_t mqtt;

    if (!g_MQ.isConnected) {
        return ESP_FAIL;
    }

    cfg_get_mqtt_attr(&mqtt);
    if (mqtt.topic[0] == '\0') {
        ESP_LOGE(TAG, "MQTT topic is empty");
        return ESP_FAIL;
    }

    char *json_str = push_build_json_payload(node);
    if (json_str == NULL) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Publishing to topic: %s, qos: %d", mqtt.topic, mqtt.qos);
    int res = esp_mqtt_client_publish(g_MQ.client, mqtt.topic, json_str, 0, mqtt.qos, 0);
    if (mqtt.qos == 0) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    cJSON_free(json_str);

    if (res < 0) {
        return ESP_FAIL;
    }

    if (mqtt.qos == 0) {
        return ESP_OK;
    }

    EventBits_t uxBits = xEventGroupWaitBits(g_MQ.eventGroup, MQTT_PUBLISHED_BIT, true, true,
                                              pdMS_TO_TICKS(MQTT_PUBLISHED_TIMEOUT_MS));
    return (uxBits & MQTT_PUBLISHED_BIT) ? ESP_OK : ESP_FAIL;
}

static void mqtt_esp_config(mdMqtt_t *m)
{
    esp_mqtt_client_config_t *c = &m->cfg;

    memset(c, 0, sizeof(esp_mqtt_client_config_t));
    cfg_get_mqtt_attr(&m->mqtt);
    /* If no user-defined host, fall back to onboarding defaults */
    m->useOnboardingDefault = false;
    if (m->mqtt.host[0] == '\0') {
        snprintf(m->mqtt.host, sizeof(m->mqtt.host), "%s", onboarding_get_default_host());
        m->mqtt.port = onboarding_get_default_port();
        m->useOnboardingDefault = true;
        ESP_LOGI(TAG, "Using default onboarding broker: %s:%lu",
                 m->mqtt.host, (unsigned long)m->mqtt.port);
    }

    strip_mqtt_scheme(m->mqtt.host);
    c->broker.address.hostname = m->mqtt.host;
    c->broker.address.port = m->mqtt.port;
    c->broker.address.transport = m->mqtt.tlsEnable ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
    c->credentials.username = m->mqtt.user;
    c->credentials.client_id = m->mqtt.clientId;
    /* Plain-TCP sessions dispatch from a 4K stack fine; TLS (mbedtls
     * handshake + cert parse) uses esp-mqtt's own default of 6144, which
     * keeps the contiguous-block requirement inside what a fragmented
     * config-mode heap can still provide. */
    c->task.stack_size = m->mqtt.tlsEnable ? (6 * 1024) : (4 * 1024);
    c->network.disable_auto_reconnect = true;
    if (strlen(m->mqtt.password)) {
        c->credentials.authentication.password = m->mqtt.password;
    }
    if (m->mqtt.tlsEnable) {
        if (strlen(m->mqtt.caName)) {
            c->broker.verification.certificate = filesystem_read(MQTT_CA_PATH);
        } else {
            c->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
        if (strlen(m->mqtt.certName) && strlen(m->mqtt.keyName)) {
            c->credentials.authentication.certificate = filesystem_read(MQTT_CERT_PATH);
            c->credentials.authentication.key = filesystem_read(MQTT_KEY_PATH);
        }
        c->network.timeout_ms = 15000;
        c->broker.verification.use_global_ca_store = false;
    }
    ESP_LOGI(TAG, "HOST:%s, USER:%s PSW:%s, PORT:%ld, TLS:%d",
             m->mqtt.host, m->mqtt.user, m->mqtt.password, m->mqtt.port, m->mqtt.tlsEnable);
}

static void mqtt_free_tls_certs(mdMqtt_t *m)
{
    esp_mqtt_client_config_t *c = &m->cfg;

    if (c->broker.verification.certificate) {
        free((void *)c->broker.verification.certificate);
        c->broker.verification.certificate = NULL;
    }
    if (c->credentials.authentication.certificate) {
        free((void *)c->credentials.authentication.certificate);
        c->credentials.authentication.certificate = NULL;
    }
    if (c->credentials.authentication.key) {
        free((void *)c->credentials.authentication.key);
        c->credentials.authentication.key = NULL;
    }
}

static int8_t mqtt_esp_start(mdMqtt_t *m)
{
    mqtt_esp_config(m);
    m->client = esp_mqtt_client_init(&m->cfg);
    if (!m->client) {
        return -1;
    }
    esp_mqtt_client_register_event(m->client, ESP_EVENT_ANY_ID, mqtt_event_handler, m);
    return esp_mqtt_client_start(m->client);
}

static int8_t mqtt_esp_stop(mdMqtt_t *m)
{
    if (!m->client) {
        return -1;
    }
    esp_mqtt_client_disconnect(m->client);
    esp_mqtt_client_stop(m->client);
    esp_mqtt_client_destroy(m->client);
    mqtt_free_tls_certs(m);
    m->client = NULL;
    m->isConnected = false;
    return 0;
}

static int do_sendrate_cmd(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    return ESP_OK;
}

/*
 * One-shot broker connectivity probe: brings up a dedicated client with the
 * saved (or onboarding-default) parameters, reports the verdict, tears it
 * down. Config mode skips the resident client to save RAM next to BLE, so
 * this is the way to answer "can this device reach my broker?".
 *
 * "mqtttest"        - lightweight: raw TCP connect + MQTT CONNECT/CONNACK
 *                     handshake, runs on the console task stack (works in
 *                     config mode where the esp-mqtt task would not fit).
 * "mqtttest full"   - full esp-mqtt client (validates TLS/auth paths); needs
 *                     the RAM headroom of snapshot/schedule mode.
 */
#define MQTTTEST_TIMEOUT_MS 15000
#define MQTTTEST_OK_BIT   BIT(0)
#define MQTTTEST_FAIL_BIT BIT(1)

static void mqtttest_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    if (id == MQTT_EVENT_CONNECTED) {
        xEventGroupSetBits((EventGroupHandle_t)args, MQTTTEST_OK_BIT);
    } else if (id == MQTT_EVENT_DISCONNECTED || id == MQTT_EVENT_ERROR) {
        xEventGroupSetBits((EventGroupHandle_t)args, MQTTTEST_FAIL_BIT);
    }
}

/* Hand-built MQTT v3.1.1 CONNECT on a raw socket; reports the CONNACK verdict. */
static int mqtttest_tcp_probe(mdMqtt_t *m)
{
    static const char cid[] = "ne101-mqtttest";

    struct hostent *he = gethostbyname(m->mqtt.host);
    if (he == NULL || he->h_addr_list[0] == NULL) {
        printf("mqtttest: DNS lookup failed for %s\n", m->mqtt.host);
        return ESP_FAIL;
    }
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t) m->mqtt.port);
    dst.sin_addr = *((struct in_addr *) he->h_addr_list[0]);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("mqtttest: socket failed, errno %d\n", errno);
        return ESP_FAIL;
    }
    fcntl(s, F_SETFL, O_NONBLOCK);
    if (connect(s, (struct sockaddr *) &dst, sizeof(dst)) != 0 && errno != EINPROGRESS) {
        printf("mqtttest: connect to %s:%lu failed, errno %d\n",
               m->mqtt.host, (unsigned long) m->mqtt.port, errno);
        close(s);
        return ESP_FAIL;
    }
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    struct timeval tv = { .tv_sec = 6, .tv_usec = 0 };
    if (select(s + 1, NULL, &wset, NULL, &tv) <= 0) {
        printf("mqtttest: TCP connect to %s:%lu timed out\n",
               m->mqtt.host, (unsigned long) m->mqtt.port);
        close(s);
        return ESP_FAIL;
    }
    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &slen);
    if (soerr != 0) {
        printf("mqtttest: TCP connect to %s:%lu refused, errno %d\n",
               m->mqtt.host, (unsigned long) m->mqtt.port, soerr);
        close(s);
        return ESP_FAIL;
    }

    /* handshake phase: blocking socket, 5s receive timeout */
    fcntl(s, F_SETFL, 0);
    tv.tv_sec = 5;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t pkt[384];
    size_t n = 0;
    pkt[n++] = 0x10; /* CONNECT, remaining length patched below */
    pkt[n++] = 0;    /* remaining length */
    size_t vh = n;
    pkt[n++] = 0x00; pkt[n++] = 0x04;
    memcpy(&pkt[n], "MQTT", 4);
    n += 4;
    pkt[n++] = 0x04;  /* protocol level 4 (v3.1.1) */
    bool has_auth = (m->mqtt.user[0] != '\0');
    pkt[n++] = has_auth ? 0xC2 : 0x02; /* clean session [+ username + password] */
    pkt[n++] = 0x00; pkt[n++] = 0x3C; /* keep-alive 60s */
    /* payload: client id, then optional username and password */
    const char *fields[3] = { cid, m->mqtt.user, m->mqtt.password };
    for (int i = 0; i < (has_auth ? 3 : 1); i++) {
        size_t l = strlen(fields[i]);
        pkt[n++] = (uint8_t) (l >> 8);
        pkt[n++] = (uint8_t) l;
        memcpy(&pkt[n], fields[i], l);
        n += l;
    }
    pkt[vh - 1] = (uint8_t) (n - 2);

    if (send(s, pkt, n, 0) != (int) n) {
        printf("mqtttest: send failed, errno %d\n", errno);
        close(s);
        return ESP_FAIL;
    }
    uint8_t ack[4] = {0};
    size_t got = 0;
    while (got < sizeof(ack)) {
        int r = recv(s, &ack[got], sizeof(ack) - got, 0);
        if (r <= 0) {
            break;
        }
        got += (size_t) r;
    }
    close(s);

    if (got < sizeof(ack) || ack[0] != 0x20) {
        printf("mqtttest: %s:%lu answered TCP but no CONNACK (not an MQTT listener?)\n",
               m->mqtt.host, (unsigned long) m->mqtt.port);
        return ESP_FAIL;
    }
    if (ack[3] == 0) {
        printf("mqtttest: CONNECTED to %s:%lu (broker accepted)\n",
               m->mqtt.host, (unsigned long) m->mqtt.port);
        if (m->mqtt.tlsEnable) {
            printf("mqtttest: note: TLS enabled in config, plaintext probe proved reachability only\n");
        }
        return ESP_OK;
    }
    printf("mqtttest: broker REFUSED, CONNACK code %u"
           " (1=protocol, 2=clientid, 3=unavailable, 4=bad user/pass, 5=not authorized)\n",
           (unsigned) ack[3]);
    return ESP_FAIL;
}

static int do_mqtttest_cmd(int argc, char **argv)
{
    bool full = (argc > 1 && strcmp(argv[1], "full") == 0);

    if (g_MQ.client) {
        printf("mqtttest: resident client is up, connected=%s\n", g_MQ.isConnected ? "yes" : "no");
        return ESP_OK;
    }

    mdMqtt_t m; /* local, no shared state touched */
    memset(&m, 0, sizeof(m));
    mqtt_esp_config(&m);

    if (!full) {
        mqtttest_tcp_probe(&m);
        mqtt_free_tls_certs(&m);
        return ESP_OK;
    }

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&m.cfg);
    if (client == NULL) {
        printf("mqtttest: client init failed (out of memory; try in snapshot/schedule mode)\n");
        mqtt_free_tls_certs(&m);
        return ESP_OK;
    }

    EventGroupHandle_t eg = xEventGroupCreate();
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtttest_event_handler, eg);
    esp_mqtt_client_start(client);

    printf("mqtttest: connecting to %s:%lu ...\n", m.mqtt.host, (unsigned long) m.mqtt.port);
    EventBits_t bits = xEventGroupWaitBits(eg, MQTTTEST_OK_BIT | MQTTTEST_FAIL_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(MQTTTEST_TIMEOUT_MS));
    if (bits & MQTTTEST_OK_BIT) {
        printf("mqtttest: CONNECTED to %s:%lu\n", m.mqtt.host, (unsigned long) m.mqtt.port);
    } else if (bits & MQTTTEST_FAIL_BIT) {
        printf("mqtttest: FAILED to connect to %s:%lu (refused or TLS error, see logs)\n",
               m.mqtt.host, (unsigned long) m.mqtt.port);
    } else {
        printf("mqtttest: TIMEOUT after %d ms (host unreachable?)\n", MQTTTEST_TIMEOUT_MS);
    }

    esp_mqtt_client_disconnect(client);
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    mqtt_free_tls_certs(&m);
    vEventGroupDelete(eg);
    return ESP_OK;
}

static esp_console_cmd_t g_cmd[] = {
    ESP_CONSOLE_CMD_INIT("sendrate", "mqtt send success rate", NULL, do_sendrate_cmd, NULL),
    ESP_CONSOLE_CMD_INIT("mqtttest", "one-shot broker connect test", NULL, do_mqtttest_cmd, NULL),
};

void mqtt_open(void)
{
    memset(&g_MQ, 0, sizeof(mdMqtt_t));
    const esp_timer_create_args_t restart_timer_args = {
        .callback = mqtt_restart_timer_cb,
        .name = "mqtt_restart",
    };
    ESP_ERROR_CHECK(esp_timer_create(&restart_timer_args, &s_restart_timer));
    g_MQ.eventGroup = xEventGroupCreate();
    g_MQ.mutex = xSemaphoreCreateMutex();
    g_MQ.sendBuf = malloc(PUSH_SEND_BUFFER_SIZE);
    assert(g_MQ.sendBuf);
    g_MQ.sendBufSize = PUSH_SEND_BUFFER_SIZE;
    debug_cmd_add(g_cmd, sizeof(g_cmd) / sizeof(esp_console_cmd_t));
}

void mqtt_start(void)
{
    xSemaphoreTake(g_MQ.mutex, portMAX_DELAY);
    if (g_MQ.client) {
        xSemaphoreGive(g_MQ.mutex);
        return;
    }
    ESP_LOGI(TAG, "mqtt esp start");
    mqtt_esp_start(&g_MQ);
    xSemaphoreGive(g_MQ.mutex);
}

void mqtt_stop(void)
{
    if (s_restart_timer) {
        esp_timer_stop(s_restart_timer);
    }
    xSemaphoreTake(g_MQ.mutex, portMAX_DELAY);
    if (!g_MQ.client) {
        xSemaphoreGive(g_MQ.mutex);
        return;
    }
    mqtt_esp_stop(&g_MQ);
    xSemaphoreGive(g_MQ.mutex);
    ESP_LOGI(TAG, "esp_mqtt_client_stop");
}

void mqtt_restart(void)
{
    xSemaphoreTake(g_MQ.mutex, portMAX_DELAY);
    if (g_MQ.client) {
        mqtt_esp_stop(&g_MQ);
    }
    ESP_LOGI(TAG, "mqtt esp restart");
    mqtt_esp_start(&g_MQ);
    xSemaphoreGive(g_MQ.mutex);
}

void mqtt_close(void)
{
    if (s_restart_timer) {
        esp_timer_stop(s_restart_timer);
        esp_timer_delete(s_restart_timer);
        s_restart_timer = NULL;
    }
    xSemaphoreTake(g_MQ.mutex, portMAX_DELAY);
    if (g_MQ.client) {
        mqtt_esp_stop(&g_MQ);
    }
    xSemaphoreGive(g_MQ.mutex);
    if (g_MQ.sendBuf) {
        free(g_MQ.sendBuf);
        g_MQ.sendBuf = NULL;
    }
}

bool mqtt_is_connected(void)
{
    return g_MQ.isConnected;
}
