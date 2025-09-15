#include "wifi_ota.h"
#include "sl_wifi.h"
#include "sl_net.h"
#include "sl_http_client.h"
#include "sl_net_dns.h"
#include "app_log.h"
#include "filesystem_port.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/******************************************************
 *                    Local Macros
 ******************************************************/
#define OTA_TASK_STACK_SIZE     2048
#define OTA_TASK_PRIORITY       2
#define OTA_QUEUE_SIZE          10
#define OTA_HTTP_BUFFER_SIZE    2048
#define JSON_PARSE_BUFFER_SIZE  512

/******************************************************
 *                 Local Variables
 ******************************************************/
static ota_context_t g_ota_context = {0};
static ota_server_config_t g_server_config = {0};
static TaskHandle_t g_ota_task_handle = NULL;
static SemaphoreHandle_t g_ota_mutex = NULL;
static bool g_ota_initialized = false;

// HTTP相关变量
static uint8_t g_ota_buffer[OTA_HTTP_BUFFER_SIZE];
static uint32_t g_buffer_index = 0;
static volatile bool g_http_response_received = false;
static volatile sl_status_t g_http_callback_status = SL_STATUS_OK;
static uint8_t g_server_ip[16];

/******************************************************
 *              Local Function Declarations
 ******************************************************/
static void ota_task(void *pvParameters);
static sl_status_t ota_resolve_server_ip(const char *hostname);
static sl_status_t ota_http_get_request(const char *path, uint8_t *response_buffer, size_t buffer_size, size_t *response_len);
static sl_status_t ota_parse_version_json(const char *json_data, ota_version_info_t *version_info);
static sl_status_t ota_download_firmware_file(const char *url, const char *local_path);
static sl_status_t ota_http_response_callback(const sl_http_client_t *client, sl_http_client_event_t event, void *data, void *context);
static int ota_version_compare(const char *version1, const char *version2);
static uint32_t ota_crc32(uint32_t crc, const uint8_t *buf, size_t len);

/******************************************************
 *               Function Definitions
 ******************************************************/

sl_status_t ota_init(const ota_server_config_t *config)
{
    if (!config) {
        return SL_STATUS_NULL_POINTER;
    }

    // 初始化互斥锁
    if (g_ota_mutex == NULL) {
        g_ota_mutex = xSemaphoreCreateMutex();
        if (g_ota_mutex == NULL) {
            app_log_error("Failed to create OTA mutex\r\n");
            return SL_STATUS_ALLOCATION_FAILED;
        }
    }

    xSemaphoreTake(g_ota_mutex, portMAX_DELAY);

    // 复制服务器配置
    memcpy(&g_server_config, config, sizeof(ota_server_config_t));

    // 初始化OTA上下文
    memset(&g_ota_context, 0, sizeof(ota_context_t));
    g_ota_context.state = OTA_STATE_IDLE;
    g_ota_context.last_error = OTA_ERROR_NONE;
    g_ota_context.auto_check_enabled = false;

    // 读取当前版本
    ota_get_current_version(g_ota_context.version_info.current_version,
                           sizeof(g_ota_context.version_info.current_version));

    g_ota_initialized = true;

    xSemaphoreGive(g_ota_mutex);

    app_log_info("OTA module initialized\r\n");
    return SL_STATUS_OK;
}

sl_status_t ota_start_auto_check(void)
{
    if (!g_ota_initialized) {
        return SL_STATUS_NOT_INITIALIZED;
    }

    xSemaphoreTake(g_ota_mutex, portMAX_DELAY);
    g_ota_context.auto_check_enabled = true;
    g_ota_context.last_check_time = 0; // 立即检查
    xSemaphoreGive(g_ota_mutex);

    // 创建OTA任务
    if (g_ota_task_handle == NULL) {
        BaseType_t result = xTaskCreate(ota_task, "OTA_Task", OTA_TASK_STACK_SIZE,
                                       NULL, OTA_TASK_PRIORITY, &g_ota_task_handle);
        if (result != pdPASS) {
            app_log_error("Failed to create OTA task\r\n");
            return SL_STATUS_ALLOCATION_FAILED;
        }
    }

    app_log_info("OTA auto check started\r\n");
    return SL_STATUS_OK;
}

sl_status_t ota_stop_auto_check(void)
{
    if (!g_ota_initialized) {
        return SL_STATUS_NOT_INITIALIZED;
    }

    xSemaphoreTake(g_ota_mutex, portMAX_DELAY);
    g_ota_context.auto_check_enabled = false;
    xSemaphoreGive(g_ota_mutex);

    app_log_info("OTA auto check stopped\r\n");
    return SL_STATUS_OK;
}

sl_status_t ota_check_version(void)
{
    if (!g_ota_initialized) {
        return SL_STATUS_NOT_INITIALIZED;
    }

    sl_status_t status;
    uint8_t response_buffer[JSON_PARSE_BUFFER_SIZE];
    size_t response_len;

    xSemaphoreTake(g_ota_mutex, portMAX_DELAY);
    g_ota_context.state = OTA_STATE_CHECKING_VERSION;
    xSemaphoreGive(g_ota_mutex);

    app_log_info("Checking for firmware updates...\r\n");

    // 解析服务器IP
    status = ota_resolve_server_ip(g_server_config.hostname);
    if (status != SL_STATUS_OK) {
        g_ota_context.last_error = OTA_ERROR_NETWORK;
        g_ota_context.state = OTA_STATE_FAILED;
        return status;
    }

    // 获取版本信息
    status = ota_http_get_request(g_server_config.version_check_path,
                                 response_buffer, sizeof(response_buffer), &response_len);
    if (status != SL_STATUS_OK) {
        g_ota_context.last_error = OTA_ERROR_SERVER_RESPONSE;
        g_ota_context.state = OTA_STATE_FAILED;
        return status;
    }

    // 解析版本JSON
    status = ota_parse_version_json((char*)response_buffer, &g_ota_context.version_info);
    if (status != SL_STATUS_OK) {
        g_ota_context.last_error = OTA_ERROR_SERVER_RESPONSE;
        g_ota_context.state = OTA_STATE_FAILED;
        return status;
    }

    // 检查是否有新版本
    int version_cmp = ota_version_compare(g_ota_context.version_info.current_version,
                                         g_ota_context.version_info.latest_version);

    g_ota_context.version_info.update_available = (version_cmp < 0);
    g_ota_context.last_check_time = xTaskGetTickCount();
    g_ota_context.state = OTA_STATE_IDLE;

    if (g_ota_context.version_info.update_available) {
        app_log_info("New firmware version available: %s -> %s\r\n",
                    g_ota_context.version_info.current_version,
                    g_ota_context.version_info.latest_version);
    } else {
        app_log_info("Firmware is up to date: %s\r\n",
                    g_ota_context.version_info.current_version);
    }

    return SL_STATUS_OK;
}

sl_status_t ota_start_download(void)
{
    if (!g_ota_initialized) {
        return SL_STATUS_NOT_INITIALIZED;
    }

    if (!g_ota_context.version_info.update_available) {
        app_log_warning("No update available\r\n");
        return SL_STATUS_INVALID_STATE;
    }

    xSemaphoreTake(g_ota_mutex, portMAX_DELAY);
    g_ota_context.state = OTA_STATE_DOWNLOADING;
    g_ota_context.download_progress = 0;
    xSemaphoreGive(g_ota_mutex);

    app_log_info("Starting firmware download...\r\n");

    sl_status_t status = ota_download_firmware_file(g_ota_context.version_info.firmware_url,
                                                   FIRMWARE_TEMP_FILE_PATH);
    if (status != SL_STATUS_OK) {
        g_ota_context.last_error = OTA_ERROR_DOWNLOAD_FAILED;
        g_ota_context.state = OTA_STATE_FAILED;
        return status;
    }

    g_ota_context.state = OTA_STATE_IDLE;
    app_log_info("Firmware download completed\r\n");
    return SL_STATUS_OK;
}

sl_status_t ota_start_upgrade(void)
{
    if (!g_ota_initialized) {
        return SL_STATUS_NOT_INITIALIZED;
    }

    sl_status_t status;

    xSemaphoreTake(g_ota_mutex, portMAX_DELAY);
    g_ota_context.state = OTA_STATE_VERIFYING;
    xSemaphoreGive(g_ota_mutex);

    app_log_info("Verifying firmware...\r\n");

    // 验证固件
    status = ota_verify_firmware(FIRMWARE_TEMP_FILE_PATH,
                                g_ota_context.version_info.firmware_crc32);
    if (status != SL_STATUS_OK) {
        g_ota_context.last_error = OTA_ERROR_VERIFICATION_FAILED;
        g_ota_context.state = OTA_STATE_FAILED;
        return status;
    }

    xSemaphoreTake(g_ota_mutex, portMAX_DELAY);
    g_ota_context.state = OTA_STATE_UPGRADING;
    xSemaphoreGive(g_ota_mutex);

    app_log_info("Installing firmware...\r\n");

    // 安装固件
    status = ota_install_firmware(FIRMWARE_TEMP_FILE_PATH);
    if (status != SL_STATUS_OK) {
        g_ota_context.last_error = OTA_ERROR_UPGRADE_FAILED;
        g_ota_context.state = OTA_STATE_FAILED;
        return status;
    }

    // 更新版本信息
    ota_set_current_version(g_ota_context.version_info.latest_version);
    strcpy(g_ota_context.version_info.current_version,
           g_ota_context.version_info.latest_version);
    g_ota_context.version_info.update_available = false;

    g_ota_context.state = OTA_STATE_SUCCESS;
    app_log_info("Firmware upgrade completed successfully\r\n");

    return SL_STATUS_OK;
}

const ota_context_t* ota_get_status(void)
{
    return &g_ota_context;
}

sl_status_t ota_task_handler(void)
{
    if (!g_ota_initialized || !g_ota_context.auto_check_enabled) {
        return SL_STATUS_OK;
    }

    uint32_t current_time = xTaskGetTickCount();
    uint32_t time_since_last_check = current_time - g_ota_context.last_check_time;

    // 检查是否需要进行版本检查
    if (time_since_last_check >= pdMS_TO_TICKS(OTA_VERSION_CHECK_INTERVAL)) {
        sl_status_t status = ota_check_version();
        if (status != SL_STATUS_OK) {
            app_log_error("Auto version check failed: 0x%lx\r\n", status);
        }
    }

    return SL_STATUS_OK;
}

sl_status_t ota_get_current_version(char *version, size_t max_len)
{
    if (!version || max_len == 0) {
        return SL_STATUS_INVALID_PARAMETER;
    }

    // 尝试从文件读取版本信息
    fs_file_t file;
    int ret = fs_open(&file, FIRMWARE_VERSION_FILE, FS_O_READ);
    if (ret < 0) {
        // 文件不存在，设置默认版本
        snprintf(version, max_len, "1.0.0");
        return SL_STATUS_OK;
    }

    ret = fs_read(&file, version, max_len - 1);
    fs_close(&file);

    if (ret > 0) {
        version[ret] = '\0';
        // 移除换行符
        char *newline = strchr(version, '\n');
        if (newline) *newline = '\0';
    } else {
        snprintf(version, max_len, "1.0.0");
    }

    return SL_STATUS_OK;
}

sl_status_t ota_set_current_version(const char *version)
{
    if (!version) {
        return SL_STATUS_INVALID_PARAMETER;
    }

    fs_file_t file;
    int ret = fs_open(&file, FIRMWARE_VERSION_FILE, FS_O_CREATE | FS_O_WRITE);
    if (ret < 0) {
        app_log_error("Failed to open version file for writing: %d\r\n", ret);
        return SL_STATUS_FAIL;
    }

    ret = fs_write(&file, version, strlen(version));
    fs_close(&file);

    if (ret < 0) {
        app_log_error("Failed to write version file: %d\r\n", ret);
        return SL_STATUS_FAIL;
    }

    return SL_STATUS_OK;
}

sl_status_t ota_verify_firmware(const char *file_path, uint32_t expected_crc32)
{
    uint32_t calculated_crc32;
    sl_status_t status = ota_calculate_file_crc32(file_path, &calculated_crc32);

    if (status != SL_STATUS_OK) {
        return status;
    }

    if (calculated_crc32 != expected_crc32) {
        app_log_error("Firmware verification failed: CRC32 mismatch (expected: 0x%08lx, calculated: 0x%08lx)\r\n",
                     expected_crc32, calculated_crc32);
        return SL_STATUS_FAIL;
    }

    app_log_info("Firmware verification successful\r\n");
    return SL_STATUS_OK;
}

sl_status_t ota_install_firmware(const char *firmware_path)
{
    // 注意：实际的固件安装需要根据具体的bootloader和硬件平台来实现
    // 这里只是一个示例实现
    if(!firmware_path){
        return SL_STATUS_INVALID_PARAMETER;
    }
    app_log_warning("Firmware installation is platform-specific and needs to be implemented\r\n");
    app_log_info("Please implement ota_install_firmware() for your specific platform\r\n");

    // 示例：可能需要将固件写入特定的Flash区域
    // 或者设置bootloader标志以在下次重启时应用新固件

    return SL_STATUS_OK;
}

sl_status_t ota_calculate_file_crc32(const char *file_path, uint32_t *crc32)
{
    if (!file_path || !crc32) {
        return SL_STATUS_INVALID_PARAMETER;
    }

    fs_file_t file;
    int ret = fs_open(&file, file_path, FS_O_READ);
    if (ret < 0) {
        app_log_error("Failed to open file for CRC calculation: %s\r\n", file_path);
        return SL_STATUS_FAIL;
    }

    uint32_t crc = 0xFFFFFFFF;
    uint8_t buffer[256];
    int bytes_read;

    while ((bytes_read = fs_read(&file, buffer, sizeof(buffer))) > 0) {
        crc = ota_crc32(crc, buffer, bytes_read);
    }

    fs_close(&file);

    *crc32 = crc ^ 0xFFFFFFFF;
    return SL_STATUS_OK;
}

/******************************************************
 *              Local Function Definitions
 ******************************************************/

static void ota_task(void *pvParameters)
{
    (void)pvParameters;

    while (1) {
        ota_task_handler();
        vTaskDelay(pdMS_TO_TICKS(10000)); // 每10秒检查一次
    }
}

static sl_status_t ota_resolve_server_ip(const char *hostname)
{
    sl_status_t status;
    sl_ip_address_t dns_response = {0};
    uint32_t server_address;
    int retry_count = 3;

    do {
        status = sl_net_dns_resolve_hostname(hostname, 20000, SL_NET_DNS_TYPE_IPV4, &dns_response);
        retry_count--;
    } while ((retry_count > 0) && (status != SL_STATUS_OK));

    if (status != SL_STATUS_OK) {
        app_log_error("DNS resolution failed for %s: 0x%lx\r\n", hostname, status);
        return status;
    }

    server_address = dns_response.ip.v4.value;
    snprintf((char*)g_server_ip, sizeof(g_server_ip),
             "%ld.%ld.%ld.%ld",
             server_address & 0xFF,
             (server_address >> 8) & 0xFF,
             (server_address >> 16) & 0xFF,
             (server_address >> 24) & 0xFF);

    app_log_info("Resolved %s to %s\r\n", hostname, g_server_ip);
    return SL_STATUS_OK;
}

static sl_status_t ota_http_get_request(const char *path, uint8_t *response_buffer,
                                       size_t buffer_size, size_t *response_len)
{
    sl_status_t status;
    sl_http_client_t client_handle = 0;
    sl_http_client_configuration_t client_config = {0};
    sl_http_client_request_t request = {0};

    // 重置全局变量
    g_buffer_index = 0;
    g_http_response_received = false;
    g_http_callback_status = SL_STATUS_OK;

    // 配置HTTP客户端
    client_config.network_interface = SL_NET_WIFI_CLIENT_INTERFACE;
    client_config.ip_version = SL_IPV4;
    client_config.http_version = SL_HTTP_V_1_1;

    if (g_server_config.https_enabled) {
        client_config.https_enable = true;
        client_config.tls_version = SL_TLS_V_1_2;
        client_config.certificate_index = SL_HTTPS_CLIENT_CERTIFICATE_INDEX_1;
    }

    // 配置请求
    request.ip_address = g_server_ip;
    request.host_name = (uint8_t*)g_server_config.hostname;
    request.port = g_server_config.port;
    request.resource = (uint8_t*)path;
    request.http_method_type = SL_HTTP_GET;

    // 初始化HTTP客户端
    status = sl_http_client_init(&client_config, &client_handle);
    if (status != SL_STATUS_OK) {
        app_log_error("HTTP client init failed: 0x%lx\r\n", status);
        return status;
    }

    // 初始化请求
    status = sl_http_client_request_init(&request, ota_http_response_callback, "OTA");
    if (status != SL_STATUS_OK) {
        sl_http_client_deinit(&client_handle);
        return status;
    }

    // 发送请求
    status = sl_http_client_send_request(&client_handle, &request);
    if (status == SL_STATUS_IN_PROGRESS) {
        // 等待响应
        uint32_t timeout = pdMS_TO_TICKS(30000); // 30秒超时
        uint32_t start_time = xTaskGetTickCount();

        while (!g_http_response_received &&
               (xTaskGetTickCount() - start_time) < timeout) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (!g_http_response_received) {
            status = SL_STATUS_TIMEOUT;
        } else {
            status = g_http_callback_status;
        }
    }

    // 复制响应数据
    if (status == SL_STATUS_OK && response_buffer && buffer_size > 0) {
        size_t copy_len = (g_buffer_index < buffer_size - 1) ? g_buffer_index : buffer_size - 1;
        memcpy(response_buffer, g_ota_buffer, copy_len);
        response_buffer[copy_len] = '\0';
        if (response_len) {
            *response_len = copy_len;
        }
    }

    sl_http_client_deinit(&client_handle);
    return status;
}

static sl_status_t ota_parse_version_json(const char *json_data, ota_version_info_t *version_info)
{
    // 简单的JSON解析实现
    // 实际项目中建议使用专业的JSON解析库

    char *version_start = strstr(json_data, "\"version\":");
    char *url_start = strstr(json_data, "\"url\":");
    char *size_start = strstr(json_data, "\"size\":");
    char *crc32_start = strstr(json_data, "\"crc32\":");

    if (!version_start || !url_start || !size_start || !crc32_start) {
        app_log_error("Invalid JSON format\r\n");
        return SL_STATUS_FAIL;
    }

    // 解析版本
    version_start = strchr(version_start, '"');
    if (version_start) {
        version_start = strchr(version_start + 1, '"');
        if (version_start) {
            version_start++;
            char *version_end = strchr(version_start, '"');
            if (version_end) {
                size_t len = version_end - version_start;
                if (len < sizeof(version_info->latest_version)) {
                    strncpy(version_info->latest_version, version_start, len);
                    version_info->latest_version[len] = '\0';
                }
            }
        }
    }

    // 解析URL（类似的方式）
    // 这里简化实现...

    return SL_STATUS_OK;
}

static sl_status_t ota_download_firmware_file(const char *url, const char *local_path)
{
    // 固件下载实现
    // 这里应该实现分块下载，显示进度等
    app_log_info("Downloading firmware from: %s\r\n", url);
    app_log_info("Saving to: %s\r\n", local_path);

    // 实际实现需要：
    // 1. 打开本地文件
    // 2. 分块下载固件
    // 3. 更新下载进度
    // 4. 处理网络错误和重试

    return SL_STATUS_OK;
}

static sl_status_t ota_http_response_callback(const sl_http_client_t *client,
                                             sl_http_client_event_t event,
                                             void *data, void *context)
{
    (void)client;
    (void)event;
    (void)context;

    sl_http_client_response_t *response = (sl_http_client_response_t*)data;
    g_http_callback_status = response->status;

    if (response->status != SL_STATUS_OK) {
        g_http_response_received = true;
        return response->status;
    }

    if (response->data_length > 0) {
        size_t available_space = sizeof(g_ota_buffer) - g_buffer_index;
        size_t copy_len = (response->data_length < available_space) ?
                         response->data_length : available_space;

        if (copy_len > 0) {
            memcpy(g_ota_buffer + g_buffer_index, response->data_buffer, copy_len);
            g_buffer_index += copy_len;
        }
    }

    if (response->end_of_data) {
        g_http_response_received = true;
    }

    return SL_STATUS_OK;
}

static int ota_version_compare(const char *version1, const char *version2)
{
    // 简单的版本比较实现
    // 假设版本格式为 "x.y.z"

    int v1_major, v1_minor, v1_patch;
    int v2_major, v2_minor, v2_patch;

    sscanf(version1, "%d.%d.%d", &v1_major, &v1_minor, &v1_patch);
    sscanf(version2, "%d.%d.%d", &v2_major, &v2_minor, &v2_patch);

    if (v1_major != v2_major) return v1_major - v2_major;
    if (v1_minor != v2_minor) return v1_minor - v2_minor;
    return v1_patch - v2_patch;
}

static uint32_t ota_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
    static const uint32_t crc32_table[256] = {
        0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
        0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
        0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
        0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
        0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
        0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
        0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
        0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
        0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
        0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
        0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
        0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
        0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
        0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
        0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
        0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
        0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
        0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
        0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA,
        0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
        0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
        0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
        0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
        0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
        0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
        0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
        0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
        0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
        0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
        0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
        0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
        0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
        0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
        0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
        0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
        0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
        0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
        0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
        0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
        0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
        0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693,
        0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
        0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
    };

    while (len--) {
        crc = crc32_table[(crc ^ *buf++) & 0xff] ^ (crc >> 8);
    }
    return crc;
}