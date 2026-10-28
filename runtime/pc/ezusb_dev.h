// SPDX-License-Identifier: GPL-3.0-or-later
// The C02 and D01 cabinets' EZ-USB board as the device under its driver (ezusb_dev.c, PLAN.md
// section 36): for the games that open the driver themselves (beatmania IIDX RED)
#pragma once

// Once, before hk_cabinet_start: the driver's calls in the game's imports are answered by HIRAKU's
// board. Environment: HK_PC_SECURITY ("GQE11JAA 0:2:2": the plug's model code and the boot's
// seeds), HK_PC_BOARD (d01, the default, or c02), HK_PC_PCBID, HK_PC_USB_LOG
void hk_ezusb_dev_start(void);
