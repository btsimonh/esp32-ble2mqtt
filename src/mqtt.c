#include "mqtt.h"
#include <esp_err.h>
#include <esp_log.h>
#include <esp_mqtt.h>
#include <string.h>

/* Constants */
static const char *TAG = "MQTT";

static char *prefix = NULL;

/* Types */
typedef struct mqtt_subscription_t {
    struct mqtt_subscription_t *next;
    char *topic;
    mqtt_on_message_received_cb_t cb;
    void *ctx;
    mqtt_free_ctx_cb_t free_cb;
} mqtt_subscription_t;

typedef struct mqtt_publications_t {
    struct mqtt_publications_t *next;
    char *topic;
    uint8_t *payload;
    size_t len;
    int qos;
    uint8_t retained;
} mqtt_publications_t;

/* Internal state */
static mqtt_subscription_t *subscription_list = NULL;
static mqtt_publications_t *publications_list = NULL;
static uint8_t is_connected = 0;

/* Callback functions */
static mqtt_on_connected_cb_t on_connected_cb = NULL;
static mqtt_on_disconnected_cb_t on_disconnected_cb = NULL;

static char *mqtt_add_prefix( char *topic ){
    if (!prefix || !prefix[0]) 
        return topic;

    char *prefixed_topic = malloc(strlen(prefix)+strlen(topic)+1);
    strcpy( prefixed_topic, prefix );
    strcat( prefixed_topic, topic );
    free(topic);
    return prefixed_topic;
}

static char *mqtt_remove_prefix( char *topic ){
    if (!prefix || !prefix[0]) 
        return topic;

    if (memcmp(topic, prefix, strlen(prefix))){
        char *unprefixed_topic = strdup( topic + strlen(prefix));
        free(topic);
        return unprefixed_topic;
    }
    return topic;
}
    

void mqtt_set_on_connected_cb(mqtt_on_connected_cb_t cb)
{
    on_connected_cb = cb;
}

void mqtt_set_on_disconnected_cb(mqtt_on_disconnected_cb_t cb)
{
    on_disconnected_cb = cb;
}

static mqtt_subscription_t *mqtt_subscription_add(mqtt_subscription_t **list,
    const char *topic, mqtt_on_message_received_cb_t cb, void *ctx,
    mqtt_free_ctx_cb_t free_cb)
{
    mqtt_subscription_t *sub, **cur;

    sub = malloc(sizeof(*sub));
    sub->next = NULL;
    sub->topic = strdup(topic);
    sub->cb = cb;
    sub->ctx = ctx;
    sub->free_cb = free_cb;

    for (cur = list; *cur; cur = &(*cur)->next);
    *cur = sub;

    return sub;
}

static void mqtt_subscription_free(mqtt_subscription_t *mqtt_subscription)
{
    if (mqtt_subscription->ctx && mqtt_subscription->free_cb)
        mqtt_subscription->free_cb(mqtt_subscription->ctx);
    free(mqtt_subscription->topic);
    free(mqtt_subscription);
}

static void mqtt_subscriptions_free(mqtt_subscription_t **list)
{
    mqtt_subscription_t *cur, **head = list;

    while (*list)
    {
        cur = *list;
        *list = cur->next;
        mqtt_subscription_free(cur);
    }
    *head = NULL;
}

static void mqtt_subscription_remove(mqtt_subscription_t **list,
    const char *topic)
{
    mqtt_subscription_t **cur, *tmp;
    size_t len = strlen(topic);

    for (cur = list; *cur; cur = &(*cur)->next)
    {
        if (!strncmp((*cur)->topic, topic, len))
            break;
    }

    if (!*cur)
        return;

    tmp = *cur;
    *cur = (*cur)->next;
    mqtt_subscription_free(tmp);
}

static mqtt_publications_t *mqtt_publication_add(mqtt_publications_t **list,
    const char *topic, uint8_t *payload, size_t len, int qos, uint8_t retained)
{
    mqtt_publications_t *pub = malloc(sizeof(*pub));

    pub->topic = strdup(topic);
    pub->payload = malloc(len);
    memcpy(pub->payload, payload, len);
    pub->len = len;
    pub->qos = qos;
    pub->retained = retained;

    pub->next = *list;
    *list = pub;

    return pub;
}

static void mqtt_publication_free(mqtt_publications_t *mqtt_publication)
{
    free(mqtt_publication->topic);
    free(mqtt_publication->payload);
    free(mqtt_publication);
}

static void mqtt_publications_free(mqtt_publications_t **list)
{
    mqtt_publications_t *cur, **head = list;

    while (*list)
    {
        cur = *list;
        *list = cur->next;
        mqtt_publication_free(cur);
    }
    *head = NULL;
}

static void mqtt_publications_publish(mqtt_publications_t *list)
{
    for (; list; list = list->next)
    {
        ESP_LOGI(TAG, "Publishing from queue: %s = %.*s", list->topic,
            list->len, list->payload);

        mqtt_publish(list->topic, list->payload, list->len, list->qos,
            list->retained);
    }
}

int mqtt_subscribe(const char *topic_in, int qos, mqtt_on_message_received_cb_t cb,
    void *ctx, mqtt_free_ctx_cb_t free_cb)
{
    uint8_t retries = 0;

    const char *topic = mqtt_add_prefix(topic_in);

    if (!is_connected)
        return -1;

    ESP_LOGD(TAG, "Subscribing to %s", topic);
    while ((esp_mqtt_subscribe(topic, qos) != true) && retries < 3)
    {
        ESP_LOGI(TAG, "Failed subscribing to %s (retries: %u), trying again...",
            topic, retries);
        retries++;
    }

    if (retries == 3)
    {
        ESP_LOGE(TAG, "Failed subscribing to %s", topic);
        return -1;
    }

    mqtt_subscription_add(&subscription_list, topic, cb, ctx, free_cb);
    return 0;
}

int mqtt_unsubscribe(const char *topic_in)
{
    const char *topic = mqtt_add_prefix(topic_in);

    ESP_LOGD(TAG, "Unsubscribing from %s", topic);
    mqtt_subscription_remove(&subscription_list, topic);

    if (!is_connected)
        return 0;

    return esp_mqtt_unsubscribe(topic);
}

int mqtt_publish(const char *topic_in, uint8_t *payload, size_t len, int qos,
    uint8_t retained)
{
    const char *topic = mqtt_add_prefix(topic_in);

    if (is_connected)
        return esp_mqtt_publish(topic, payload, len, qos, retained) != true;

    /* If we're currently not connected, queue publication */
    ESP_LOGD(TAG, "MQTT is disconnected, adding publication to queue...");
    mqtt_publication_add(&publications_list, topic, payload, len, qos,
        retained);

    return 0;
}

static void mqtt_status_cb(esp_mqtt_status_t status)
{
    switch (status) {
    case ESP_MQTT_STATUS_CONNECTED:
        ESP_LOGI(TAG, "MQTT client connected");
        is_connected = 1;
        mqtt_publications_publish(publications_list);
        mqtt_publications_free(&publications_list);
        if (on_connected_cb)
            on_connected_cb();
        break;
    case ESP_MQTT_STATUS_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT client disconnected");
        is_connected = 0;
        mqtt_subscriptions_free(&subscription_list);
        if (on_disconnected_cb)
            on_disconnected_cb();
        break;
    }
}

static void mqtt_message_cb(const char *topic_in, uint8_t *payload, size_t len)
{
    mqtt_subscription_t *cur;

    ESP_LOGD(TAG, "Recevied: %s => %s (%d)\n", topic_in, payload, (int)len);

    for (cur = subscription_list; cur; cur = cur->next)
    {
        /* TODO: Correctly match MQTT topics (i.e. support wildcards) */
        if (strcmp(cur->topic, topic_in))
            continue;

        const char *topic = mqtt_remove_prefix(topic_in);
        cur->cb(topic, payload, len, cur->ctx);
    }
}

int mqtt_connect(const char *host, uint16_t port, const char *client_id,
    const char *username, const char *password, const char *prefix_in)
{
    if (prefix)
        free(prefix);
    if (prefix_in)
        prefix = prefix_in; // note - malloced in cJSON?
    else
        prefix = NULL;

    ESP_LOGI(TAG, "Connecting MQTT client");
    esp_mqtt_start(host, port, client_id, username, password);
    return 0;
}

int mqtt_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting MQTT client");
    is_connected = 0;
    esp_mqtt_stop();
    return 0;
}

int mqtt_initialize(void)
{
    ESP_LOGD(TAG, "Initializing MQTT client");
    esp_mqtt_init(mqtt_status_cb, mqtt_message_cb, 256, 2000);
    return 0;
}
