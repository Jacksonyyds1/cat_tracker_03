#ifndef OTA_CONFIG_EXAMPLE_H
#define OTA_CONFIG_EXAMPLE_H

/**
 * @file ota_config_example.h
 * @brief OTA升级配置示例
 *
 * 此文件包含OTA升级功能的配置示例，用户可以根据自己的AWS S3设置进行修改
 */

/******************************************************
 *              AWS S3 服务器配置示例
 ******************************************************/

// AWS S3存储桶配置
// 请替换为您的AWS S3存储桶信息
#define OTA_AWS_S3_BUCKET_NAME      "your-firmware-bucket"
#define OTA_AWS_S3_REGION           "us-west-2"
#define OTA_AWS_S3_HOSTNAME         OTA_AWS_S3_BUCKET_NAME ".s3." OTA_AWS_S3_REGION ".amazonaws.com"

// 固件版本信息文件路径 (JSON格式)
#define OTA_VERSION_FILE_PATH       "/firmware/version.json"

// 固件文件存储路径前缀
#define OTA_FIRMWARE_BASE_PATH      "/firmware/"

// HTTPS端口 (AWS S3使用443)
#define OTA_SERVER_PORT             443

/******************************************************
 *              版本信息JSON格式示例
 ******************************************************/
/*
在您的AWS S3存储桶中，版本信息文件应该包含以下JSON格式：

{
  "version": "1.2.0",
  "url": "/firmware/catcollar_firmware_v1.2.0.bin",
  "size": 524288,
  "crc32": "0x12345678",
  "description": "Bug fixes and performance improvements",
  "release_date": "2024-09-14",
  "minimum_version": "1.0.0"
}

字段说明：
- version: 最新固件版本号
- url: 固件文件在S3中的相对路径
- size: 固件文件大小（字节）
- crc32: 固件文件的CRC32校验值（十六进制字符串）
- description: 版本更新说明
- release_date: 发布日期
- minimum_version: 支持升级的最低版本
*/

/******************************************************
 *              OTA升级流程配置
 ******************************************************/

// 版本检查间隔 (毫秒)
#define OTA_VERSION_CHECK_INTERVAL_MS   (60 * 60 * 1000)  // 1小时

// 下载超时时间 (毫秒)
#define OTA_DOWNLOAD_TIMEOUT_MS         (10 * 60 * 1000)  // 10分钟

// HTTP请求超时时间 (毫秒)
#define OTA_HTTP_TIMEOUT_MS             (30 * 1000)       // 30秒

// 最大重试次数
#define OTA_MAX_RETRY_COUNT             3

// 是否启用自动升级 (0=手动, 1=自动)
#define OTA_AUTO_UPGRADE_ENABLED        0

/******************************************************
 *              固件存储配置
 ******************************************************/

// 临时固件文件存储路径
#define OTA_TEMP_FIRMWARE_PATH          "/tmp/firmware_update.bin"

// 当前版本信息文件路径
#define OTA_CURRENT_VERSION_PATH        "/config/current_version.txt"

// 固件备份路径（可选）
#define OTA_BACKUP_FIRMWARE_PATH        "/backup/firmware_backup.bin"

/******************************************************
 *              日志和调试配置
 ******************************************************/

// 是否启用详细日志
#define OTA_VERBOSE_LOGGING             1

// 是否启用进度显示
#define OTA_SHOW_PROGRESS               1

// 是否启用错误详情
#define OTA_DETAILED_ERRORS             1

/******************************************************
 *              安全配置
 ******************************************************/

// 是否验证固件签名（需要实现具体的签名验证）
#define OTA_VERIFY_SIGNATURE            0

// 是否使用TLS证书验证
#define OTA_VERIFY_TLS_CERT             1

// 最大允许的固件大小 (字节)
#define OTA_MAX_FIRMWARE_SIZE           (2 * 1024 * 1024)  // 2MB

#endif // OTA_CONFIG_EXAMPLE_H