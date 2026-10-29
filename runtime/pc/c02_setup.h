// SPDX-License-Identifier: GPL-3.0-or-later
// A new C02 cabinet's set-up and its movies (c02_setup.c, PLAN.md section 36)
#pragma once

// The operator settings: when none load, the game's defaults saved with the clock set (and up to
// DistorteD, the shop's name and prefecture), so a new cabinet starts as one set up
void hk_c02_set_up_cabinet(void);
// HAPPY SKY's and DistorteD's movie renderer given the pictures in the form it takes
void hk_c02_movies(void);
// HK_PC_JNET_LOG: the game's network library's debug lines to pc.log ("jnet:")
void hk_c02_debug_lines(void);
