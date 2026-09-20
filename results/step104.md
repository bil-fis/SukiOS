# step104 — USB 鼠标上下方向取反（与 PS/2 鼠标相反）

用户诉求：

> 「现在，让 usb 鼠标和 ps2 鼠标的移动方向相反（上下相反）」

## 实现

`kernel/drivers/usb/usb_hid.c::hid_mouse_process()`：USB(HID) 鼠标的报告经本函数**合成 PS/2 3/4 字节包**后经 `mouse_feed_byte()` 注入既有输入栈（与 PS/2 鼠标完全同构）。原实现把 HID 报告的 `dy` **原样**填入包中；现对 `dy` **取反**，使 USB 鼠标的上下方向与 PS/2 鼠标**相反**。

```c
/* USB_MOUSE_INVERT_Y=1：对 HID dy 取反后再合成 PS/2 包，
 * 使 USB 鼠标上下方向与 PS/2 相反（用户要求）。改回 0 即恢复一致。 */
#define USB_MOUSE_INVERT_Y 1
...
    int8_t dy = (int8_t)rep[2];
#if USB_MOUSE_INVERT_Y
    dy = (int8_t)(-dy);
#endif
    uint8_t b0 = (uint8_t)(0x08 | buttons);
    uint8_t b1 = (uint8_t)dx;
    uint8_t b2 = (uint8_t)dy;
    mouse_feed_byte(b0); mouse_feed_byte(b1); mouse_feed_byte(b2);
```

- 水平方向（dx）保持不变；仅垂直（dy）取反。
- 通过宏 `USB_MOUSE_INVERT_Y` 一键切换（1=相反，0=一致）。

## 验证（QEMU，USB + hub + kbd + mouse）

```
lines=399  usb_mouse=1  disp=1  wm=2  prompt=1  err=0  PASS=105
[mouse]      PS/2 mouse ready (IOAPIC GSI12 ...)
[usb-hid]    mouse ready (addr=3 ep=81 mps=4)
[boot]       display-server ready
SukiOS:/>
```
无回归：USB 键鼠枚举正常、窗口创建、shell 出提示符、**零 panic/EXC**、PASS=105。

> 方向的实际观感需在图形界面实测（headless `-display none` 无法注入真实鼠标移动）。若方向不符合预期，把 `USB_MOUSE_INVERT_Y` 改为 0 即可。

## 修改文件
- `kernel/drivers/usb/usb_hid.c`：`hid_mouse_process()` 对 dy 取反 + `USB_MOUSE_INVERT_Y` 宏
