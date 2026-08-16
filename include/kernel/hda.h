/*
 * include/kernel/hda.h
 * -----------------------------------------------------------------------------
 * Intel High Definition Audio (HDA) 控制器 + 编解码器驱动接口。
 *
 * 参考：Intel HDA Specification Rev 1.0a（high-definition-audio-specification.pdf，
 *       CORB/RIRB §4.4.1，流描述符/BDL §3.3.35+，verb §7.3.3/§7.3.4）
 *       与 osdev wiki（wiki.osdev.org/Intel_High_Definition_Audio）。
 * 注：寄存器布局遵循 QEMU intel-hda / osdev 简化模型（流描述符每项 0x20 字节，
 *     0x5A=RINTCNT 等），与严格 1.0a 的 SDnCTL 位布局略有差异；驱动以 QEMU
 *     验证为准，verb 编码数值与规范完全一致。
 *
 * 驱动模型（与 ATA 相同的"内核态特例"）：控制器 MMIO / CORB / RIRB /
 * 流 DMA 全部驻留 Ring0；Ring3 播放器通过 SYS_AUDIO_* 系统调用把 PCM
 * 数据写入内核环形缓冲（BDL 32×4KB，轮询 LPIB，不使用中断）。
 *
 * 调用关系：kmain -> hda_init()；
 *           sys_audio_* (syscall.c) -> hda_pcm_open/write/queued/stop。
 */
#ifndef _SUKI_KERNEL_HDA_H
#define _SUKI_KERNEL_HDA_H

#include <kernel/types.h>

/* 环形缓冲几何：BDL 32 项 × 4KB = 128KB（44.1kHz/16bit/双声道 ≈ 0.74s） */
#define HDA_BDL_ENTRIES   32
#define HDA_BUF_BYTES     (HDA_BDL_ENTRIES * PAGE_SIZE)

/* 探测并初始化 HDA 控制器与编解码器（无设备返回 false，系统继续运行） */
bool hda_init(void);

/* 是否已成功初始化（供 syscall 快速判断） */
bool hda_present(void);

/* 打开输出 PCM 流：rate=44100/48000/...，channels=1/2，bits=16。
 * 成功返回 0；不支持的参数或无设备返回 -1。可重复调用（重新配置）。 */
int hda_pcm_open(uint32_t rate, uint32_t channels, uint32_t bits);

/* 向播放环写入 PCM（S16LE 交错）。返回实际接受的字节数（0=环满，
 * 调用方应让出 CPU 后重试）；未 open 或无设备返回 -1。
 * 环内数据积累到预填充水位后自动启动流 DMA。 */
int64_t hda_pcm_write(const void *buf, size_t len);

/* 返回环中尚未播放的字节数（用于播放器排空等待） */
uint64_t hda_pcm_queued(void);

/* 停止流 DMA 并复位流（排空后调用）。 */
void hda_pcm_stop(void);

/* 任务退出清理：若 t 是 PCM 流 owner，停流并释放所有权（P0-R1）。
 * 由 task_exit_current 调用，防 owner 悬空指针 + 音频永久锁死。 */
struct task;
void hda_release_owner(struct task *t);

#endif /* _SUKI_KERNEL_HDA_H */
