/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2020 Analog Devices, Inc.
 */

#ifndef _DT_BINDINGS_JESD204_DEVICE_STATE_H
#define _DT_BINDINGS_JESD204_DEVICE_STATE_H

/* These *MUST* be in a monotonic order */
#define JESD204_FSM_STATE_DEVICE_INIT			0
#define JESD204_FSM_STATE_LINK_INIT			1
#define JESD204_FSM_STATE_LINK_SUPPORTED		2
#define JESD204_FSM_STATE_LINK_PRE_SETUP		3
#define JESD204_FSM_STATE_CLK_SYNC_STAGE1		4
#define JESD204_FSM_STATE_CLK_SYNC_STAGE2		5
#define JESD204_FSM_STATE_CLK_SYNC_STAGE3		6
#define JESD204_FSM_STATE_LINK_SETUP			7
#define JESD204_FSM_STATE_OPT_SETUP_STAGE1		8
#define JESD204_FSM_STATE_OPT_SETUP_STAGE2		9
#define JESD204_FSM_STATE_OPT_SETUP_STAGE3		10
#define JESD204_FSM_STATE_OPT_SETUP_STAGE4		11
#define JESD204_FSM_STATE_OPT_SETUP_STAGE5		12
#define JESD204_FSM_STATE_CLOCKS_ENABLE			13
#define JESD204_FSM_STATE_LINK_ENABLE			14
#define JESD204_FSM_STATE_LINK_RUNNING			15
#define JESD204_FSM_STATE_OPT_POST_RUNNING_STAGE	16

/* Update this when adding states */
#define JESD204_FSM_STATE_LAST			(JESD204_FSM_STATE_OPT_POST_RUNNING_STAGE)

#define JESD204_FSM_STATES_NUM			(JESD204_FSM_STATE_LAST + 1)

#endif /* _DT_BINDINGS_JESD204_DEVICE_STATE_H */
