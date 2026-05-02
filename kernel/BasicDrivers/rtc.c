#include "kernel/rtc.h"
#include "kernel/io.h"
#include "kernel/tty.h"

/* CMOS 端口 */
#define CMOS_ADDR      0x70
#define CMOS_DATA      0x71

/* RTC 寄存器索引 */
#define RTC_SECONDS    0x00
#define RTC_MINUTES    0x02
#define RTC_HOURS      0x04
#define RTC_WEEKDAY    0x06
#define RTC_DAY        0x07
#define RTC_MONTH      0x08
#define RTC_YEAR       0x09
#define RTC_CENTURY    0x32
#define RTC_STATUS_A   0x0A
#define RTC_STATUS_B   0x0B

/* NMI 禁用位（bit 7），写入地址端口时带上此位可禁用 NMI */
#define CMOS_NMI_DISABLE 0x80

/* 从 CMOS 读取一个字节 */
static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, CMOS_NMI_DISABLE | reg);
    io_wait();   // 短暂延时
    return inb(CMOS_DATA);
}

/* 检查 RTC 是否正在更新（Status Register A bit 7） */
static int rtc_is_updating(void)
{
    outb(CMOS_ADDR, CMOS_NMI_DISABLE | RTC_STATUS_A);
    io_wait();
    return (inb(CMOS_DATA) & 0x80) != 0;
}

/* BCD 转二进制 */
static uint8_t bcd_to_bin(uint8_t bcd)
{
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

void rtc_init(void)
{
    tty_print("[RTC] Driver initialized.\n");
}

void rtc_read_time(rtc_time_t *time)
{
    /* 等待 RTC 不处于更新状态（OSDev 标准做法） */
    while (rtc_is_updating());

    time->second = cmos_read(RTC_SECONDS);
    time->minute = cmos_read(RTC_MINUTES);
    time->hour   = cmos_read(RTC_HOURS);
    time->day    = cmos_read(RTC_DAY);
    time->month  = cmos_read(RTC_MONTH);
    time->year   = cmos_read(RTC_YEAR);

    /* 尝试读取世纪寄存器 */
    uint8_t century = cmos_read(RTC_CENTURY);
    time->century = century;

    /* 检查 Status Register B 的 bit 2，决定数据是否为 BCD 格式 */
    outb(CMOS_ADDR, CMOS_NMI_DISABLE | RTC_STATUS_B);
    io_wait();
    uint8_t status_b = inb(CMOS_DATA);

    /* 如果 bit 2 (DM, Data Mode) 为 0，表示 BCD 模式，需要转换 */
    if (!(status_b & 0x04)) {
        time->second = bcd_to_bin(time->second);
        time->minute = bcd_to_bin(time->minute);
        time->hour   = bcd_to_bin(time->hour);
        time->day    = bcd_to_bin(time->day);
        time->month  = bcd_to_bin(time->month);
        time->year   = bcd_to_bin(time->year);
        if (century != 0xFF && century != 0x00) {
            time->century = bcd_to_bin(century);
        }
    }

    /* 计算完整年份 */
    if (time->century == 0x00 || time->century == 0xFF) {
        /* 没有世纪寄存器，根据年份推断 */
        time->century = 20;   // 假设 2000 年之后
    }
    time->year = time->century * 100 + time->year;
}

uint16_t rtc_to_fat_time(const rtc_time_t *time)
{
    /* FAT 时间格式: hhhhhmmmmmmsssss
     *   高 5 位: 小时 (0-23)
     *   中 6 位: 分钟 (0-59)
     *   低 5 位: 秒/2 (0-29)
     */
    uint16_t fat_time = 0;
    fat_time |= ((uint16_t)(time->second / 2) & 0x1F);
    fat_time |= ((uint16_t)time->minute & 0x3F) << 5;
    fat_time |= ((uint16_t)time->hour & 0x1F) << 11;
    return fat_time;
}

uint16_t rtc_to_fat_date(const rtc_time_t *time)
{
    /* FAT 日期格式: yyyyyyymmmmddddd
     *   高 7 位: 年份 - 1980
     *   中 4 位: 月份 (1-12)
     *   低 5 位: 日期 (1-31)
     */
    uint16_t fat_date = 0;
    uint16_t year_offset = (uint16_t)(time->year - 1980);
    fat_date |= ((uint16_t)time->day & 0x1F);
    fat_date |= ((uint16_t)time->month & 0x0F) << 5;
    fat_date |= (year_offset & 0x7F) << 9;
    return fat_date;
}