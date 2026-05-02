#ifndef SUKIOS_RTC_H
#define SUKIOS_RTC_H

#include <stdint.h>

/* RTC 时间结构体 */
typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;       // 完整年份，如 2026
    uint8_t century;     // 世纪，如 20
} rtc_time_t;

/* 初始化 RTC（无特殊操作，但保留） */
void rtc_init(void);

/* 读取当前时间（会等待更新完成） */
void rtc_read_time(rtc_time_t *time);

/* 将 RTC 时间转换为 FAT32 目录项的时间戳格式 */
uint16_t rtc_to_fat_time(const rtc_time_t *time);
uint16_t rtc_to_fat_date(const rtc_time_t *time);

#endif