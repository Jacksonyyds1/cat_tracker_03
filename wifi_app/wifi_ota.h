#ifndef WIFI_OTA_H
#define WIFI_OTA_H

#include "sl_status.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/******************************************************
 *                      Macros
 ******************************************************/
// OTA配置
#define OTA_MAX_URL_LENGTH          256
#define OTA_MAX_VERSION_LENGTH      32
#define OTA_MAX_HOSTNAME_LENGTH     128
#define OTA_FIRMWARE_CHUNK_SIZE     1024
#define OTA_VERSION_CHECK_INTERVAL  (60 * 60 * 1000) // 1小时检查一次
#define OTA_DOWNLOAD_TIMEOUT        (5 * 60 * 1000)  // 5分钟下载超时

// AWS S3配置
#define AWS_S3_HOSTNAME             "your-bucket.s3.amazonaws.com"
#define AWS_S3_VERSION_FILE_PATH    "/firmware/version.json"
#define AWS_S3_FIRMWARE_PATH        "/firmware/"
#define AWS_S3_PORT                 443

// 固件存储配置
#define FIRMWARE_TEMP_FILE_PATH     "/firmware_temp.bin"
#define FIRMWARE_VERSION_FILE       "/current_version.txt"

/******************************************************
 *                    Enumerations
 ******************************************************/
typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING_VERSION,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_UPGRADING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED
} ota_state_t;

typedef enum {
    OTA_ERROR_NONE = 0,
    OTA_ERROR_NETWORK,
    OTA_ERROR_SERVER_RESPONSE,
    OTA_ERROR_DOWNLOAD_FAILED,
    OTA_ERROR_VERIFICATION_FAILED,
    OTA_ERROR_UPGRADE_FAILED,
    OTA_ERROR_STORAGE_FULL,
    OTA_ERROR_INVALID_FIRMWARE
} ota_error_t;

/******************************************************
 *                   Structures
 ******************************************************/
typedef struct {
    char current_version[OTA_MAX_VERSION_LENGTH];
    char latest_version[OTA_MAX_VERSION_LENGTH];
    char firmware_url[OTA_MAX_URL_LENGTH];
    uint32_t firmware_size;
    uint32_t firmware_crc32;
    bool update_available;
} ota_version_info_t;

typedef struct {
    char hostname[OTA_MAX_HOSTNAME_LENGTH];
    char version_check_path[OTA_MAX_URL_LENGTH];
    char firmware_base_path[OTA_MAX_URL_LENGTH];
    uint16_t port;
    bool https_enabled;
} ota_server_config_t;

typedef struct {
    ota_state_t state;
    ota_error_t last_error;
    ota_version_info_t version_info;
    uint32_t download_progress;
    uint32_t last_check_time;
    bool auto_check_enabled;
} ota_context_t;

/******************************************************
 *               Function Declarations
 ******************************************************/

/**
 * @brief 初始化OTA升级模块
 * @param config 服务器配置
 * @return sl_status_t 状态码
 */
sl_status_t ota_init(const ota_server_config_t *config);

/**
 * @brief 启动自动版本检查
 * @return sl_status_t 状态码
 */
sl_status_t ota_start_auto_check(void);

/**
 * @brief 停止自动版本检查
 * @return sl_status_t 状态码
 */
sl_status_t ota_stop_auto_check(void);

/**
 * @brief 手动检查版本更新
 * @return sl_status_t 状态码
 */
sl_status_t ota_check_version(void);

/**
 * @brief 开始固件下载
 * @return sl_status_t 状态码
 */
sl_status_t ota_start_download(void);

/**
 * @brief 开始固件升级
 * @return sl_status_t 状态码
 */
sl_status_t ota_start_upgrade(void);

/**
 * @brief 获取当前OTA状态
 * @return ota_context_t* OTA上下文指针
 */
const ota_context_t* ota_get_status(void);

/**
 * @brief OTA任务处理函数（定期调用）
 * @return sl_status_t 状态码
 */
sl_status_t ota_task_handler(void);

/**
 * @brief 获取当前固件版本
 * @param version 版本字符串缓冲区
 * @param max_len 缓冲区最大长度
 * @return sl_status_t 状态码
 */
sl_status_t ota_get_current_version(char *version, size_t max_len);

/**
 * @brief 设置当前固件版本
 * @param version 版本字符串
 * @return sl_status_t 状态码
 */
sl_status_t ota_set_current_version(const char *version);

/**
 * @brief 验证固件文件
 * @param file_path 固件文件路径
 * @param expected_crc32 期望的CRC32值
 * @return sl_status_t 状态码
 */
sl_status_t ota_verify_firmware(const char *file_path, uint32_t expected_crc32);

/**
 * @brief 执行固件安装
 * @param firmware_path 固件文件路径
 * @return sl_status_t 状态码
 */
sl_status_t ota_install_firmware(const char *firmware_path);

/**
 * @brief 计算文件CRC32
 * @param file_path 文件路径
 * @param crc32 输出CRC32值
 * @return sl_status_t 状态码
 */
sl_status_t ota_calculate_file_crc32(const char *file_path, uint32_t *crc32);

#endif // WIFI_OTA_H