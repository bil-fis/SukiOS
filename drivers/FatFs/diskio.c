/*-----------------------------------------------------------------------*/
/* Low level disk I/O module for SukiOS        (C)ChaN + SukiOS adapt     */
/*-----------------------------------------------------------------------*/
/* 本文件是 FatFs 的磁盘 IO 适配层，把 FatFs 的扇区读写请求经 SukiOS 的
 * DISK_PORT Mach IPC 转交给运行于 Ring0 的 disk-srv（ATA PIO / AHCI DMA），
 * 实现「用户态 FAT32 服务 + 内核态裸磁盘驱动」的混合内核架构。
 *
 * 约束（include/ipc/disk_proto.h）：
 *   - 单次 DISK_PORT 请求最多 DISK_MAX_SECTORS(=7) 扇区（受内联消息 3968B 限制）；
 *     故本层对 FatFs 传入的 count>7 自动分批循环。
 *   - 请求发 DISK_PORT，应答经 FS_REPLY_PORT 回收。
 *   - 扇区固定 512 字节（与 FatFs FF_MIN_SS=FF_MAX_SS=512 一致）。
 */

#include "ff.h"            /* FatFs 类型/BYTE/UINT/LBA_t/DRESULT/DSTATUS */
#include "diskio.h"
#include "lib/suki.h"      /* mach_msg 封装、DISK_PORT、FS_REPLY_PORT、sys_port_claim */
#include <ipc/disk_proto.h>

/* 物理驱动器编号：SukiOS 仅一块固定磁盘，映射为 pdrv 0。 */
#define DEV_DISK   0

/* ---- 私有：经 DISK_PORT 读最多 DISK_MAX_SECTORS 扇区 ---- */
static int suki_disk_read_sectors(uint32_t lba, uint32_t count, void *out)
{
    if (count == 0 || count > DISK_MAX_SECTORS) return -1;
    uint8_t req[sizeof(mach_msg_header_t) + sizeof(disk_read_req_t)];
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits        = 0;
    h->msgh_size        = (uint32_t)sizeof(req);
    h->msgh_remote_port = DISK_PORT;
    h->msgh_local_port  = FS_REPLY_PORT;
    h->msgh_id          = DISK_MSG_READ;
    disk_read_req_t *r = (disk_read_req_t *)(req + sizeof(mach_msg_header_t));
    r->lba = (uint64_t)lba; r->count = count; r->pad = 0;

    if (mach_msg_send(req, h->msgh_size) != MACH_MSG_SUCCESS) return -1;

    static uint8_t s_resp[sizeof(mach_msg_header_t) + sizeof(disk_read_resp_t) +
                          DISK_MAX_SECTORS * 512];
    if (mach_msg_recv(s_resp, (uint32_t)sizeof(s_resp), FS_REPLY_PORT) != MACH_MSG_SUCCESS)
        return -1;
    mach_msg_header_t *rh = (mach_msg_header_t *)s_resp;
    uint32_t need = (uint32_t)(sizeof(mach_msg_header_t) +
                               sizeof(disk_read_resp_t) + count * 512);
    if (rh->msgh_size < need || rh->msgh_size > (uint32_t)sizeof(s_resp)) return -1;
    disk_read_resp_t *pr = (disk_read_resp_t *)(s_resp + sizeof(mach_msg_header_t));
    if (pr->status != 0) return -1;
    u_memcpy(out, (uint8_t *)(pr + 1), count * 512);
    return 0;
}

/* ---- 私有：经 DISK_PORT 写最多 DISK_MAX_SECTORS 扇区 ---- */
static int suki_disk_write_sectors(uint32_t lba, uint32_t count, const void *in)
{
    if (count == 0 || count > DISK_MAX_SECTORS) return -1;
    uint8_t req[sizeof(mach_msg_header_t) + sizeof(disk_write_req_t) +
                DISK_MAX_SECTORS * 512];
    mach_msg_header_t *h = (mach_msg_header_t *)req;
    h->msgh_bits        = 0;
    h->msgh_size        = (uint32_t)(sizeof(mach_msg_header_t) +
                                     sizeof(disk_write_req_t) + count * 512);
    h->msgh_remote_port = DISK_PORT;
    h->msgh_local_port  = FS_REPLY_PORT;
    h->msgh_id          = DISK_MSG_WRITE;
    disk_write_req_t *r = (disk_write_req_t *)(req + sizeof(mach_msg_header_t));
    r->lba = (uint64_t)lba; r->count = count; r->pad = 0;
    u_memcpy((uint8_t *)(r + 1), in, count * 512);

    if (mach_msg_send(req, h->msgh_size) != MACH_MSG_SUCCESS) return -1;

    uint8_t resp[sizeof(mach_msg_header_t) + sizeof(disk_write_resp_t)];
    if (mach_msg_recv(resp, (uint32_t)sizeof(resp), FS_REPLY_PORT) != MACH_MSG_SUCCESS)
        return -1;
    mach_msg_header_t *rh = (mach_msg_header_t *)resp;
    if (rh->msgh_size < sizeof(mach_msg_header_t) + sizeof(disk_write_resp_t))
        return -1;
    disk_write_resp_t *pr = (disk_write_resp_t *)(resp + sizeof(mach_msg_header_t));
    return (pr->status == 0) ? 0 : -1;
}

/* 缓存的总扇区数（GET_SECTOR_COUNT 用），首次初始化时查询。 */
static uint32_t g_total_sectors = 0;

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/
DSTATUS disk_status (BYTE pdrv)
{
    if (pdrv != DEV_DISK) return STA_NOINIT;
    return 0;   /* 内核 disk-srv 已就绪 */
}

/*-----------------------------------------------------------------------*/
/* Initialize a Drive                                                    */
/*-----------------------------------------------------------------------*/
DSTATUS disk_initialize (BYTE pdrv)
{
    if (pdrv != DEV_DISK) return STA_NOINIT;
    /* 认领私有应答端口（FS_REPLY_PORT），供 disk_read/write 回收应答。 */
    sys_port_claim(FS_REPLY_PORT);
    return 0;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/
DRESULT disk_read (
    BYTE pdrv,
    BYTE *buff,
    LBA_t sector,
    UINT count
)
{
    if (pdrv != DEV_DISK) return RES_PARERR;
    if (!buff || count == 0) return RES_PARERR;

    /* 分批：每批 ≤ DISK_MAX_SECTORS，循环直到全部读完。 */
    uint8_t *p = buff;
    LBA_t remaining = count;
    LBA_t base = sector;
    while (remaining > 0) {
        uint32_t batch = (remaining > DISK_MAX_SECTORS) ? DISK_MAX_SECTORS : (uint32_t)remaining;
        if (suki_disk_read_sectors((uint32_t)base, batch, p) != 0)
            return RES_ERROR;
        p += batch * 512;
        base += batch;
        remaining -= batch;
    }
    return RES_OK;
}

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/
#if FF_FS_READONLY == 0
DRESULT disk_write (
    BYTE pdrv,
    const BYTE *buff,
    LBA_t sector,
    UINT count
)
{
    if (pdrv != DEV_DISK) return RES_PARERR;
    if (!buff || count == 0) return RES_PARERR;

    const uint8_t *p = buff;
    LBA_t remaining = count;
    LBA_t base = sector;
    while (remaining > 0) {
        uint32_t batch = (remaining > DISK_MAX_SECTORS) ? DISK_MAX_SECTORS : (uint32_t)remaining;
        if (suki_disk_write_sectors((uint32_t)base, batch, p) != 0)
            return RES_ERROR;
        p += batch * 512;
        base += batch;
        remaining -= batch;
    }
    return RES_OK;
}
#endif

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/
DRESULT disk_ioctl (
    BYTE pdrv,
    BYTE cmd,
    void *buff
)
{
    if (pdrv != DEV_DISK) return RES_PARERR;

    switch (cmd) {
    case CTRL_SYNC:
        /* 写透传内核，无需额外同步；返回 OK。 */
        return RES_OK;

    case GET_SECTOR_COUNT:
        if (buff) {
            /* 总扇区数：通过读最后一个查询不便，这里用 FatFs 挂载时记录的
             * 值（f_mount 会先 GET_SECTOR_COUNT）。若未知，先读 BPB。 */
            if (g_total_sectors == 0) {
                uint8_t bpb[512];
                if (suki_disk_read_sectors(0, 1, bpb) != 0) return RES_ERROR;
                uint32_t ts16 = (uint32_t)bpb[19] | ((uint32_t)bpb[20] << 8);
                uint32_t ts32 = (uint32_t)bpb[32] | ((uint32_t)bpb[33] << 8) |
                                ((uint32_t)bpb[34] << 16) | ((uint32_t)bpb[35] << 24);
                g_total_sectors = (ts16 != 0) ? ts16 : ts32;
            }
            *(LBA_t *)buff = (LBA_t)g_total_sectors;
        }
        return RES_OK;

    case GET_SECTOR_SIZE:
        if (buff) *(WORD *)buff = 512;
        return RES_OK;

    case GET_BLOCK_SIZE:
        if (buff) *(DWORD *)buff = 1;   /* 擦除块大小（FAT 用 1 扇区） */
        return RES_OK;

    default:
        return RES_PARERR;
    }
}
