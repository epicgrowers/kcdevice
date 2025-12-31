#include "provisioning/provisioning_wifi_ops.h"

#include "provisioning/wifi_manager.h"

static esp_err_t wifi_init_impl(void)
{
    return wifi_manager_init();
}

static esp_err_t wifi_connect_impl(const char *ssid, const char *password)
{
    return wifi_manager_connect(ssid, password);
}

static bool wifi_is_connected_impl(void)
{
    return wifi_manager_is_connected();
}

static esp_err_t wifi_get_stored_impl(char *ssid, char *password)
{
    return wifi_manager_get_stored_credentials(ssid, password);
}

static esp_err_t wifi_disconnect_impl(void)
{
    return wifi_manager_disconnect();
}

static esp_err_t wifi_clear_impl(void)
{
    return wifi_manager_clear_credentials();
}

static const provisioning_wifi_ops_t s_default_wifi_ops = {
    .init = wifi_init_impl,
    .connect = wifi_connect_impl,
    .is_connected = wifi_is_connected_impl,
    .get_stored_credentials = wifi_get_stored_impl,
    .disconnect = wifi_disconnect_impl,
    .clear_credentials = wifi_clear_impl,
};

const provisioning_wifi_ops_t *provisioning_wifi_ops_default(void)
{
    return &s_default_wifi_ops;
}
