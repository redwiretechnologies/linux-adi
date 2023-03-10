// SPDX-License-Identifier: GPL-2.0
/*
 * ADRV9002 RF Transceiver
 *
 * Copyright 2019 Analog Devices Inc.
 */
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>
#include <linux/units.h>

#include "adrv9002.h"
#include "adi_adrv9001.h"
#include "adi_adrv9001_arm.h"
#include "adi_adrv9001_arm_types.h"
#include "adi_adrv9001_auxadc.h"
#include "adi_adrv9001_auxadc_types.h"
#include "adi_adrv9001_bbdc.h"
#include "adi_adrv9001_cals.h"
#include "adi_adrv9001_cals_types.h"
#include "adi_common_types.h"
#include "adi_adrv9001_auxdac.h"
#include "adi_adrv9001_auxdac_types.h"
#include "adi_adrv9001_fh.h"
#include "adi_adrv9001_fh_types.h"
#include "adi_adrv9001_gpio.h"
#include "adi_adrv9001_gpio_types.h"
#include "adi_adrv9001_orx.h"
#include "adi_adrv9001_powermanagement.h"
#include "adi_adrv9001_powermanagement_types.h"
#include "adi_adrv9001_profile_types.h"
#include "adi_adrv9001_profileutil.h"
#include "adi_adrv9001_radio.h"
#include "adi_adrv9001_radio_types.h"
#include "adi_adrv9001_rx_gaincontrol.h"
#include "adi_adrv9001_rx_gaincontrol_types.h"
#include "adi_adrv9001_rx.h"
#include "adi_adrv9001_rx_types.h"
#include "adi_adrv9001_rxSettings_types.h"
#include "adi_adrv9001_spi.h"
#include "adi_adrv9001_ssi.h"
#include "adi_adrv9001_ssi_types.h"
#include "adi_adrv9001_stream.h"
#include "adi_adrv9001_stream_types.h"
#include "adi_adrv9001_types.h"
#include "adi_adrv9001_tx.h"
#include "adi_adrv9001_tx_types.h"
#include "adi_adrv9001_txSettings_types.h"
#include "adi_adrv9001_utilities.h"
#include "adi_adrv9001_version.h"
#include "adi_common_error_types.h"

#define ALL_RX_CHANNEL_MASK	(ADI_ADRV9001_RX1 | ADI_ADRV9001_RX2 | \
				 ADI_ADRV9001_ORX1 | ADI_ADRV9001_ORX2)

#define ADRV9002_RX_EN(nr)	BIT(((nr) * 2) & 0x3)
#define ADRV9002_TX_EN(nr)	BIT(((nr) * 2 + 1) & 0x3)

#define ADRV9002_RX_MAX_GAIN_mdB	\
	((ADI_ADRV9001_RX_GAIN_INDEX_MAX - ADI_ADRV9001_RX_GAIN_INDEX_MIN) *	\
	 ADRV9002_RX_GAIN_STEP_mDB)
#define ADRV9002_RX_GAIN_STEP_mDB	500
/* ORx gain defines */
#define ADRV9002_ORX_GAIN_STEP_mDB	5000
#define ADRV9002_ORX_MIN_GAIN_IDX	ADI_ADRV9001_ORX_GAIN_INDEX_MIN
#define ADRV9002_ORX_MAX_GAIN_IDX	ADI_ADRV9001_ORX_GAIN_INDEX_MAX
/*
 * the Orx tables indexes are the same in a x2 step. And only the even index will actually
 * take effect in the device. That's why we divide by 2...
 */
#define ADRV9002_ORX_MAX_GAIN_DROP_mdB	\
	((ADI_ADRV9001_ORX_GAIN_INDEX_MAX - ADI_ADRV9001_ORX_GAIN_INDEX_MIN) / 2 \
	 * ADRV9002_ORX_GAIN_STEP_mDB)
#define ADRV9002_ORX_MIN_GAIN_mdB	({ \
	BUILD_BUG_ON(ADRV9002_ORX_MAX_GAIN_DROP_mdB > ADRV9002_RX_MAX_GAIN_mdB); \
	(ADRV9002_RX_MAX_GAIN_mdB - ADRV9002_ORX_MAX_GAIN_DROP_mdB); \
})

#define ADRV9002_STREAM_BINARY_SZ	ADI_ADRV9001_STREAM_BINARY_IMAGE_FILE_SIZE_BYTES
#define ADRV9002_PROFILE_MAX_SZ		73728
#define ADRV9002_HP_CLK_PLL_DAHZ	884736000
#define ADRV9002_NO_EXT_LO		0xff
#define ADRV9002_EXT_LO_FREQ_MIN	60000000
#define ADRV9002_EXT_LO_FREQ_MAX	12000000000ULL

/* Frequency hopping */
#define ADRV9002_FH_TABLE_COL_SZ	7

/* IRQ Masks */
#define ADRV9002_GP_MASK_RX_DP_RECEIVE_ERROR		0x08000000
#define ADRV9002_GP_MASK_TX_DP_TRANSMIT_ERROR		0x04000000
#define ADRV9002_GP_MASK_RX_DP_READ_REQUEST_FROM_BBIC	0x02000000
#define ADRV9002_GP_MASK_TX_DP_WRITE_REQUEST_TO_BBIC	0x01000000
#define ADRV9002_GP_MASK_STREAM_PROCESSOR_3_ERROR	0x00100000
#define ADRV9002_GP_MASK_STREAM_PROCESSOR_2_ERROR	0x00080000
#define ADRV9002_GP_MASK_STREAM_PROCESSOR_1_ERROR	0x00040000
#define ADRV9002_GP_MASK_STREAM_PROCESSOR_0_ERROR	0x00020000
#define ADRV9002_GP_MASK_MAIN_STREAM_PROCESSOR_ERROR	0x00010000
#define ADRV9002_GP_MASK_LSSI_RX2_CLK_MCS		0x00008000
#define ADRV9002_GP_MASK_LSSI_RX1_CLK_MCS		0x00004000
#define ADRV9002_GP_MASK_CLK_1105_MCS_SECOND		0x00002000
#define ADRV9002_GP_MASK_CLK_1105_MCS			0x00001000
#define ADRV9002_GP_MASK_CLK_PLL_LOCK			0x00000800
#define ADRV9002_GP_MASK_AUX_PLL_LOCK			0x00000400
#define ADRV9002_GP_MASK_RF2_SYNTH_LOCK			0x00000200
#define ADRV9002_GP_MASK_RF_SYNTH_LOCK			0x00000100
#define ADRV9002_GP_MASK_CLK_PLL_LOW_POWER_LOCK		0x00000080
#define ADRV9002_GP_MASK_TX2_PA_PROTECTION_ERROR	0x00000040
#define ADRV9002_GP_MASK_TX1_PA_PROTECTION_ERROR	0x00000020
#define ADRV9002_GP_MASK_CORE_ARM_MONITOR_ERROR		0x00000010
#define ADRV9002_GP_MASK_CORE_ARM_CALIBRATION_ERROR	0x00000008
#define ADRV9002_GP_MASK_CORE_ARM_SYSTEM_ERROR		0x00000004
#define ADRV9002_GP_MASK_CORE_FORCE_GP_INTERRUPT	0x00000002
#define ADRV9002_GP_MASK_CORE_ARM_ERROR			0x00000001

#define ADRV9002_IRQ_MASK					\
	(ADRV9002_GP_MASK_CORE_ARM_ERROR |			\
	 ADRV9002_GP_MASK_CORE_FORCE_GP_INTERRUPT |		\
	 ADRV9002_GP_MASK_CORE_ARM_SYSTEM_ERROR |		\
	 ADRV9002_GP_MASK_CORE_ARM_CALIBRATION_ERROR |		\
	 ADRV9002_GP_MASK_CORE_ARM_MONITOR_ERROR |		\
	 ADRV9002_GP_MASK_TX1_PA_PROTECTION_ERROR |		\
	 ADRV9002_GP_MASK_TX2_PA_PROTECTION_ERROR |		\
	 ADRV9002_GP_MASK_CLK_PLL_LOW_POWER_LOCK |		\
	 ADRV9002_GP_MASK_RF_SYNTH_LOCK |			\
	 ADRV9002_GP_MASK_RF2_SYNTH_LOCK |			\
	 ADRV9002_GP_MASK_AUX_PLL_LOCK |			\
	 ADRV9002_GP_MASK_CLK_PLL_LOCK |			\
	 ADRV9002_GP_MASK_MAIN_STREAM_PROCESSOR_ERROR |		\
	 ADRV9002_GP_MASK_STREAM_PROCESSOR_0_ERROR |		\
	 ADRV9002_GP_MASK_STREAM_PROCESSOR_1_ERROR |		\
	 ADRV9002_GP_MASK_STREAM_PROCESSOR_2_ERROR |		\
	 ADRV9002_GP_MASK_STREAM_PROCESSOR_3_ERROR |		\
	 ADRV9002_GP_MASK_TX_DP_WRITE_REQUEST_TO_BBIC |		\
	 ADRV9002_GP_MASK_RX_DP_READ_REQUEST_FROM_BBIC |	\
	 ADRV9002_GP_MASK_TX_DP_TRANSMIT_ERROR |		\
	 ADRV9002_GP_MASK_RX_DP_RECEIVE_ERROR)

enum {
	ADRV9002_RX1_BIT_NR,
	ADRV9002_RX2_BIT_NR,
	ADRV9002_TX1_BIT_NR,
	ADRV9002_TX2_BIT_NR,
	ADRV9002_ORX1_BIT_NR,
	ADRV9002_ORX2_BIT_NR,
};

int __adrv9002_dev_err(const struct adrv9002_rf_phy *phy, const char *function, const int line)
{
	int ret;

	dev_err(&phy->spi->dev, "%s, %d: failed with \"%s\" (%d)\n", function, line,
		phy->adrv9001->common.error.errormessage ?
		phy->adrv9001->common.error.errormessage : "",
		phy->adrv9001->common.error.errCode);

	switch (phy->adrv9001->common.error.errCode) {
	case ADI_COMMON_ERR_INV_PARAM:
	case ADI_COMMON_ERR_NULL_PARAM:
		ret = -EINVAL;
		break;
	case ADI_COMMON_ERR_API_FAIL:
		ret = -EFAULT;
		break;
	case ADI_COMMON_ERR_SPI_FAIL:
		ret = -EIO;
		break;
	case ADI_COMMON_ERR_MEM_ALLOC_FAIL:
		ret = -ENOMEM;
		break;
	default:
		ret = -EFAULT;
		break;
	}

	adi_common_ErrorClear(&phy->adrv9001->common);

	return ret;
}

static void adrv9002_get_ssi_interface(const struct adrv9002_rf_phy *phy, const int chann,
				       const bool tx, u8 *n_lanes, bool *cmos_ddr_en)
{
	if (tx) {
		adi_adrv9001_TxProfile_t *tx_cfg;

		tx_cfg = &phy->curr_profile->tx.txProfile[chann];
		*n_lanes = tx_cfg->txSsiConfig.numLaneSel;
		*cmos_ddr_en = tx_cfg->txSsiConfig.ddrEn;
	} else {
		adi_adrv9001_RxProfile_t *rx_cfg;

		rx_cfg = &phy->curr_profile->rx.rxChannelCfg[chann].profile;
		*n_lanes = rx_cfg->rxSsiConfig.numLaneSel;
		*cmos_ddr_en = rx_cfg->rxSsiConfig.ddrEn;
	}
}

static int adrv9002_ssi_configure(const struct adrv9002_rf_phy *phy)
{
	bool cmos_ddr;
	u8 n_lanes;
	int c, ret;

	for (c = 0; c < ARRAY_SIZE(phy->channels); c++) {
		const struct adrv9002_chan *chann = phy->channels[c];

		/* RX2/TX2 can only be enabled if RX1/TX1 are also enabled */
		if (phy->rx2tx2 && chann->idx > ADRV9002_CHANN_1)
			break;

		if (!chann->enabled)
			continue;

		adrv9002_sync_gpio_toggle(phy);

		adrv9002_get_ssi_interface(phy, chann->idx, chann->port == ADI_TX, &n_lanes,
					   &cmos_ddr);
		ret = adrv9002_axi_interface_set(phy, n_lanes, cmos_ddr, chann->idx,
						 chann->port == ADI_TX);
		if (ret)
			return ret;

		/*
		 * We should set the tdd rate on TX's iterations since only at this point we
		 * have the up to date dds rate. Moreover it does not make sense to do any
		 * tdd configuration if both TX/RX on the same channel are not enabled.
		 */
		if (chann->port == ADI_TX) {
			const struct adrv9002_rx_chan *rx = &phy->rx_channels[chann->idx];
			unsigned long rate;

			if (!rx->channel.enabled)
				continue;

			rate = adrv9002_axi_dds_rate_get(phy, chann->idx) * rx->channel.rate;
			clk_set_rate(rx->tdd_clk, rate);
		}
	}

	return 0;
}

static int adrv9002_phy_reg_access(struct iio_dev *indio_dev,
				   u32 reg, u32 writeval,
				   u32 *readval)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	int ret;

	mutex_lock(&phy->lock);
	if (!readval) {
		ret = api_call(phy, adi_adrv9001_spi_Byte_Write, reg, writeval);
	} else {
		u8 val;

		ret = api_call(phy, adi_adrv9001_spi_Byte_Read, reg, &val);
		*readval = val;
	}
	mutex_unlock(&phy->lock);

	return ret;
}

#define ADRV9002_MAX_CLK_NAME 79

static char *adrv9002_clk_set_dev_name(const struct adrv9002_rf_phy *phy,
				       char *dest, const char *name)
{
	size_t len = 0;

	if (!name)
		return NULL;

	if (*name == '-')
		len = strscpy(dest, dev_name(&phy->spi->dev),
			      ADRV9002_MAX_CLK_NAME);
	else
		*dest = '\0';

	return strncat(dest, name, ADRV9002_MAX_CLK_NAME - len);
}

static unsigned long adrv9002_bb_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	struct adrv9002_clock *clk_priv = to_clk_priv(hw);

	return clk_priv->rate;
}

static int adrv9002_bb_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct adrv9002_clock *clk_priv = to_clk_priv(hw);

	clk_priv->rate = rate;

	return 0;
}

static long adrv9002_bb_round_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long *prate)
{
	struct adrv9002_clock *clk_priv = to_clk_priv(hw);

	dev_dbg(&clk_priv->spi->dev, "%s: Rate %lu Hz", __func__, rate);

	return rate;
}

static const struct clk_ops bb_clk_ops = {
	.round_rate = adrv9002_bb_round_rate,
	.set_rate = adrv9002_bb_set_rate,
	.recalc_rate = adrv9002_bb_recalc_rate,
};

static struct clk *adrv9002_clk_register(struct adrv9002_rf_phy *phy, const char *name,
					 const unsigned long flags, const u32 source)
{
	struct adrv9002_clock *clk_priv = &phy->clk_priv[source];
	struct clk_init_data init;
	struct clk *clk;
	char c_name[ADRV9002_MAX_CLK_NAME + 1];

	/* struct adrv9002_clock assignments */
	clk_priv->source = source;
	clk_priv->hw.init = &init;
	clk_priv->spi = phy->spi;
	clk_priv->phy = phy;

	init.name = adrv9002_clk_set_dev_name(phy, c_name, name);
	init.flags = flags;
	init.num_parents = 0;
	init.ops = &bb_clk_ops;

	clk = devm_clk_register(&phy->spi->dev, &clk_priv->hw);
	if (IS_ERR(clk)) {
		dev_err(&phy->spi->dev, "Error registering clock=%d, err=%ld\n", source,
			PTR_ERR(clk));
		return ERR_CAST(clk);
	}

	phy->clks[phy->n_clks++] = clk;

	return clk;
}

static void adrv9002_set_clk_rates(const struct adrv9002_rf_phy *phy)
{
	int c;

	for (c = 0; c < ARRAY_SIZE(phy->channels); c++) {
		const struct adrv9002_chan *chan = phy->channels[c];

		if (!chan->enabled)
			continue;

		clk_set_rate(chan->clk, chan->rate);
	}
}

enum lo_ext_info {
	LOEXT_FREQ,
};

static int adrv9002_gainidx_to_gain(int idx, int port)
{
	int gain;

	if (port  == ADI_RX) {
		idx = clamp(idx, ADRV9002_RX_MIN_GAIN_IDX, ADRV9002_RX_MAX_GAIN_IDX);
		gain = (idx - ADRV9002_RX_MIN_GAIN_IDX) * ADRV9002_RX_GAIN_STEP_mDB;
	} else {
		/* ADI_ORX - look at the ORX defines for why we have the div/mult by 2 */
		idx = clamp(idx, ADRV9002_ORX_MIN_GAIN_IDX, ADRV9002_ORX_MAX_GAIN_IDX);
		gain = (idx - ADRV9002_ORX_MIN_GAIN_IDX) / 2 * ADRV9002_ORX_GAIN_STEP_mDB +
			ADRV9002_ORX_MIN_GAIN_mdB;
	}

	return gain;
}

static int adrv9002_gain_to_gainidx(int gain, int port)
{
	int temp;

	if (port  == ADI_RX) {
		gain = clamp(gain, 0, ADRV9002_RX_MAX_GAIN_mdB);
		temp = DIV_ROUND_CLOSEST(gain, ADRV9002_RX_GAIN_STEP_mDB);
		temp += ADRV9002_RX_MIN_GAIN_IDX;
	} else {
		/* ADI_ORX */
		gain = clamp(gain, ADRV9002_ORX_MIN_GAIN_mdB, ADRV9002_RX_MAX_GAIN_mdB);
		temp = DIV_ROUND_CLOSEST(gain - ADRV9002_ORX_MIN_GAIN_mdB,
					 ADRV9002_ORX_GAIN_STEP_mDB) * 2;
		temp += ADRV9002_ORX_MIN_GAIN_IDX;
	}

	return temp;
}

static int adrv9002_chan_to_state_poll(const struct adrv9002_rf_phy *phy,
				       const struct adrv9002_chan *c,
				       const adi_adrv9001_ChannelState_e state,
				       const int n_tries)
{
	int ret, tmp;
	adi_adrv9001_ChannelState_e __state;

	tmp = read_poll_timeout(adi_adrv9001_Radio_Channel_State_Get, ret,
				ret || (__state == state), 1000, n_tries * 1000, false,
				phy->adrv9001, c->port, c->number, &__state);

	/* so that we keep the same behavior as before introducing read_poll_timeout() */
	if (tmp == -ETIMEDOUT)
		return -EBUSY;

	return ret ? __adrv9002_dev_err(phy, __func__, __LINE__) : 0;
}

static bool adrv9002_orx_enabled(const struct adrv9002_rf_phy *phy, const struct adrv9002_chan *c)
{
	const struct adrv9002_rx_chan *rx;

	if (c->port == ADI_RX)
		rx = chan_to_rx(c);
	else
		rx = &phy->rx_channels[c->idx];

	if (!rx->orx_gpio)
		return false;

	return !!gpiod_get_value_cansleep(rx->orx_gpio);
}

int adrv9002_channel_to_state(const struct adrv9002_rf_phy *phy, struct adrv9002_chan *chann,
			      const adi_adrv9001_ChannelState_e state, const bool cache_state)
{
	int ret;
	adi_adrv9001_ChannelEnableMode_e mode;

	/* nothing to do */
	if (!chann->enabled)
		return 0;
	/*
	 * if ORx is enabled we are not expected to do any state transition on RX/TX in the
	 * same channel as that might have the non explicit side effect of breaking the
	 * capture in the ORx port. Hence, we should protect against that...
	 */
	if (adrv9002_orx_enabled(phy, chann))
		return -EPERM;

	ret = api_call(phy, adi_adrv9001_Radio_ChannelEnableMode_Get, chann->port,
		       chann->number, &mode);
	if (ret)
		return ret;

	/* we need to set it to spi */
	if (mode == ADI_ADRV9001_PIN_MODE) {
		ret = api_call(phy, adi_adrv9001_Radio_ChannelEnableMode_Set, chann->port,
			       chann->number, ADI_ADRV9001_SPI_MODE);
		if (ret)
			return ret;
	}

	if (cache_state) {
		ret = api_call(phy, adi_adrv9001_Radio_Channel_State_Get, chann->port,
			       chann->number, &chann->cached_state);
		if (ret)
			return ret;
	}

	ret = api_call(phy, adi_adrv9001_Radio_Channel_ToState, chann->port, chann->number, state);
	if (ret)
		return ret;
	/*
	 * Make sure that the channel is really in the state we want as it might take time
	 * for the device to actually do the change (mainly when moving to rf_enabled).
	 */
	ret = adrv9002_chan_to_state_poll(phy, chann, state, 7);
	if (ret) {
		/*
		 * This is important when the device is in PIN mode as changing it to SPI
		 * might trigger a state change to rf_enabled. In that case it looks like the
		 * first call to @adi_adrv9001_Radio_Channel_ToState() is just ignored as the
		 * device is still busy. Hence we try one last time to move the channel to the
		 * desired state and double up the number of tries...
		 */
		dev_dbg(&phy->spi->dev, "Try to change to state(%d) again...\n", state);
		ret = api_call(phy, adi_adrv9001_Radio_Channel_ToState, chann->port,
			       chann->number, state);
		if (ret)
			return ret;

		ret = adrv9002_chan_to_state_poll(phy, chann, state, 14);
		if (ret)
			return ret;
	}

	if (mode == ADI_ADRV9001_SPI_MODE)
		return 0;

	/* restore enable mode */
	return api_call(phy, adi_adrv9001_Radio_ChannelEnableMode_Set,
			chann->port, chann->number, mode);
}

static struct
adrv9002_chan *adrv9002_get_channel(struct adrv9002_rf_phy *phy,
				    const int port, const int chann)
{
	if (port == ADI_TX)
		return &phy->tx_channels[chann].channel;

	return &phy->rx_channels[chann].channel;
}

enum {
	ADRV9002_HOP_1_TABLE_SEL,
	ADRV9002_HOP_2_TABLE_SEL,
	ADRV9002_HOP_1_TRIGGER,
	ADRV9002_HOP_2_TRIGGER
};

static const char * const adrv9002_hop_table[ADRV9002_FH_TABLES_NR + 1] = {
	"TABLE_A",
	"TABLE_B",
	"Unknown"
};

static int adrv9002_fh_table_show(struct adrv9002_rf_phy *phy, char *buf, u64 address)
{
	adi_adrv9001_FhMode_e mode = phy->fh.mode;
	adi_adrv9001_FhHopTable_e table;
	int ret = -ENOTSUPP;

	mutex_lock(&phy->lock);
	if (!phy->curr_profile->sysConfig.fhModeOn) {
		dev_err(&phy->spi->dev, "Frequency hopping not enabled\n");
		goto out_unlock;
	}

	if (address && mode != ADI_ADRV9001_FHMODE_LO_RETUNE_REALTIME_PROCESS_DUAL_HOP) {
		dev_err(&phy->spi->dev, "HOP2 not supported! FH mode not in dual hop.\n");
		goto out_unlock;
	}

	ret = api_call(phy, adi_adrv9001_fh_HopTable_Get, address, &table);
	if (ret)
		goto out_unlock;
	mutex_unlock(&phy->lock);

	if (table >= ADRV9002_FH_TABLES_NR)
		table = ADRV9002_FH_TABLES_NR;

	return sysfs_emit(buf, "%s\n", adrv9002_hop_table[table]);
out_unlock:
	mutex_unlock(&phy->lock);
	return ret;
}

static ssize_t adrv9002_attr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	struct iio_dev_attr *iio_attr = to_iio_dev_attr(attr);

	switch (iio_attr->address) {
	case ADRV9002_HOP_1_TABLE_SEL:
	case ADRV9002_HOP_2_TABLE_SEL:
		return adrv9002_fh_table_show(phy, buf, iio_attr->address);
	default:
		return -EINVAL;
	}
}

static int adrv9002_attr_do_store(const struct adrv9002_rf_phy *phy, const char *buf, u64 address)
{
	int tbl;

	switch (address) {
	case ADRV9002_HOP_1_TABLE_SEL:
	case ADRV9002_HOP_2_TABLE_SEL:
		for (tbl = 0; tbl < ARRAY_SIZE(adrv9002_hop_table) - 1; tbl++) {
			if (sysfs_streq(buf, adrv9002_hop_table[tbl]))
				break;
		}

		if (tbl == ARRAY_SIZE(adrv9002_hop_table) - 1) {
			dev_err(&phy->spi->dev, "Unknown table %s\n", buf);
			return -EINVAL;
		}

		return api_call(phy, adi_adrv9001_fh_HopTable_Set, address, tbl);
	case ADRV9002_HOP_1_TRIGGER:
		return api_call(phy, adi_adrv9001_fh_Hop, ADI_ADRV9001_FH_HOP_SIGNAL_1);
	case ADRV9002_HOP_2_TRIGGER:
		return api_call(phy, adi_adrv9001_fh_Hop, ADI_ADRV9001_FH_HOP_SIGNAL_2);
	default:
		return  -EINVAL;
	}
}

static ssize_t adrv9002_attr_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	struct iio_dev_attr *iio_attr = to_iio_dev_attr(attr);
	int ret = -ENOTSUPP;

	mutex_lock(&phy->lock);
	if (!phy->curr_profile->sysConfig.fhModeOn) {
		dev_err(&phy->spi->dev, "Frequency hopping not enabled\n");
		goto out_unlock;
	}

	if (iio_attr->address == ADRV9002_HOP_2_TABLE_SEL ||
	    iio_attr->address == ADRV9002_HOP_2_TRIGGER) {
		if (phy->fh.mode != ADI_ADRV9001_FHMODE_LO_RETUNE_REALTIME_PROCESS_DUAL_HOP) {
			dev_err(&phy->spi->dev, "HOP2 not supported! FH mode not in dual hop.\n");
			goto out_unlock;
		}
	}

	ret = adrv9002_attr_do_store(phy, buf, iio_attr->address);

out_unlock:
	mutex_unlock(&phy->lock);
	return ret ? ret : len;
}

static int adrv9002_set_ext_lo(const struct adrv9002_chan *c, u64 freq)
{
	u64 lo_freq;

	if (!c->ext_lo)
		return 0;

	lo_freq = freq * c->ext_lo->divider;
	if (lo_freq < ADRV9002_EXT_LO_FREQ_MIN || lo_freq > ADRV9002_EXT_LO_FREQ_MAX) {
		const struct adrv9002_rf_phy *phy = chan_to_phy(c);

		dev_err(&phy->spi->dev, "Ext LO freq not in the [%d %llu] range\n",
			ADRV9002_EXT_LO_FREQ_MIN, ADRV9002_EXT_LO_FREQ_MAX);
		return -EINVAL;
	}

	return clk_set_rate_scaled(c->ext_lo->clk, freq, &c->ext_lo->scale);
}

static ssize_t adrv9002_phy_lo_do_write(struct adrv9002_rf_phy *phy, struct adrv9002_chan *c,
					const char *buf, size_t len)
{
	struct adi_adrv9001_Carrier lo_freq;
	u64 freq;
	int ret;

	ret = kstrtoull(buf, 10, &freq);
	if (ret)
		return ret;

	mutex_lock(&phy->lock);

	if (!c->enabled) {
		ret = -ENODEV;
		goto out_unlock;
	}

	ret = api_call(phy, adi_adrv9001_Radio_Carrier_Inspect, c->port, c->number, &lo_freq);
	if (ret)
		goto out_unlock;

	ret = adrv9002_set_ext_lo(c, freq);
	if (ret)
		goto out_unlock;

	lo_freq.carrierFrequency_Hz = freq;
	ret = adrv9002_channel_to_state(phy, c, ADI_ADRV9001_CHANNEL_CALIBRATED, true);
	if (ret)
		goto out_unlock;

	ret = api_call(phy, adi_adrv9001_Radio_Carrier_Configure, c->port, c->number, &lo_freq);
	if (ret)
		goto out_unlock;

	ret = adrv9002_channel_to_state(phy, c, c->cached_state, false);

out_unlock:
	mutex_unlock(&phy->lock);
	return ret ? ret : len;
}

static ssize_t adrv9002_phy_lo_write(struct iio_dev *indio_dev,
				     uintptr_t private,
				     const struct iio_chan_spec *chan,
				     const char *buf, size_t len)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const adi_common_Port_e port = ADRV_ADDRESS_PORT(chan->address);
	const int chan_nr = ADRV_ADDRESS_CHAN(chan->address);
	struct adrv9002_chan *chann = adrv9002_get_channel(phy, port, chan_nr);

	switch (private) {
	case LOEXT_FREQ:
		return adrv9002_phy_lo_do_write(phy, chann, buf, len);
	default:
		return -EINVAL;
	}
}

static ssize_t adrv9002_phy_lo_read(struct iio_dev *indio_dev,
				    uintptr_t private,
				    const struct iio_chan_spec *chan,
				    char *buf)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	adi_common_Port_e port = ADRV_ADDRESS_PORT(chan->address);
	const int channel = ADRV_ADDRESS_CHAN(chan->address);
	int ret = -ENODEV;
	struct adrv9002_chan *chann = adrv9002_get_channel(phy, port, channel);
	struct adi_adrv9001_Carrier lo_freq;

	switch (private) {
	case LOEXT_FREQ:
		mutex_lock(&phy->lock);
		if (!chann->enabled)
			goto out_unlock;

		ret = api_call(phy, adi_adrv9001_Radio_Carrier_Inspect, port,
			       chann->number, &lo_freq);
		if (ret)
			goto out_unlock;

		mutex_unlock(&phy->lock);
		return sysfs_emit(buf, "%llu\n", lo_freq.carrierFrequency_Hz);
	default:
		return -EINVAL;
	}

out_unlock:
	mutex_unlock(&phy->lock);
	return ret;
}

#define _ADRV9002_EXT_LO_INFO(_name, _ident) { \
	.name = _name, \
	.read = adrv9002_phy_lo_read, \
	.write = adrv9002_phy_lo_write, \
	.private = _ident, \
}

static const struct iio_chan_spec_ext_info adrv9002_phy_ext_lo_info[] = {
	/* Ideally we use IIO_CHAN_INFO_FREQUENCY, but there are
	 * values > 2^32 in order to support the entire frequency range
	 * in Hz. Using scale is a bit ugly.
	 */
	_ADRV9002_EXT_LO_INFO("frequency", LOEXT_FREQ),
	{ },
};

static int adrv9002_set_agc_mode(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan, u32 mode)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	adi_common_ChannelNumber_e chann = ADRV_ADDRESS_CHAN(chan->address);
	struct adrv9002_rx_chan *rx = &phy->rx_channels[chann];
	int ret;

	if (mode > ADI_ADRV9001_RX_GAIN_CONTROL_MODE_AUTO)
		return -EINVAL;

	mutex_lock(&phy->lock);

	if (!rx->channel.enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}

	ret = api_call(phy, adi_adrv9001_Rx_GainControl_Mode_Set, rx->channel.number, mode);

	mutex_unlock(&phy->lock);

	return ret;
}

static int adrv9002_get_agc_mode(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	adi_adrv9001_RxGainControlMode_e gain_ctrl_mode;
	const int chann = ADRV_ADDRESS_CHAN(chan->address);
	struct adrv9002_rx_chan *rx = &phy->rx_channels[chann];
	int ret;

	mutex_lock(&phy->lock);
	if (!rx->channel.enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}

	ret = api_call(phy, adi_adrv9001_Rx_GainControl_Mode_Get,
		       rx->channel.number, &gain_ctrl_mode);
	mutex_unlock(&phy->lock);

	return ret ? ret : gain_ctrl_mode;
}

static const char * const adrv9002_agc_modes[] = {
	"spi", "pin", "automatic"
};

static const struct iio_enum adrv9002_agc_modes_available = {
	.items = adrv9002_agc_modes,
	.num_items = ARRAY_SIZE(adrv9002_agc_modes),
	.get = adrv9002_get_agc_mode,
	.set = adrv9002_set_agc_mode,
};

static int adrv9002_set_ensm_mode(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan, u32 mode)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	adi_common_Port_e port = ADRV_ADDRESS_PORT(chan->address);
	const int channel = ADRV_ADDRESS_CHAN(chan->address);
	int ret;
	struct adrv9002_chan *chann = adrv9002_get_channel(phy, port, channel);

	mutex_lock(&phy->lock);
	if (!chann->enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}

	if (adrv9002_orx_enabled(phy, chann)) {
		mutex_unlock(&phy->lock);
		return -EPERM;
	}
	/*
	 * In TDD, we cannot have TX and RX enabled at the same time on the same
	 * channel (due to TDD nature). Hence, we will return -EPERM if that is
	 * attempted...
	 */
	if (phy->curr_profile->sysConfig.duplexMode == ADI_ADRV9001_TDD_MODE &&
	    mode + 1 == ADI_ADRV9001_CHANNEL_RF_ENABLED) {
		enum adi_adrv9001_ChannelState state;
		/* just the last bit matters as RX is 0 and TX is 1 */
		adi_common_Port_e __port = ~port & 0x1;

		ret = api_call(phy, adi_adrv9001_Radio_Channel_State_Get, __port,
			       chann->number, &state);
		if (ret)
			goto unlock;

		if (state == ADI_ADRV9001_CHANNEL_RF_ENABLED) {
			ret = -EPERM;
			goto unlock;
		}
	}

	ret = api_call(phy, adi_adrv9001_Radio_Channel_ToState, port, chann->number, mode + 1);
unlock:
	mutex_unlock(&phy->lock);

	return ret;
}

static int adrv9002_get_ensm_mode(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	adi_common_Port_e port = ADRV_ADDRESS_PORT(chan->address);
	const int channel = ADRV_ADDRESS_CHAN(chan->address);
	enum adi_adrv9001_ChannelState state;
	int ret;
	struct adrv9002_chan *chann = adrv9002_get_channel(phy, port, channel);

	mutex_lock(&phy->lock);
	if (!chann->enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}

	ret = api_call(phy, adi_adrv9001_Radio_Channel_State_Get, port, chann->number, &state);
	mutex_unlock(&phy->lock);

	return ret ? ret : state - 1;
}

static const char * const adrv9002_ensm_modes[] = {
	"calibrated", "primed", "rf_enabled"
};

static const struct iio_enum adrv9002_ensm_modes_available = {
	.items = adrv9002_ensm_modes,
	.num_items = ARRAY_SIZE(adrv9002_ensm_modes),
	.get = adrv9002_get_ensm_mode,
	.set = adrv9002_set_ensm_mode,
};

static int adrv9002_set_digital_gain_ctl_mode(struct iio_dev *indio_dev,
					      const struct iio_chan_spec *chan,
					      u32 mode)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int chann = ADRV_ADDRESS_CHAN(chan->address);
	struct adrv9002_rx_chan *rx = &phy->rx_channels[chann];
	int ret;
	struct adi_adrv9001_RxInterfaceGainCtrl rx_intf_gain_mode = {0};
	u32 gain_table_type;

	mutex_lock(&phy->lock);
	if (!rx->channel.enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}

	ret = api_call(phy, adi_adrv9001_Rx_InterfaceGain_Inspect, rx->channel.number,
		       &rx_intf_gain_mode, &gain_table_type);
	if (ret)
		goto unlock;

	rx_intf_gain_mode.controlMode = mode;

	ret = adrv9002_channel_to_state(phy, &rx->channel, ADI_ADRV9001_CHANNEL_CALIBRATED, true);
	if (ret)
		goto unlock;

	ret = api_call(phy, adi_adrv9001_Rx_InterfaceGain_Configure,
		       rx->channel.number, &rx_intf_gain_mode);
	if (ret)
		goto unlock;

	ret = adrv9002_channel_to_state(phy, &rx->channel, rx->channel.cached_state, false);
unlock:
	mutex_unlock(&phy->lock);

	return ret;
}

static int adrv9002_get_digital_gain_ctl_mode(struct iio_dev *indio_dev,
					      const struct iio_chan_spec *chan)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int chann = ADRV_ADDRESS_CHAN(chan->address);
	struct adrv9002_rx_chan *rx = &phy->rx_channels[chann];
	int ret;
	struct adi_adrv9001_RxInterfaceGainCtrl rx_intf_gain_mode;
	u32 gain_table_type;

	mutex_lock(&phy->lock);
	if (!rx->channel.enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}

	ret = api_call(phy, adi_adrv9001_Rx_InterfaceGain_Inspect, rx->channel.number,
		       &rx_intf_gain_mode, &gain_table_type);
	mutex_unlock(&phy->lock);

	return ret ? ret : rx_intf_gain_mode.controlMode;
}

static int adrv9002_get_intf_gain(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int chann = ADRV_ADDRESS_CHAN(chan->address);
	struct adrv9002_rx_chan *rx = &phy->rx_channels[chann];
	int ret;
	adi_adrv9001_RxInterfaceGain_e gain;

	mutex_lock(&phy->lock);
	if (!rx->channel.enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}

	ret = api_call(phy, adi_adrv9001_Rx_InterfaceGain_Get, rx->channel.number, &gain);
	mutex_unlock(&phy->lock);

	return ret ? ret : gain;
}

static int adrv9002_set_intf_gain(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan, u32 mode)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int chann = ADRV_ADDRESS_CHAN(chan->address);
	struct adrv9002_rx_chan *rx = &phy->rx_channels[chann];
	int ret;

	mutex_lock(&phy->lock);
	if (!rx->channel.enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}

	ret = api_call(phy, adi_adrv9001_Rx_InterfaceGain_Set, rx->channel.number, mode);
	mutex_unlock(&phy->lock);

	return ret;
}

static int adrv9002_get_port_en_mode(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int chan_nr = ADRV_ADDRESS_CHAN(chan->address);
	adi_common_Port_e port = ADRV_ADDRESS_PORT(chan->address);
	adi_adrv9001_ChannelEnableMode_e mode;
	int ret;
	struct adrv9002_chan *chann = adrv9002_get_channel(phy, port, chan_nr);

	mutex_lock(&phy->lock);
	if (!chann->enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}

	ret = api_call(phy, adi_adrv9001_Radio_ChannelEnableMode_Get, port, chann->number, &mode);
	mutex_unlock(&phy->lock);

	return ret ? ret : mode;
}

static int adrv9002_set_port_en_mode(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan, u32 mode)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int chan_nr = ADRV_ADDRESS_CHAN(chan->address);
	adi_common_Port_e port = ADRV_ADDRESS_PORT(chan->address);
	int ret;
	struct adrv9002_chan *chann = adrv9002_get_channel(phy, port, chan_nr);

	mutex_lock(&phy->lock);
	if (!chann->enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}

	if (adrv9002_orx_enabled(phy, chann)) {
		/*
		 * Don't allow changing port enable mode if ORx is enabled, because it
		 * might trigger an ensm state transition which can potentially break ORx
		 */
		mutex_unlock(&phy->lock);
		return -EPERM;
	}

	ret = api_call(phy, adi_adrv9001_Radio_ChannelEnableMode_Set, port, chann->number, mode);
	mutex_unlock(&phy->lock);

	return ret;
}

static int adrv9002_update_tracking_calls(const struct adrv9002_rf_phy *phy,
					  const u32 mask, const int chann,
					  const bool enable)
{
	int ret, i;
	struct adi_adrv9001_TrackingCals tracking_cals;

	ret = api_call(phy, adi_adrv9001_cals_Tracking_Get, &tracking_cals);
	if (ret)
		return ret;

	/* all channels need to be in calibrated state...*/
	for (i = 0; i < ARRAY_SIZE(phy->channels); i++) {
		struct adrv9002_chan *c = phy->channels[i];

		ret = adrv9002_channel_to_state(phy, c, ADI_ADRV9001_CHANNEL_CALIBRATED, true);
		if (ret)
			return ret;
	}

	if (enable)
		tracking_cals.chanTrackingCalMask[chann] |= mask;
	else
		tracking_cals.chanTrackingCalMask[chann] &= ~mask;

	ret = api_call(phy, adi_adrv9001_cals_Tracking_Set, &tracking_cals);
	if (ret)
		return ret;

	/* restore state */
	for (i = 0; i < ARRAY_SIZE(phy->channels); i++) {
		struct adrv9002_chan *c = phy->channels[i];

		ret = adrv9002_channel_to_state(phy, c, c->cached_state, false);
		if (ret)
			return ret;
	}

	return 0;
}

static const u32 rx_track_calls[] = {
	[RX_QEC_FIC] = ADI_ADRV9001_TRACKING_CAL_RX_QEC_FIC,
	[RX_QEC_W_POLY] = ADI_ADRV9001_TRACKING_CAL_RX_QEC_WBPOLY,
	[ORX_QEC_W_POLY] = ADI_ADRV9001_TRACKING_CAL_ORX_QEC_WBPOLY,
	[RX_AGC] = ADI_ADRV9001_TRACKING_CAL_RX_GAIN_CONTROL_DETECTORS,
	[RX_TRACK_BBDC] = ADI_ADRV9001_TRACKING_CAL_RX_BBDC,
	[RX_HD2] = ADI_ADRV9001_TRACKING_CAL_RX_HD2,
	[RX_RSSI_CAL] = ADI_ADRV9001_TRACKING_CAL_RX_RSSI,
	[RX_RFDC] = ADI_ADRV9001_TRACKING_CAL_RX_RFDC
};

static int adrv9002_phy_rx_do_write(const struct adrv9002_rf_phy *phy, struct adrv9002_rx_chan *rx,
				    uintptr_t private, const char *buf)
{
	struct adi_adrv9001_RxSettings *rx_settings = &phy->curr_profile->rx;
	struct adi_adrv9001_RxChannelCfg *rx_cfg = &rx_settings->rxChannelCfg[rx->channel.idx];
	int ret, freq_offset_hz;
	bool enable;
	u32 val;

	switch (private) {
	case RX_QEC_FIC:
	case RX_QEC_W_POLY:
	case ORX_QEC_W_POLY:
	case RX_HD2:
	case RX_TRACK_BBDC:
	case RX_AGC:
	case RX_RSSI_CAL:
	case RX_RFDC:
		if (private == ORX_QEC_W_POLY && !rx->orx_en)
			return -ENODEV;

		ret = kstrtobool(buf, &enable);
		if (ret)
			return ret;

		return adrv9002_update_tracking_calls(phy, rx_track_calls[private],
						      rx->channel.idx, enable);
	case RX_NCO_FREQUENCY:
		if (!rx_cfg->profile.rxDpProfile.rxNbDem.rxNbNco.rxNbNcoEn)
			return -ENOTSUPP;

		ret = kstrtoint(buf, 10, &freq_offset_hz);
		if (ret)
			return ret;

		ret = api_call(phy, adi_adrv9001_Rx_FrequencyCorrection_Set, rx->channel.number,
			       freq_offset_hz, true);
		if (ret)
			return ret;

		rx->channel.nco_freq = freq_offset_hz;
		return 0;
	case RX_ADC_SWITCH:
		ret = kstrtobool(buf, &enable);
		if (ret)
			return ret;

		/* we must be in calibrated state */
		ret = adrv9002_channel_to_state(phy, &rx->channel, ADI_ADRV9001_CHANNEL_CALIBRATED,
						true);
		if (ret)
			return ret;

		ret = api_call(phy, adi_adrv9001_Rx_AdcSwitchEnable_Set,
			       rx->channel.number, enable);
		if (ret)
			return ret;

		return adrv9002_channel_to_state(phy, &rx->channel, rx->channel.cached_state,
						 false);
	case RX_BBDC:
		if (!rx->orx_en && rx->channel.port == ADI_ORX)
			return -ENODEV;

		ret = kstrtobool(buf, &enable);
		if (ret)
			return ret;
		/*
		 * Disabling the bbdc will completely disable the algorithm and set the correction
		 * value to 0. The difference with the tracking cal is that disabling it, just
		 * disables the algorithm but the last used correction value is still applied...
		 */
		return api_call(phy, adi_adrv9001_bbdc_RejectionEnable_Set, rx->channel.port,
				rx->channel.number, enable);
	case RX_BBDC_LOOP_GAIN:
		ret = kstrtou32(buf, 10, &val);
		if (ret)
			return ret;

		return api_call(phy, adi_adrv9010_bbdc_LoopGain_Set, rx->channel.number, val);
	default:
		return -EINVAL;
	}
}

static ssize_t adrv9002_phy_rx_write(struct iio_dev *indio_dev,
				     uintptr_t private,
				     const struct iio_chan_spec *chan,
				     const char *buf, size_t len)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int channel = ADRV_ADDRESS_CHAN(chan->address);
	const int port = ADRV_ADDRESS_PORT(chan->address);
	struct adrv9002_rx_chan *rx = &phy->rx_channels[channel];
	int ret = -ENODEV;

	mutex_lock(&phy->lock);
	if (!rx->channel.enabled && port == ADI_RX)
		goto out_unlock;

	ret = adrv9002_phy_rx_do_write(phy, rx, private, buf);

out_unlock:
	mutex_unlock(&phy->lock);
	return ret ? ret : len;
}

static const char * const adrv9002_intf_gain[] = {
	"18dB", "12dB", "6dB", "0dB", "-6dB", "-12dB", "-18dB",
	"-24dB", "-30dB", "-36dB"
};

static int adrv9002_intf_gain_avail(const struct adrv9002_rf_phy *phy,
				    const struct adrv9002_rx_chan *rx, char *buf)
{
	adi_adrv9001_RxChannelCfg_t *rx_cfg = &phy->curr_profile->rx.rxChannelCfg[rx->channel.idx];
	/* typical case: only 0db allowed for correction table (rate >= 1Mhz) */
	u32 off = 3, nelem = 1, i;
	int sz = 0;

	/*
	 * Yes, this could be done at setup time just once
	 * (calculating the offset and number of elements) but this is also not
	 * a fastpath and like this there's no need to add more elements to
	 * struct adrv9002_rx_chan and simplifies the changes.
	 */
	if (rx_cfg->profile.gainTableType) {
		if (rx->channel.rate >= MEGA) {
			nelem = 7;
		} else {
			off = 0;
			nelem = ARRAY_SIZE(adrv9002_intf_gain);
		}
	} else {
		if (rx->channel.rate < MEGA) {
			off = 0;
			nelem = 4;
		}
	}

	for (i = off; i < off + nelem; i++)
		sz += sysfs_emit_at(buf, sz, "%s ", adrv9002_intf_gain[i]);

	buf[sz - 1] = '\n';

	return sz;
}

static int adrv9002_phy_rx_do_read(const struct adrv9002_rf_phy *phy,
				   const struct adrv9002_rx_chan *rx, uintptr_t private, char *buf)
{
	struct adi_adrv9001_RxSettings *rx_settings = &phy->curr_profile->rx;
	struct adi_adrv9001_RxChannelCfg *rx_cfg = &rx_settings->rxChannelCfg[rx->channel.idx];
	struct adi_adrv9001_TrackingCals tracking_cals;
	const u32 *calls_mask = tracking_cals.chanTrackingCalMask;
	adi_adrv9001_BbdcRejectionStatus_e bbdc;
	u32 rssi_pwr_mdb, val;
	u16 dec_pwr_mdb;
	bool enable;
	int ret;

	switch (private) {
	case RX_QEC_FIC:
	case RX_QEC_W_POLY:
	case ORX_QEC_W_POLY:
	case RX_HD2:
	case RX_TRACK_BBDC:
	case RX_AGC:
	case RX_RSSI_CAL:
	case RX_RFDC:
		if (private == ORX_QEC_W_POLY && !rx->orx_en)
			return -ENODEV;

		ret = api_call(phy, adi_adrv9001_cals_Tracking_Get, &tracking_cals);
		if (ret)
			return ret;

		return sysfs_emit(buf, "%d\n",
				  !!(calls_mask[rx->channel.idx] & rx_track_calls[private]));
	case RX_DECIMATION_POWER:
		/* it might depend on proper AGC parameters */
		ret = api_call(phy, adi_adrv9001_Rx_DecimatedPower_Get,
			       rx->channel.number, &dec_pwr_mdb);
		if (ret)
			return ret;

		return sysfs_emit(buf, "%u.%02u dB\n", dec_pwr_mdb / 1000, dec_pwr_mdb % 1000);
	case RX_RSSI:
		ret = api_call(phy, adi_adrv9001_Rx_Rssi_Read, rx->channel.number, &rssi_pwr_mdb);
		if (ret)
			return ret;

		return sysfs_emit(buf, "%u.%02u dB\n", rssi_pwr_mdb / 1000, rssi_pwr_mdb % 1000);
	case RX_RF_BANDWIDTH:
		return  sysfs_emit(buf, "%u\n", rx_cfg->profile.primarySigBandwidth_Hz);
	case RX_NCO_FREQUENCY:
		if (!rx_cfg->profile.rxDpProfile.rxNbDem.rxNbNco.rxNbNcoEn)
			return -ENOTSUPP;

		return sysfs_emit(buf, "%d\n", rx->channel.nco_freq);
	case RX_ADC_SWITCH:
		ret = api_call(phy, adi_adrv9001_Rx_AdcSwitchEnable_Get,
			       rx->channel.number, &enable);
		if (ret)
			return ret;

		return sysfs_emit(buf, "%d\n", enable);
	case RX_BBDC:
		if (!rx->orx_en && rx->channel.port == ADI_ORX)
			return -ENODEV;

		ret = api_call(phy, adi_adrv9001_bbdc_RejectionEnable_Get,
			       rx->channel.port, rx->channel.number, &bbdc);
		if (ret)
			return ret;

		return sysfs_emit(buf, "%d\n", bbdc);
	case RX_BBDC_LOOP_GAIN:
		ret = api_call(phy, adi_adrv9010_bbdc_LoopGain_Get, rx->channel.number, &val);
		if (ret)
			return ret;

		return sysfs_emit(buf, "%u\n", val);
	case RX_INTERFACE_GAIN_AVAIL:
		return adrv9002_intf_gain_avail(phy, rx, buf);
	default:
		return -EINVAL;
	}
}

static ssize_t adrv9002_phy_rx_read(struct iio_dev *indio_dev,
				    uintptr_t private,
				    const struct iio_chan_spec *chan,
				    char *buf)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int channel = ADRV_ADDRESS_CHAN(chan->address);
	const int port = ADRV_ADDRESS_PORT(chan->address);
	struct adrv9002_rx_chan *rx = &phy->rx_channels[channel];
	int ret = -ENODEV;

	mutex_lock(&phy->lock);
	/*
	 * We still want to be able to get the available interface gain values to keep
	 * the same behavior as with IIO_ENUMS.
	 */
	if (!rx->channel.enabled && port == ADI_RX && private != RX_INTERFACE_GAIN_AVAIL)
		goto out_unlock;

	ret = adrv9002_phy_rx_do_read(phy, rx, private, buf);

out_unlock:
	mutex_unlock(&phy->lock);
	return ret;
}

#define _ADRV9002_EXT_RX_INFO(_name, _ident) { \
	.name = _name, \
	.read = adrv9002_phy_rx_read, \
	.write = adrv9002_phy_rx_write, \
	.private = _ident, \
}

static const u32 tx_track_calls[] = {
	[TX_QEC] = ADI_ADRV9001_TRACKING_CAL_TX_QEC,
	[TX_LOL] = ADI_ADRV9001_TRACKING_CAL_TX_LO_LEAKAGE,
	[TX_LB_PD] = ADI_ADRV9001_TRACKING_CAL_TX_LB_PD,
	[TX_PAC] = ADI_ADRV9001_TRACKING_CAL_TX_PAC,
	[TX_CLGC] = ADI_ADRV9001_TRACKING_CAL_TX_DPD_CLGC
};

static int adrv9002_set_atten_control_mode(struct iio_dev *indio_dev,
					   const struct iio_chan_spec *chan,
					   u32 mode)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int chann = ADRV_ADDRESS_CHAN(chan->address);
	struct adrv9002_tx_chan *tx = &phy->tx_channels[chann];
	adi_adrv9001_TxAttenuationControlMode_e tx_mode;
	int ret;

	switch (mode) {
	case 0:
		tx_mode = ADI_ADRV9001_TX_ATTENUATION_CONTROL_MODE_BYPASS;
		break;
	case 1:
		tx_mode = ADI_ADRV9001_TX_ATTENUATION_CONTROL_MODE_SPI;
		break;
	case 2:
		tx_mode = ADI_ADRV9001_TX_ATTENUATION_CONTROL_MODE_PIN;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&phy->lock);
	if (!tx->channel.enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}
	/* we must be in calibrated state */
	ret = adrv9002_channel_to_state(phy, &tx->channel, ADI_ADRV9001_CHANNEL_CALIBRATED, true);
	if (ret)
		goto unlock;

	ret = api_call(phy, adi_adrv9001_Tx_AttenuationMode_Set, tx->channel.number, tx_mode);
	if (ret)
		goto unlock;

	ret = adrv9002_channel_to_state(phy, &tx->channel, tx->channel.cached_state, false);
unlock:
	mutex_unlock(&phy->lock);

	return ret;
}

static int adrv9002_get_atten_control_mode(struct iio_dev *indio_dev,
					   const struct iio_chan_spec *chan)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int chann = ADRV_ADDRESS_CHAN(chan->address);
	struct adrv9002_tx_chan *tx = &phy->tx_channels[chann];
	adi_adrv9001_TxAttenuationControlMode_e tx_mode;
	int mode, ret;

	mutex_lock(&phy->lock);
	if (!tx->channel.enabled) {
		mutex_unlock(&phy->lock);
		return -ENODEV;
	}

	ret = api_call(phy, adi_adrv9001_Tx_AttenuationMode_Get, tx->channel.number, &tx_mode);
	mutex_unlock(&phy->lock);

	if (ret)
		return ret;

	switch (tx_mode) {
	case ADI_ADRV9001_TX_ATTENUATION_CONTROL_MODE_BYPASS:
		mode = 0;
		break;
	case ADI_ADRV9001_TX_ATTENUATION_CONTROL_MODE_SPI:
		mode = 1;
		break;
	case ADI_ADRV9001_TX_ATTENUATION_CONTROL_MODE_PIN:
		mode = 2;
		break;
	default:
		return -EINVAL;
	}

	return mode;
}

static int adrv9002_phy_tx_do_read(const struct adrv9002_rf_phy *phy,
				   const struct adrv9002_chan *tx, uintptr_t private, char *buf)
{
	struct adi_adrv9001_TxProfile *tx_cfg = &phy->curr_profile->tx.txProfile[tx->idx];
	struct adi_adrv9001_TrackingCals tracking_cals;
	const u32 *calls_mask = tracking_cals.chanTrackingCalMask;
	int ret;

	switch (private) {
	case TX_QEC:
	case TX_LOL:
	case TX_LB_PD:
	case TX_PAC:
	case TX_CLGC:
		ret = api_call(phy, adi_adrv9001_cals_Tracking_Get, &tracking_cals);
		if (ret)
			return ret;

		return sysfs_emit(buf, "%d\n", !!(calls_mask[tx->idx] & tx_track_calls[private]));
	case TX_RF_BANDWIDTH:
		return sysfs_emit(buf, "%d\n", tx_cfg->primarySigBandwidth_Hz);
	case TX_NCO_FREQUENCY:
		/*
		 * This field seems to be the only thing that changes on TX profiles when nco
		 * is enabled.
		 */
		if (tx_cfg->txDpProfile.txIqdmDuc.iqdmDucMode != ADI_ADRV9001_TX_DP_IQDMDUC_MODE2)
			return -ENOTSUPP;

		return sysfs_emit(buf, "%d\n", tx->nco_freq);
	default:
		return -EINVAL;
	}
}

static ssize_t adrv9002_phy_tx_read(struct iio_dev *indio_dev,
				    uintptr_t private,
				    const struct iio_chan_spec *chan,
				    char *buf)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int channel = ADRV_ADDRESS_CHAN(chan->address);
	struct adrv9002_tx_chan *tx = &phy->tx_channels[channel];
	int ret = -ENODEV;

	mutex_lock(&phy->lock);
	if (!tx->channel.enabled)
		goto out_unlock;

	ret = adrv9002_phy_tx_do_read(phy, &tx->channel, private, buf);

out_unlock:
	mutex_unlock(&phy->lock);
	return ret;
}

static int adrv9002_phy_tx_do_write(const struct adrv9002_rf_phy *phy, struct adrv9002_chan *tx,
				    uintptr_t private, const char *buf)
{
	struct adi_adrv9001_TxProfile *tx_cfg = &phy->curr_profile->tx.txProfile[tx->idx];
	int ret, nco_freq_hz;
	bool en;

	switch (private) {
	case TX_QEC:
	case TX_LOL:
	case TX_LB_PD:
	case TX_PAC:
	case TX_CLGC:
		ret = kstrtobool(buf, &en);
		if (ret)
			return ret;

		return adrv9002_update_tracking_calls(phy, tx_track_calls[private], tx->idx, en);
	case TX_NCO_FREQUENCY:
		if (tx_cfg->txDpProfile.txIqdmDuc.iqdmDucMode != ADI_ADRV9001_TX_DP_IQDMDUC_MODE2)
			return -ENOTSUPP;

		ret = kstrtoint(buf, 10, &nco_freq_hz);
		if (ret)
			return ret;

		ret = api_call(phy, adi_adrv9001_Tx_FrequencyCorrection_Set,
			       tx->number, nco_freq_hz, true);
		if (ret)
			return ret;

		tx->nco_freq = nco_freq_hz;
		return 0;
	default:
		return -EINVAL;
	}
}

static ssize_t adrv9002_phy_tx_write(struct iio_dev *indio_dev,
				     uintptr_t private,
				     const struct iio_chan_spec *chan,
				     const char *buf, size_t len)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int channel = ADRV_ADDRESS_CHAN(chan->address);
	struct adrv9002_tx_chan *tx = &phy->tx_channels[channel];
	int ret = -ENODEV;

	mutex_lock(&phy->lock);
	if (!tx->channel.enabled)
		goto out_unlock;

	ret = adrv9002_phy_tx_do_write(phy, &tx->channel, private, buf);

out_unlock:
	mutex_unlock(&phy->lock);
	return ret ? ret : len;
}

#define _ADRV9002_EXT_TX_INFO(_name, _ident) { \
	.name = _name, \
	.read = adrv9002_phy_tx_read, \
	.write = adrv9002_phy_tx_write, \
	.private = _ident, \
}

static const char * const adrv9002_digital_gain_ctl_modes[] = {
	"automatic", "spi"
};

static const struct iio_enum adrv9002_digital_gain_ctl_modes_available = {
	.items = adrv9002_digital_gain_ctl_modes,
	.num_items = ARRAY_SIZE(adrv9002_digital_gain_ctl_modes),
	.get = adrv9002_get_digital_gain_ctl_mode,
	.set = adrv9002_set_digital_gain_ctl_mode,
};

static const struct iio_enum adrv9002_intf_gain_available = {
	.items = adrv9002_intf_gain,
	.num_items = ARRAY_SIZE(adrv9002_intf_gain),
	.get = adrv9002_get_intf_gain,
	.set = adrv9002_set_intf_gain,
};

static const char *const adrv9002_port_en_mode[] = {
	"spi", "pin"
};

static const struct iio_enum adrv9002_port_en_modes_available = {
	.items = adrv9002_port_en_mode,
	.num_items = ARRAY_SIZE(adrv9002_port_en_mode),
	.get = adrv9002_get_port_en_mode,
	.set = adrv9002_set_port_en_mode,
};

static const char *const adrv9002_atten_control_mode[] = {
	"bypass", "spi", "pin"
};

static const struct iio_enum adrv9002_atten_control_mode_available = {
	.items = adrv9002_atten_control_mode,
	.num_items = ARRAY_SIZE(adrv9002_atten_control_mode),
	.get = adrv9002_get_atten_control_mode,
	.set = adrv9002_set_atten_control_mode,
};

static const struct iio_chan_spec_ext_info adrv9002_phy_rx_ext_info[] = {
	/* Ideally we use IIO_CHAN_INFO_FREQUENCY, but there are
	 * values > 2^32 in order to support the entire frequency range
	 * in Hz. Using scale is a bit ugly.
	 */
	IIO_ENUM_AVAILABLE_SHARED("ensm_mode", 0,
				  &adrv9002_ensm_modes_available),
	IIO_ENUM("ensm_mode", 0, &adrv9002_ensm_modes_available),
	IIO_ENUM_AVAILABLE_SHARED("gain_control_mode", 0,
				  &adrv9002_agc_modes_available),
	IIO_ENUM("gain_control_mode", 0, &adrv9002_agc_modes_available),
	IIO_ENUM_AVAILABLE_SHARED("digital_gain_control_mode", 0,
				  &adrv9002_digital_gain_ctl_modes_available),
	IIO_ENUM("digital_gain_control_mode", 0,
		 &adrv9002_digital_gain_ctl_modes_available),
	_ADRV9002_EXT_RX_INFO("interface_gain_available", RX_INTERFACE_GAIN_AVAIL),
	IIO_ENUM("interface_gain", 0,
		 &adrv9002_intf_gain_available),
	IIO_ENUM_AVAILABLE_SHARED("port_en_mode", 0,
				  &adrv9002_port_en_modes_available),
	IIO_ENUM("port_en_mode", 0, &adrv9002_port_en_modes_available),
	_ADRV9002_EXT_RX_INFO("rssi", RX_RSSI),
	_ADRV9002_EXT_RX_INFO("decimated_power", RX_DECIMATION_POWER),
	_ADRV9002_EXT_RX_INFO("rf_bandwidth", RX_RF_BANDWIDTH),
	_ADRV9002_EXT_RX_INFO("nco_frequency", RX_NCO_FREQUENCY),
	_ADRV9002_EXT_RX_INFO("quadrature_fic_tracking_en", RX_QEC_FIC),
	_ADRV9002_EXT_RX_INFO("quadrature_w_poly_tracking_en", RX_QEC_W_POLY),
	_ADRV9002_EXT_RX_INFO("agc_tracking_en", RX_AGC),
	_ADRV9002_EXT_RX_INFO("bbdc_rejection_tracking_en", RX_TRACK_BBDC),
	_ADRV9002_EXT_RX_INFO("hd_tracking_en", RX_HD2),
	_ADRV9002_EXT_RX_INFO("rssi_tracking_en", RX_RSSI_CAL),
	_ADRV9002_EXT_RX_INFO("rfdc_tracking_en", RX_RFDC),
	_ADRV9002_EXT_RX_INFO("dynamic_adc_switch_en", RX_ADC_SWITCH),
	_ADRV9002_EXT_RX_INFO("bbdc_rejection_en", RX_BBDC),
	_ADRV9002_EXT_RX_INFO("bbdc_loop_gain_raw", RX_BBDC_LOOP_GAIN),
	{ },
};

static const struct iio_chan_spec_ext_info adrv9002_phy_orx_ext_info[] = {
	_ADRV9002_EXT_RX_INFO("bbdc_rejection_en", RX_BBDC),
	_ADRV9002_EXT_RX_INFO("quadrature_w_poly_tracking_en", ORX_QEC_W_POLY),
	{ },
};

static struct iio_chan_spec_ext_info adrv9002_phy_tx_ext_info[] = {
	IIO_ENUM_AVAILABLE_SHARED("ensm_mode", 0,
				  &adrv9002_ensm_modes_available),
	IIO_ENUM("ensm_mode", 0, &adrv9002_ensm_modes_available),
	IIO_ENUM_AVAILABLE_SHARED("port_en_mode", 0,
				  &adrv9002_port_en_modes_available),
	IIO_ENUM("port_en_mode", 0, &adrv9002_port_en_modes_available),
	IIO_ENUM_AVAILABLE_SHARED("atten_control_mode", 0,
				  &adrv9002_atten_control_mode_available),
	IIO_ENUM("atten_control_mode", 0,
		 &adrv9002_atten_control_mode_available),
	_ADRV9002_EXT_TX_INFO("rf_bandwidth", TX_RF_BANDWIDTH),
	_ADRV9002_EXT_TX_INFO("quadrature_tracking_en", TX_QEC),
	_ADRV9002_EXT_TX_INFO("lo_leakage_tracking_en", TX_LOL),
	_ADRV9002_EXT_TX_INFO("loopback_delay_tracking_en", TX_LB_PD),
	_ADRV9002_EXT_TX_INFO("pa_correction_tracking_en", TX_PAC),
	_ADRV9002_EXT_TX_INFO("close_loop_gain_tracking_en", TX_CLGC),
	_ADRV9002_EXT_TX_INFO("nco_frequency", TX_NCO_FREQUENCY),
	{ },
};

static int adrv9002_channel_power_set(const struct adrv9002_rf_phy *phy,
				      struct adrv9002_chan *channel, const int val)
{
	int ret;

	dev_dbg(&phy->spi->dev, "Set power: %d, chan: %d, port: %d\n",
		val, channel->number, channel->port);

	if (!val && channel->power) {
		ret = adrv9002_channel_to_state(phy, channel, ADI_ADRV9001_CHANNEL_CALIBRATED,
						true);
		if (ret)
			return ret;

		ret = api_call(phy, adi_adrv9001_Radio_Channel_PowerDown,
			       channel->port, channel->number);
		if (ret)
			return ret;

		channel->power = false;
	} else if (val && !channel->power) {
		ret = api_call(phy, adi_adrv9001_Radio_Channel_PowerUp,
			       channel->port, channel->number);
		if (ret)
			return ret;

		ret = adrv9002_channel_to_state(phy, channel, channel->cached_state, false);
		if (ret)
			return ret;

		channel->power = true;
	}

	return 0;
}

static int adrv9002_phy_read_raw_no_rf_chan(const struct adrv9002_rf_phy *phy,
					    const struct iio_chan_spec *chan,
					    int *val, int *val2, long m)
{
	int ret;
	bool en;
	u16 temp;

	switch (m) {
	case IIO_CHAN_INFO_ENABLE:
		if (chan->output)
			ret = api_call(phy, adi_adrv9001_AuxDac_Inspect, chan->address, &en);
		else
			ret = api_call(phy, adi_adrv9001_AuxAdc_Inspect, chan->address, &en);
		if (ret)
			return ret;

		*val = en;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_PROCESSED:
		switch (chan->type) {
		case IIO_TEMP:
			ret = api_call(phy, adi_adrv9001_Temperature_Get, &temp);
			if (ret)
				return ret;

			*val = temp * 1000;
			return IIO_VAL_INT;
		case IIO_VOLTAGE:
			if (!chan->output) {
				ret = api_call(phy, adi_adrv9001_AuxAdc_Voltage_Get,
					       chan->address, &temp);
				if (ret)
					return ret;

				*val = temp;
				return IIO_VAL_INT;
			}

			ret = api_call(phy, adi_adrv9001_AuxDac_Code_Get, chan->address, &temp);
			if (ret)
				return ret;

			*val = 900 + DIV_ROUND_CLOSEST((temp - 2048) * 1700, 4096);
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	};
}

static int adrv9002_hardware_gain_get(const struct adrv9002_rf_phy *phy,
				      const struct adrv9002_chan *c, int *val, int *val2)
{
	int temp, ret;
	u8 index;

	if (c->port == ADI_TX) {
		u16 atten_mdb;

		ret = api_call(phy, adi_adrv9001_Tx_Attenuation_Get, c->number, &atten_mdb);
		if (ret)
			return ret;

		*val = -1 * (atten_mdb / 1000);
		*val2 = (atten_mdb % 1000) * 1000;
		if (!*val)
			*val2 *= -1;

		return IIO_VAL_INT_PLUS_MICRO_DB;
	}

	if (c->port == ADI_ORX)
		ret = api_call(phy, adi_adrv9001_ORx_Gain_Get, c->number, &index);
	else
		ret = api_call(phy, adi_adrv9001_Rx_Gain_Get, c->number, &index);
	if (ret)
		return ret;

	temp = adrv9002_gainidx_to_gain(index, c->port);
	*val = temp / 1000;
	*val2 = temp % 1000 * 1000;

	return IIO_VAL_INT_PLUS_MICRO_DB;
}

static int adrv9002_phy_read_raw_rf_chan(const struct adrv9002_rf_phy *phy,
					 const struct adrv9002_chan *chann, int *val,
					 int *val2, long m)
{
	switch (m) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		return adrv9002_hardware_gain_get(phy, chann, val, val2);
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = clk_get_rate(chann->clk);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_ENABLE:
		if (chann->port == ADI_ORX) {
			struct adrv9002_rx_chan *rx = chan_to_rx(chann);

			if (!rx->orx_gpio)
				return -ENOTSUPP;

			*val = gpiod_get_value_cansleep(rx->orx_gpio);
			return IIO_VAL_INT;
		}

		*val = chann->power;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int adrv9002_phy_read_raw(struct iio_dev *indio_dev,
				 const struct iio_chan_spec *chan, int *val,
				 int *val2, long m)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	struct adrv9002_chan *chann;
	const int chan_nr = ADRV_ADDRESS_CHAN(chan->address);
	const adi_common_Port_e port = ADRV_ADDRESS_PORT(chan->address);
	int ret = -ENODEV;

	mutex_lock(&phy->lock);
	if (chan->type != IIO_VOLTAGE || chan->channel > ADRV9002_CHANN_2) {
		ret = adrv9002_phy_read_raw_no_rf_chan(phy, chan, val, val2, m);
		goto out_unlock;
	}

	chann = adrv9002_get_channel(phy, port, chan_nr);
	if (port == ADI_ORX) {
		struct adrv9002_rx_chan *rx = chan_to_rx(chann);

		if (!rx->orx_en)
			goto out_unlock;
	} else if (!chann->enabled) {
		goto out_unlock;
	}

	ret = adrv9002_phy_read_raw_rf_chan(phy, chann, val, val2, m);

out_unlock:
	mutex_unlock(&phy->lock);
	return ret;
};

static int adrv9002_phy_write_raw_no_rf_chan(const struct adrv9002_rf_phy *phy,
					     const struct iio_chan_spec *chan, int val,
					     int val2, long mask)
{
	u16 code;

	switch (mask) {
	case IIO_CHAN_INFO_ENABLE:
		if (chan->output)
			return api_call(phy, adi_adrv9001_AuxDac_Configure, chan->address, val);

		return api_call(phy, adi_adrv9001_AuxAdc_Configure, chan->address, val);
	case IIO_CHAN_INFO_PROCESSED:
		code = clamp_val(val, 50, 1750);
		code = 2048 + DIV_ROUND_CLOSEST((code - 900) * 4096, 1700);
		if (code == 4096)
			code = 4095;

		return api_call(phy, adi_adrv9001_AuxDac_Code_Set, chan->address, code);
	default:
		return -EINVAL;
	}
}

static bool adrv9002_orx_can_enable(const struct adrv9002_rf_phy *phy,
				    const struct adrv9002_chan *rx)
{
	adi_adrv9001_ChannelState_e state;
	int ret;

	/* We can't enable ORx if RX is already enabled */
	ret = api_call(phy, adi_adrv9001_Radio_Channel_State_Get, ADI_RX, rx->number, &state);
	if (ret)
		return false;

	if (state == ADI_ADRV9001_CHANNEL_RF_ENABLED) {
		dev_err(&phy->spi->dev, "RX%d cannot be enabled in order to enable ORx%d\n",
			rx->number, rx->number);
		return false;
	}

	/*
	 * TX must be enabled in order to enable ORx. We just use the rx object as we are
	 * interested in the TX on the same channel
	 */
	ret = api_call(phy, adi_adrv9001_Radio_Channel_State_Get, ADI_TX, rx->number, &state);
	if (ret)
		return false;

	if (state != ADI_ADRV9001_CHANNEL_RF_ENABLED) {
		dev_err(&phy->spi->dev, "TX%d must be enabled in order to enable ORx%d\n",
			rx->number, rx->number);
		return false;
	}

	return true;
}

static int adrv9002_hardware_gain_set(const struct adrv9002_rf_phy *phy,
				      const struct adrv9002_chan *c, int val, int val2)
{
	int gain;
	u32 code;
	u8 idx;

	if (c->port == ADI_TX) {
		if (val > 0 || (val == 0 && val2 > 0))
			return -EINVAL;

		code = ((abs(val) * 1000) + (abs(val2) / 1000));
		return api_call(phy, adi_adrv9001_Tx_Attenuation_Set, c->number, code);
	}

	gain = val * 1000 + val2 / 1000;
	idx = adrv9002_gain_to_gainidx(gain, c->port);

	if (c->port == ADI_RX)
		return api_call(phy, adi_adrv9001_Rx_Gain_Set, c->number, idx);

	return api_call(phy, adi_adrv9001_ORx_Gain_Set, c->number, idx);
}

static int adrv9002_phy_write_raw_rf_chan(const struct adrv9002_rf_phy *phy,
					  struct adrv9002_chan *chann, int val, int val2, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		return adrv9002_hardware_gain_set(phy, chann, val, val2);
	case IIO_CHAN_INFO_ENABLE:
		if (chann->port == ADI_ORX) {
			struct adrv9002_rx_chan *rx = chan_to_rx(chann);

			if (!rx->orx_gpio)
				return -ENOTSUPP;

			if (val && !adrv9002_orx_can_enable(phy, chann))
				return -EPERM;

			gpiod_set_value_cansleep(rx->orx_gpio, !!val);
			return 0;
		}

		return adrv9002_channel_power_set(phy, chann, val);
	default:
		return -EINVAL;
	}
}

static int adrv9002_phy_write_raw(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan, int val,
				  int val2, long mask)
{
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	const int chan_nr = ADRV_ADDRESS_CHAN(chan->address);
	const adi_common_Port_e port = ADRV_ADDRESS_PORT(chan->address);
	struct adrv9002_chan *chann;
	int ret = -ENODEV;

	mutex_lock(&phy->lock);
	if (chan->type != IIO_VOLTAGE || chan->channel > ADRV9002_CHANN_2) {
		ret = adrv9002_phy_write_raw_no_rf_chan(phy, chan, val, val2, mask);
		goto out_unlock;
	}

	chann = adrv9002_get_channel(phy, port, chan_nr);
	if (port == ADI_ORX) {
		struct adrv9002_rx_chan *rx = chan_to_rx(chann);

		if (!rx->orx_en)
			goto out_unlock;
	} else if (!chann->enabled) {
		goto out_unlock;
	}

	ret = adrv9002_phy_write_raw_rf_chan(phy, chann, val, val2, mask);

out_unlock:
	mutex_unlock(&phy->lock);
	return ret;
}

#define ADRV9002_IIO_LO_CHAN(idx, name, port, chan) {	\
	.type = IIO_ALTVOLTAGE,				\
	.indexed = 1,					\
	.output = 1,					\
	.channel = idx,					\
	.extend_name = name,				\
	.ext_info = adrv9002_phy_ext_lo_info,		\
	.address = ADRV_ADDRESS(port, chan),		\
}

#define ADRV9002_IIO_TX_CHAN(idx, port, chan) {			\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = idx,						\
	.output = 1,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_HARDWAREGAIN) | \
			BIT(IIO_CHAN_INFO_ENABLE) |		\
			BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
	.ext_info = adrv9002_phy_tx_ext_info,			\
	.address = ADRV_ADDRESS(port, chan),			\
}

#define ADRV9002_IIO_RX_CHAN(idx, port, chan) {			\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = idx,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_HARDWAREGAIN) | \
			BIT(IIO_CHAN_INFO_ENABLE) |		\
			BIT(IIO_CHAN_INFO_SAMP_FREQ),		\
	.ext_info = adrv9002_phy_rx_ext_info,			\
	.address = ADRV_ADDRESS(port, chan),			\
}

#define ADRV9002_IIO_ORX_CHAN(idx, port, chan) {		\
	.type = IIO_VOLTAGE,                                    \
	.indexed = 1,                                           \
	.channel = idx,                                         \
	.info_mask_separate = BIT(IIO_CHAN_INFO_HARDWAREGAIN) |	\
			BIT(IIO_CHAN_INFO_ENABLE),		\
	.extend_name = "orx",                                   \
	.ext_info = adrv9002_phy_orx_ext_info,                  \
	.address = ADRV_ADDRESS(port, chan),                    \
}

#define ADRV9002_IIO_AUX_CONV_CHAN(idx, out, chan) {		\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = idx,						\
	.output = out,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) |	\
			BIT(IIO_CHAN_INFO_ENABLE),		\
	.address = chan,					\
}

static const struct iio_chan_spec adrv9002_phy_chan[] = {
	ADRV9002_IIO_LO_CHAN(0, "RX1_LO", ADI_RX, ADRV9002_CHANN_1),
	ADRV9002_IIO_LO_CHAN(1, "RX2_LO", ADI_RX, ADRV9002_CHANN_2),
	ADRV9002_IIO_LO_CHAN(2, "TX1_LO", ADI_TX, ADRV9002_CHANN_1),
	ADRV9002_IIO_LO_CHAN(3, "TX2_LO", ADI_TX, ADRV9002_CHANN_2),
	ADRV9002_IIO_TX_CHAN(0, ADI_TX, ADRV9002_CHANN_1),
	ADRV9002_IIO_TX_CHAN(1, ADI_TX, ADRV9002_CHANN_2),
	ADRV9002_IIO_AUX_CONV_CHAN(2, true, ADI_ADRV9001_AUXDAC0),
	ADRV9002_IIO_AUX_CONV_CHAN(3, true, ADI_ADRV9001_AUXDAC1),
	ADRV9002_IIO_AUX_CONV_CHAN(4, true, ADI_ADRV9001_AUXDAC2),
	ADRV9002_IIO_AUX_CONV_CHAN(5, true, ADI_ADRV9001_AUXDAC3),
	ADRV9002_IIO_RX_CHAN(0, ADI_RX, ADRV9002_CHANN_1),
	ADRV9002_IIO_RX_CHAN(1, ADI_RX, ADRV9002_CHANN_2),
	/*
	 * as Orx shares the same data path as Rx, let's just point
	 * them to the same Rx index...
	 */
	ADRV9002_IIO_ORX_CHAN(0, ADI_ORX, ADRV9002_CHANN_1),
	ADRV9002_IIO_ORX_CHAN(1, ADI_ORX, ADRV9002_CHANN_2),
	ADRV9002_IIO_AUX_CONV_CHAN(2, false, ADI_ADRV9001_AUXADC0),
	ADRV9002_IIO_AUX_CONV_CHAN(3, false, ADI_ADRV9001_AUXADC1),
	ADRV9002_IIO_AUX_CONV_CHAN(4, false, ADI_ADRV9001_AUXADC2),
	ADRV9002_IIO_AUX_CONV_CHAN(5, false, ADI_ADRV9001_AUXADC3),
	{
		.type = IIO_TEMP,
		.indexed = 1,
		.channel = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
};

static const struct iio_chan_spec adrv9003_phy_chan[] = {
	ADRV9002_IIO_LO_CHAN(0, "RX1_LO", ADI_RX, ADRV9002_CHANN_1),
	ADRV9002_IIO_LO_CHAN(1, "RX2_LO", ADI_RX, ADRV9002_CHANN_2),
	ADRV9002_IIO_LO_CHAN(2, "TX1_LO", ADI_TX, ADRV9002_CHANN_1),
	ADRV9002_IIO_TX_CHAN(0, ADI_TX, ADRV9002_CHANN_1),
	ADRV9002_IIO_AUX_CONV_CHAN(1, true, ADI_ADRV9001_AUXDAC0),
	ADRV9002_IIO_AUX_CONV_CHAN(2, true, ADI_ADRV9001_AUXDAC1),
	ADRV9002_IIO_AUX_CONV_CHAN(3, true, ADI_ADRV9001_AUXDAC2),
	ADRV9002_IIO_AUX_CONV_CHAN(4, true, ADI_ADRV9001_AUXDAC3),
	ADRV9002_IIO_RX_CHAN(0, ADI_RX, ADRV9002_CHANN_1),
	ADRV9002_IIO_RX_CHAN(1, ADI_RX, ADRV9002_CHANN_2),
	/*
	 * as Orx shares the same data path as Rx, let's just point
	 * them to the same Rx index...
	 */
	ADRV9002_IIO_ORX_CHAN(0, ADI_ORX, ADRV9002_CHANN_1),
	ADRV9002_IIO_AUX_CONV_CHAN(2, false, ADI_ADRV9001_AUXADC0),
	ADRV9002_IIO_AUX_CONV_CHAN(3, false, ADI_ADRV9001_AUXADC1),
	ADRV9002_IIO_AUX_CONV_CHAN(4, false, ADI_ADRV9001_AUXADC2),
	ADRV9002_IIO_AUX_CONV_CHAN(5, false, ADI_ADRV9001_AUXADC3),
	{
		.type = IIO_TEMP,
		.indexed = 1,
		.channel = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
};

static IIO_CONST_ATTR(frequency_hopping_hop_table_select_available, "TABLE_A TABLE_B");
static IIO_DEVICE_ATTR(frequency_hopping_hop1_table_select, 0600, adrv9002_attr_show,
		       adrv9002_attr_store, ADRV9002_HOP_1_TABLE_SEL);
static IIO_DEVICE_ATTR(frequency_hopping_hop2_table_select, 0600, adrv9002_attr_show,
		       adrv9002_attr_store, ADRV9002_HOP_2_TABLE_SEL);
static IIO_DEVICE_ATTR(frequency_hopping_hop1_signal_trigger, 0200, NULL, adrv9002_attr_store,
		       ADRV9002_HOP_1_TRIGGER);
static IIO_DEVICE_ATTR(frequency_hopping_hop2_signal_trigger, 0200, NULL, adrv9002_attr_store,
		       ADRV9002_HOP_2_TRIGGER);

static struct attribute *adrv9002_sysfs_attrs[] = {
	&iio_const_attr_frequency_hopping_hop_table_select_available.dev_attr.attr,
	&iio_dev_attr_frequency_hopping_hop1_table_select.dev_attr.attr,
	&iio_dev_attr_frequency_hopping_hop2_table_select.dev_attr.attr,
	&iio_dev_attr_frequency_hopping_hop1_signal_trigger.dev_attr.attr,
	&iio_dev_attr_frequency_hopping_hop2_signal_trigger.dev_attr.attr,
	NULL
};

static const struct attribute_group adrv9002_sysfs_attrs_group = {
	.attrs = adrv9002_sysfs_attrs,
};

static const struct iio_info adrv9002_phy_info = {
	.attrs = &adrv9002_sysfs_attrs_group,
	.read_raw = &adrv9002_phy_read_raw,
	.write_raw = &adrv9002_phy_write_raw,
	.debugfs_reg_access = &adrv9002_phy_reg_access,
};

static const struct {
	char *irq_source;
	int action;
} adrv9002_irqs[] = {
	{"ARM error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"Force GP interrupt(Set by firmware to send an interrupt to BBIC)",
	 ADI_ADRV9001_ACT_ERR_BBIC_LOG_ERROR},
	{"ARM System error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"ARM Calibration error", ADI_ADRV9001_ACT_WARN_RERUN_TRCK_CAL},
	{"ARM monitor interrupt", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"TX1 PA protection error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"TX2 PA protection error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"Low-power PLL lock indicator", ADI_ADRV9001_ACT_ERR_BBIC_LOG_ERROR},
	{"RF PLL1 lock indicator", ADI_ADRV9001_ACT_ERR_BBIC_LOG_ERROR},
	{"RF PLL2 lock indicator", ADI_ADRV9001_ACT_ERR_BBIC_LOG_ERROR},
	{"Auxiliary Clock PLL lock indicator", ADI_ADRV9001_ACT_ERR_BBIC_LOG_ERROR},
	{"Clock PLL lock indicator", ADI_ADRV9001_ACT_ERR_BBIC_LOG_ERROR},
	{"Main clock 1105 MCS", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"Main clock 1105 second MCS", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"RX1 LSSI MCS", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"RX2 LSSI MCS", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"Core stream processor error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"Stream0 error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"Stream1 error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"Stream2 error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"Stream3 error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"Unknown GP Interrupt source"},
	{"Unknown GP Interrupt source"},
	{"Unknown GP Interrupt source"},
	{"TX DP write request error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"RX DP write request error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"SPI interface transmit error", ADI_COMMON_ACT_ERR_RESET_FULL},
	{"SPI interface receive error", ADI_COMMON_ACT_ERR_RESET_FULL},
};

static irqreturn_t adrv9002_irq_handler(int irq, void *p)
{
	struct iio_dev *indio_dev = p;
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	int ret, bit;
	u32 status;
	unsigned long active_irq;
	u8 error;

	mutex_lock(&phy->lock);
	ret = api_call(phy, adi_adrv9001_gpio_GpIntStatus_Get, &status);
	if (ret)
		goto irq_done;

	dev_dbg(&phy->spi->dev, "GP Interrupt Status 0x%08X Mask 0x%08X\n",
		status, ADRV9002_IRQ_MASK);

	active_irq = status & ADRV9002_IRQ_MASK;
	for_each_set_bit(bit, &active_irq, ARRAY_SIZE(adrv9002_irqs)) {
		/* check if there's something to be done */
		switch (adrv9002_irqs[bit].action) {
		case ADI_ADRV9001_ACT_WARN_RERUN_TRCK_CAL:
			dev_warn(&phy->spi->dev, "Re-running tracking calibrations\n");
			api_call(phy, adi_adrv9001_cals_InitCals_Run,
				 &phy->init_cals, 60000, &error);
			break;
		case ADI_COMMON_ACT_ERR_RESET_FULL:
			dev_warn(&phy->spi->dev, "[%s]: Reset might be needed...\n",
				 adrv9002_irqs[bit].irq_source);
			break;
		case ADI_ADRV9001_ACT_ERR_BBIC_LOG_ERROR:
			/* just log the irq */
			dev_dbg(&phy->spi->dev, "%s\n", adrv9002_irqs[bit].irq_source);
			break;
		default:
			/* no defined action. print out interrupt source */
			dev_warn(&phy->spi->dev, "%s\n", adrv9002_irqs[bit].irq_source);
			break;
		}
	}

irq_done:
	mutex_unlock(&phy->lock);
	return IRQ_HANDLED;
}

static int adrv9002_dgpio_config(const struct adrv9002_rf_phy *phy)
{
	struct adrv9002_gpio *dgpio = phy->adrv9002_gpios;
	int i, ret;

	for (i = 0; i < phy->ngpios; i++) {
		dev_dbg(&phy->spi->dev, "Set dpgio: %d, signal: %d\n",
			dgpio[i].gpio.pin, dgpio[i].signal);

		ret = api_call(phy, adi_adrv9001_gpio_Configure, dgpio[i].signal, &dgpio[i].gpio);
		if (ret)
			return ret;
	}

	return 0;
}

static int adrv9001_rx_path_config(struct adrv9002_rf_phy *phy,
				   const adi_adrv9001_ChannelState_e state)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(phy->rx_channels); i++) {
		struct adrv9002_rx_chan *rx = &phy->rx_channels[i];

		/* For each rx channel enabled */
		if (!rx->channel.enabled)
			continue;

		if (!rx->pin_cfg)
			goto agc_cfg;

		ret = api_call(phy, adi_adrv9001_Rx_GainControl_PinMode_Configure,
			       rx->channel.number, rx->pin_cfg);
		if (ret)
			return ret;

agc_cfg:
		ret = api_call(phy, adi_adrv9001_Rx_GainControl_Configure,
			       rx->channel.number, &rx->agc);
		if (ret)
			return ret;

		ret = api_call(phy, adi_adrv9001_Radio_Channel_ToState, ADI_RX,
			       rx->channel.number, state);
		if (ret)
			return ret;
	}

	return 0;
}

static int adrv9002_tx_set_dac_full_scale(const struct adrv9002_rf_phy *phy)
{
	int i, ret = 0;

	for (i = 0; i < ARRAY_SIZE(phy->tx_channels); i++) {
		const struct adrv9002_tx_chan *tx = &phy->tx_channels[i];

		if (!tx->channel.enabled || !tx->dac_boost_en)
			continue;

		ret = api_call(phy, adi_adrv9001_Tx_OutputPowerBoost_Set, tx->channel.number, true);
		if (ret)
			return ret;
	}

	return ret;
}

static int adrv9002_tx_path_config(const struct adrv9002_rf_phy *phy,
				   const adi_adrv9001_ChannelState_e state)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(phy->tx_channels); i++) {
		const struct adrv9002_tx_chan *tx = &phy->tx_channels[i];

		/* For each tx channel enabled */
		if (!tx->channel.enabled)
			continue;

		if (!tx->pin_cfg)
			goto rf_enable;

		ret = api_call(phy, adi_adrv9001_Tx_Attenuation_PinControl_Configure,
			       tx->channel.number, tx->pin_cfg);
		if (ret)
			return ret;

rf_enable:
		ret = api_call(phy, adi_adrv9001_Radio_Channel_ToState, ADI_TX,
			       tx->channel.number, state);
		if (ret)
			return ret;
	}

	return 0;
}

static const u32 adrv9002_init_cals_mask[16][2] = {
	/* Not a valid case. At least one channel should be enabled */
	[0] = {0, 0},
	/* tx2:0 rx2:0 tx1:0 rx1:1 */
	[1] = {0x1BE400, 0},
	/* tx2:0 rx2:0 tx1:1 rx1:0 */
	[2] = {0x1BE5F7, 0},
	/* tx2:0 rx2:0 tx1:1 rx1:1 */
	[3] = {0x1BE5F7, 0},
	/* tx2:0 rx2:1 tx1:0 rx1:0 */
	[4] = {0, 0x11E400},
	/* tx2:0 rx2:1 tx1:0 rx1:1 */
	[5] = {0x1BE400, 0x1BE400},
	/* tx2:0 rx2:1 tx1:1 rx1:0 */
	[6] = {0x1BE5F7, 0x1BE400},
	/* tx2:0 rx2:1 tx1:1 rx1:1 */
	[7] = {0x1BE5F7, 0x1BE400},
	/* tx2:1 rx2:0 tx1:0 rx1:0 */
	[8] = {0, 0x11E5F0},
	/* tx2:1 rx2:0 tx1:0 rx1:1 */
	[9] = {0x1BE400, 0x1BE5F0},
	/* tx2:1 rx2:0 tx1:1 rx1:0 */
	[10] = {0x1BE5F7, 0x1BE5F7},
	/* tx2:1 rx2:0 tx1:1 rx1:1 */
	[11] = {0x1BE5F7, 0x1BE5F7},
	/* tx2:1 rx2:1 tx1:0 rx1:0 */
	[12] = {0, 0x11E5F0},
	/* tx2:1 rx2:1 tx1:0 rx1:1 */
	[13] = {0x1BE400, 0x1BE5F0},
	/* tx2:1 rx2:1 tx1:1 rx1:0 */
	[14] = {0x1BE5F7, 0x1BE5F7},
	/* tx2:1 rx2:1 tx1:1 rx1:1 */
	[15] = {0x1BE5F7, 0x1BE5F7},
};

static void adrv9002_compute_init_cals(struct adrv9002_rf_phy *phy)
{
	int i, pos = 0;

	phy->init_cals.sysInitCalMask = 0;
	phy->init_cals.calMode = ADI_ADRV9001_INIT_CAL_MODE_ALL;

	for (i = 0; i < ARRAY_SIZE(phy->channels); i++) {
		struct adrv9002_chan *c = phy->channels[i];

		if (!c->enabled)
			continue;

		if (c->port == ADI_RX)
			pos |= ADRV9002_RX_EN(c->idx);
		else
			pos |= ADRV9002_TX_EN(c->idx);
	}

	phy->init_cals.chanInitCalMask[0] = adrv9002_init_cals_mask[pos][0];
	phy->init_cals.chanInitCalMask[1] = adrv9002_init_cals_mask[pos][1];

	dev_dbg(&phy->spi->dev, "pos: %u, Chan1:%X, Chan2:%X", pos,
		phy->init_cals.chanInitCalMask[0],
		phy->init_cals.chanInitCalMask[1]);
}

static int adrv9002_ext_lo_validate(struct adrv9002_rf_phy *phy, int idx, bool tx)
{
	adi_adrv9001_ClockSettings_t *clocks = &phy->curr_profile->clocks;
	adi_adrv9001_LoSel_e lo_selects[] = {
		clocks->rx1LoSelect, clocks->tx1LoSelect, clocks->rx2LoSelect, clocks->tx2LoSelect
	};
	adi_adrv9001_PllLoMode_e modes[] = { clocks->rfPll1LoMode, clocks->rfPll2LoMode };
	u16 dividers[] = { clocks->extLo1Divider, clocks->extLo2Divider };
	/* -1 since the enums start at 1 */
	unsigned int lo = lo_selects[idx * 2 + tx] - 1;
	struct device *dev = &phy->spi->dev;

	if (lo >= ARRAY_SIZE(modes)) {
		/*
		 * Anything other than ADI_ADRV9001_LOSEL_LO{1|2} should be wrong...
		 */
		dev_err(dev, "Unknown LO(%u) on %s%d\n", lo, tx ? "TX" : "RX", idx + 1);
		return -EINVAL;
	}

	if (modes[lo] == ADI_ADRV9001_INT_LO1)
		return ADRV9002_NO_EXT_LO;

	/*
	 * Alright, if external LO is being set on the profile for this port, we need to have
	 * a matching clk to control.
	 */
	if (!phy->ext_los[lo].clk) {
		dev_err(dev, "Ext LO%d set for %s%d but not controlling clk provided via dts\n",
			lo + 1, tx ? "TX" : "RX", idx + 1);
		return -EINVAL;
	}

	/* should never happen but we should also not blindly trust in the loaded profile... */
	if (!dividers[lo]) {
		dev_err(dev, "LO%d cannot have a divider of 0!\n", lo + 1);
		return -EINVAL;
	}

	phy->ext_los[lo].divider = dividers[lo];

	dev_dbg(dev, "EXT LO%d being used for %s%d with div(%u)\n", lo + 1, tx ? "TX" : "RX",
		idx + 1, dividers[lo]);

	return lo;
}

static int adrv9002_validate_profile(struct adrv9002_rf_phy *phy)
{
	const struct adi_adrv9001_RxChannelCfg *rx_cfg = phy->curr_profile->rx.rxChannelCfg;
	const struct adi_adrv9001_TxProfile *tx_cfg = phy->curr_profile->tx.txProfile;
	unsigned long rx_mask = phy->curr_profile->rx.rxInitChannelMask;
	unsigned long tx_mask = phy->curr_profile->tx.txInitChannelMask;
	const u32 ports[ADRV9002_CHANN_MAX * 2 + ADI_ADRV9001_MAX_ORX_ONLY] = {
		ADRV9002_RX1_BIT_NR, ADRV9002_TX1_BIT_NR, ADRV9002_RX2_BIT_NR,
		ADRV9002_TX2_BIT_NR, ADRV9002_ORX1_BIT_NR, ADRV9002_ORX2_BIT_NR,
	};
	int i, lo;

	for (i = 0; i < ADRV9002_CHANN_MAX; i++) {
		struct adrv9002_chan *tx = &phy->tx_channels[i].channel;
		struct adrv9002_rx_chan *rx = &phy->rx_channels[i];

		/* rx validations */
		if (!test_bit(ports[i * 2], &rx_mask))
			goto tx;

		lo = adrv9002_ext_lo_validate(phy, i, false);
		if (lo < 0)
			return lo;

		if (phy->rx2tx2 && i &&
		    rx_cfg[i].profile.rxOutputRate_Hz != phy->rx_channels[0].channel.rate) {
			dev_err(&phy->spi->dev, "In rx2tx2, RX%d rate=%u must be equal to RX1, rate=%ld\n",
				i + 1, rx_cfg[i].profile.rxOutputRate_Hz,
				phy->rx_channels[0].channel.rate);
			return -EINVAL;
		}

		if (phy->rx2tx2 && i && !phy->rx_channels[0].channel.enabled) {
			dev_err(&phy->spi->dev, "In rx2tx2, RX%d cannot be enabled while RX1 is disabled",
				i + 1);
			return -EINVAL;
		}

		if (phy->ssi_type != rx_cfg[i].profile.rxSsiConfig.ssiType) {
			dev_err(&phy->spi->dev, "SSI interface mismatch. PHY=%d, RX%d=%d\n",
				phy->ssi_type, i + 1, rx_cfg[i].profile.rxSsiConfig.ssiType);
			return -EINVAL;
		}

		if (rx_cfg[i].profile.rxSsiConfig.strobeType == ADI_ADRV9001_SSI_LONG_STROBE) {
			dev_err(&phy->spi->dev, "SSI interface Long Strobe not supported\n");
			return -EINVAL;
		}

		if (phy->ssi_type == ADI_ADRV9001_SSI_TYPE_LVDS &&
		    !rx_cfg[i].profile.rxSsiConfig.ddrEn) {
			dev_err(&phy->spi->dev, "RX%d: Single Data Rate port not supported for LVDS\n",
				i + 1);
			return -EINVAL;
		}

		dev_dbg(&phy->spi->dev, "RX%d enabled\n", i + 1);
		rx->channel.power = true;
		rx->channel.enabled = true;
		rx->channel.nco_freq = 0;
		rx->channel.rate = rx_cfg[i].profile.rxOutputRate_Hz;
		if (lo < ADI_ADRV9001_LOSEL_LO2)
			rx->channel.ext_lo = &phy->ext_los[lo];
tx:
		/* tx validations*/
		if (!test_bit(ports[i * 2 + 1], &tx_mask))
			continue;

		if (i >= phy->chip->n_tx) {
			dev_err(&phy->spi->dev, "TX%d not supported for this device\n", i + 1);
			return -EINVAL;
		}

		lo = adrv9002_ext_lo_validate(phy, i, true);
		if (lo < 0)
			return lo;

		/* check @tx_only comments in adrv9002.h to better understand the next checks */
		if (phy->ssi_type != tx_cfg[i].txSsiConfig.ssiType) {
			dev_err(&phy->spi->dev, "SSI interface mismatch. PHY=%d, TX%d=%d\n",
				phy->ssi_type, i + 1,  tx_cfg[i].txSsiConfig.ssiType);
			return -EINVAL;
		}

		if (tx_cfg[i].txSsiConfig.strobeType == ADI_ADRV9001_SSI_LONG_STROBE) {
			dev_err(&phy->spi->dev, "SSI interface Long Strobe not supported\n");
			return -EINVAL;
		}

		if (phy->ssi_type == ADI_ADRV9001_SSI_TYPE_LVDS &&
		    !tx_cfg[i].txSsiConfig.ddrEn) {
			dev_err(&phy->spi->dev, "TX%d: Single Data Rate port not supported for LVDS\n",
				i + 1);
			return -EINVAL;
		}

		if (phy->rx2tx2) {
			if (!phy->tx_only && !phy->rx_channels[0].channel.enabled) {
				/*
				 * pretty much means that in this case either all channels are
				 * disabled, which obviously does not make sense, or RX1 must
				 * be enabled...
				 */
				dev_err(&phy->spi->dev, "In rx2tx2, TX%d cannot be enabled while RX1 is disabled",
					i + 1);
				return -EINVAL;
			}

			if (i && !phy->tx_channels[0].channel.enabled) {
				dev_err(&phy->spi->dev, "In rx2tx2, TX%d cannot be enabled while TX1 is disabled",
					i + 1);
				return -EINVAL;
			}

			if (!phy->tx_only &&
			    tx_cfg[i].txInputRate_Hz != phy->rx_channels[0].channel.rate) {
				/*
				 * pretty much means that in this case, all ports must have
				 * the same rate. We match against RX1 since RX2 can be disabled
				 * even if it does not make much sense to disable it in rx2tx2 mode
				 */
				dev_err(&phy->spi->dev, "In rx2tx2, TX%d rate=%u must be equal to RX1, rate=%ld\n",
					i + 1, tx_cfg[i].txInputRate_Hz,
					phy->rx_channels[0].channel.rate);
				return -EINVAL;
			}

			if (phy->tx_only && i &&
			    tx_cfg[i].txInputRate_Hz != phy->tx_channels[0].channel.rate) {
				dev_err(&phy->spi->dev, "In rx2tx2, TX%d rate=%u must be equal to TX1, rate=%ld\n",
					i + 1, tx_cfg[i].txInputRate_Hz,
					phy->tx_channels[0].channel.rate);
				return -EINVAL;
			}
		} else if (!phy->tx_only && !rx->channel.enabled) {
			dev_err(&phy->spi->dev, "TX%d cannot be enabled while RX%d is disabled",
				i + 1, i + 1);
			return -EINVAL;
		} else if (!phy->tx_only && tx_cfg[i].txInputRate_Hz != rx->channel.rate) {
			dev_err(&phy->spi->dev, "TX%d rate=%u must be equal to RX%d, rate=%ld\n",
				i + 1, tx_cfg[i].txInputRate_Hz, i + 1, rx->channel.rate);
			return -EINVAL;
		}

		dev_dbg(&phy->spi->dev, "TX%d enabled\n", i + 1);
		/* orx actually depends on whether or not TX is enabled and not RX */
		rx->orx_en = test_bit(ports[i + ADRV9002_CHANN_MAX * 2], &rx_mask);
		tx->power = true;
		tx->enabled = true;
		tx->nco_freq = 0;
		tx->rate = tx_cfg[i].txInputRate_Hz;
		if (lo < ADI_ADRV9001_LOSEL_LO2)
			tx->ext_lo = &phy->ext_los[lo];
	}

	return 0;
}

static int adrv9002_power_mgmt_config(const struct adrv9002_rf_phy *phy)
{
	struct adi_adrv9001_PowerManagementSettings power_mgmt = {
		.ldoPowerSavingModes = {
			ADI_ADRV9001_LDO_POWER_SAVING_MODE_1, ADI_ADRV9001_LDO_POWER_SAVING_MODE_1,
			ADI_ADRV9001_LDO_POWER_SAVING_MODE_1, ADI_ADRV9001_LDO_POWER_SAVING_MODE_1,
			ADI_ADRV9001_LDO_POWER_SAVING_MODE_1, ADI_ADRV9001_LDO_POWER_SAVING_MODE_1,
			ADI_ADRV9001_LDO_POWER_SAVING_MODE_1, ADI_ADRV9001_LDO_POWER_SAVING_MODE_1,
			ADI_ADRV9001_LDO_POWER_SAVING_MODE_1, ADI_ADRV9001_LDO_POWER_SAVING_MODE_1,
			ADI_ADRV9001_LDO_POWER_SAVING_MODE_1, ADI_ADRV9001_LDO_POWER_SAVING_MODE_1,
			ADI_ADRV9001_LDO_POWER_SAVING_MODE_1, ADI_ADRV9001_LDO_POWER_SAVING_MODE_1,
			ADI_ADRV9001_LDO_POWER_SAVING_MODE_1, ADI_ADRV9001_LDO_POWER_SAVING_MODE_1,
			ADI_ADRV9001_LDO_POWER_SAVING_MODE_1, ADI_ADRV9001_LDO_POWER_SAVING_MODE_1,
			ADI_ADRV9001_LDO_POWER_SAVING_MODE_1
		}
	};

	return api_call(phy, adi_adrv9001_powermanagement_Configure, &power_mgmt);
}

static int adrv9002_digital_init(const struct adrv9002_rf_phy *phy)
{
	int spi_mode = ADI_ADRV9001_ARM_SINGLE_SPI_WRITE_MODE_STANDARD_BYTES_252;
	int ret;
	u8 tx_mask = 0;
	int c;
	adi_adrv9001_RxChannelCfg_t *rx_cfg = phy->curr_profile->rx.rxChannelCfg;

	ret = adi_adrv9001_arm_AhbSpiBridge_Enable(phy->adrv9001);
	if (ret)
		return ret;

	/*
	 * If we find a custom stream, we will load that. Otherwise we will load the default one.
	 * Note that if we are in the middle of filling @phy->stream_buf with a new stream and we
	 * get somehow here, the default one will be used. Either way, after filling the stream, we
	 * __must__ write a new profile which will get us here again and we can then load then new
	 * stream.
	 */
	if (phy->stream_size == ADI_ADRV9001_STREAM_BINARY_IMAGE_FILE_SIZE_BYTES)
		ret = api_call(phy, adi_adrv9001_Stream_Image_Write, 0,
			       phy->stream_buf, phy->stream_size, spi_mode);
	else
		ret = api_call(phy, adi_adrv9001_Utilities_StreamImage_Load,
			       "Navassa_Stream.bin", spi_mode);
	if (ret)
		return ret;

	/* program arm firmware */
	ret = api_call(phy, adi_adrv9001_Utilities_ArmImage_Load,
		       "Navassa_EvaluationFw.bin", spi_mode);
	if (ret)
		return ret;

	ret = api_call(phy, adi_adrv9001_arm_Profile_Write, phy->curr_profile);
	if (ret)
		return ret;

	ret = api_call(phy, adi_adrv9001_arm_PfirProfiles_Write, phy->curr_profile);
	if (ret)
		return ret;

	/* Load gain tables */
	for (c = 0; c < ADRV9002_CHANN_MAX; c++) {
		const struct adrv9002_rx_chan *rx = &phy->rx_channels[c];
		const struct adrv9002_tx_chan *tx = &phy->tx_channels[c];
		adi_adrv9001_RxProfile_t *p = &rx_cfg[c].profile;
		adi_adrv9001_RxGainTableType_e t_type;
		const char *rx_table;

		if (p->gainTableType) {
			rx_table = "RxGainTable_GainCompensated.csv";
			t_type = ADI_ADRV9001_RX_GAIN_COMPENSATION_TABLE;
		} else {
			rx_table = "RxGainTable.csv";
			t_type = ADI_ADRV9001_RX_GAIN_CORRECTION_TABLE;
		}

		if (rx->orx_en || tx->channel.enabled) {
			ret = api_call(phy, adi_adrv9001_Utilities_RxGainTable_Load, ADI_ORX,
				       "ORxGainTable.csv", rx->channel.number, &p->lnaConfig,
				       t_type);
			if (ret)
				return ret;
		}

		if (tx->channel.enabled)
			tx_mask |= tx->channel.number;

		if (!rx->channel.enabled)
			continue;

		ret = api_call(phy, adi_adrv9001_Utilities_RxGainTable_Load, ADI_RX, rx_table,
			       rx->channel.number, &p->lnaConfig, t_type);
		if (ret)
			return ret;
	}

	if (tx_mask) {
		ret = api_call(phy, adi_adrv9001_Utilities_TxAttenTable_Load,
			       "TxAttenTable.csv", tx_mask);
		if (ret)
			return ret;
	}

	ret = adrv9002_power_mgmt_config(phy);
	if (ret)
		return ret;

	ret = api_call(phy, adi_adrv9001_arm_Start);
	if (ret)
		return ret;

	return api_call(phy, adi_adrv9001_arm_StartStatus_Check, 5000000);
}

static u64 adrv9002_get_init_carrier(const struct adrv9002_chan *c)
{
	u64 lo_freq;

	if (!c->ext_lo) {
		/* If no external LO, keep the same values as before */
		if (c->port == ADI_RX)
			return 2400000000ULL;

		return 2450000000ULL;
	}

	lo_freq = clk_get_rate_scaled(c->ext_lo->clk, &c->ext_lo->scale);
	return DIV_ROUND_CLOSEST_ULL(lo_freq, c->ext_lo->divider);
}

/*
 * All of these structures are taken from TES when exporting the default profile to C code. Consider
 * about having all of these configurable through devicetree.
 */
static int adrv9002_radio_init(const struct adrv9002_rf_phy *phy)
{
	int ret;
	int chan;
	u8 channel_mask = (phy->curr_profile->tx.txInitChannelMask |
			   phy->curr_profile->rx.rxInitChannelMask) & 0xFF;
	struct adi_adrv9001_PllLoopFilterCfg pll_loop_filter = {
		.effectiveLoopBandwidth_kHz = 0,
		.loopBandwidth_kHz = 300,
		.phaseMargin_degrees = 60,
		.powerScale = 5
	};
	struct adi_adrv9001_Carrier carrier = {0};

	ret = api_call(phy, adi_adrv9001_Radio_PllLoopFilter_Set,
		       ADI_ADRV9001_PLL_LO1, &pll_loop_filter);
	if (ret)
		return ret;

	ret = api_call(phy, adi_adrv9001_Radio_PllLoopFilter_Set,
		       ADI_ADRV9001_PLL_LO2, &pll_loop_filter);
	if (ret)
		return ret;

	ret = api_call(phy, adi_adrv9001_Radio_PllLoopFilter_Set,
		       ADI_ADRV9001_PLL_AUX, &pll_loop_filter);
	if (ret)
		return ret;

	for (chan = 0; chan < ARRAY_SIZE(phy->channels); chan++) {
		const struct adrv9002_chan *c = phy->channels[chan];
		struct adi_adrv9001_ChannelEnablementDelays en_delays;

		if (!c->enabled)
			continue;

		/*
		 * For some low rate profiles, the intermediate frequency is non 0.
		 * In these cases, forcing it 0, will cause a firmware error. Hence, we need to
		 * read what we have and make sure we just change the carrier frequency...
		 */
		ret = api_call(phy, adi_adrv9001_Radio_Carrier_Inspect, c->port,
			       c->number, &carrier);
		if (ret)
			return ret;

		carrier.carrierFrequency_Hz = adrv9002_get_init_carrier(c);
		ret = api_call(phy, adi_adrv9001_Radio_Carrier_Configure,
			       c->port, c->number, &carrier);
		if (ret)
			return ret;

		adrv9002_en_delays_ns_to_arm(phy, &c->en_delays_ns, &en_delays);

		ret = api_call(phy, adi_adrv9001_Radio_ChannelEnablementDelays_Configure,
			       c->port, c->number, &en_delays);
		if (ret)
			return ret;
	}

	return api_call(phy, adi_adrv9001_arm_System_Program, channel_mask);
}

static struct adi_adrv9001_SpiSettings adrv9002_spi = {
	.msbFirst = 1,
	.enSpiStreaming = 0,
	.autoIncAddrUp = 1,
	.fourWireMode = 1,
	.cmosPadDrvStrength = ADI_ADRV9001_CMOSPAD_DRV_STRONG,
};

static int adrv9002_setup(struct adrv9002_rf_phy *phy)
{
	u8 init_cals_error = 0;
	int ret;
	adi_adrv9001_ChannelState_e init_state;

	/* in TDD we cannot start with all ports enabled as RX/TX cannot be on at the same time */
	if (phy->curr_profile->sysConfig.duplexMode == ADI_ADRV9001_TDD_MODE)
		init_state = ADI_ADRV9001_CHANNEL_PRIMED;
	else
		init_state = ADI_ADRV9001_CHANNEL_RF_ENABLED;

	adi_common_ErrorClear(&phy->adrv9001->common);
	ret = api_call(phy, adi_adrv9001_HwOpen, &adrv9002_spi);
	if (ret)
		return ret;

	ret = adrv9002_validate_profile(phy);
	if (ret)
		return ret;

	adrv9002_compute_init_cals(phy);

	adrv9002_log_enable(&phy->adrv9001->common);

	ret = api_call(phy, adi_adrv9001_InitAnalog, phy->curr_profile,
		       ADI_ADRV9001_DEVICECLOCKDIVISOR_2);
	if (ret)
		return ret;

	ret = adrv9002_digital_init(phy);
	if (ret)
		return ret;

	ret = adrv9002_radio_init(phy);
	if (ret)
		return ret;

	/* should be done before init calibrations */
	ret = adrv9002_tx_set_dac_full_scale(phy);
	if (ret)
		return ret;

	if (phy->curr_profile->sysConfig.fhModeOn) {
		ret = api_call(phy, adi_adrv9001_fh_Configure, &phy->fh);
		if (ret)
			return ret;
	}

	ret = api_call(phy, adi_adrv9001_cals_InitCals_Run, &phy->init_cals,
		       60000, &init_cals_error);
	if (ret)
		return ret;

	ret = adrv9001_rx_path_config(phy, init_state);
	if (ret)
		return ret;

	ret = adrv9002_tx_path_config(phy, init_state);
	if (ret)
		return ret;

	/* unmask IRQs */
	ret = api_call(phy, adi_adrv9001_gpio_GpIntMask_Set, ~ADRV9002_IRQ_MASK);
	if (ret)
		return ret;

	return adrv9002_dgpio_config(phy);
}

int adrv9002_intf_change_delay(const struct adrv9002_rf_phy *phy, const int channel, u8 clk_delay,
			       u8 data_delay, const bool tx)
{
	struct adi_adrv9001_SsiCalibrationCfg delays = {0};

	dev_dbg(&phy->spi->dev, "Set intf delay clk:%u, d:%u, tx:%d c:%d\n", clk_delay,
		data_delay, tx, channel);

	if (tx) {
		delays.txClkDelay[channel] = clk_delay;
		delays.txIDataDelay[channel] = data_delay;
		delays.txQDataDelay[channel] = data_delay;
		delays.txStrobeDelay[channel] = data_delay;
		if (phy->rx2tx2) {
			delays.txClkDelay[channel + 1] = clk_delay;
			delays.txIDataDelay[channel + 1] = data_delay;
			delays.txQDataDelay[channel + 1] = data_delay;
			delays.txStrobeDelay[channel + 1] = data_delay;
		}
	} else {
		delays.rxClkDelay[channel] = clk_delay;
		delays.rxIDataDelay[channel] = data_delay;
		delays.rxQDataDelay[channel] = data_delay;
		delays.rxStrobeDelay[channel] = data_delay;
		if (phy->rx2tx2) {
			delays.rxClkDelay[channel + 1] = clk_delay;
			delays.rxIDataDelay[channel + 1] = data_delay;
			delays.rxQDataDelay[channel + 1] = data_delay;
			delays.rxStrobeDelay[channel + 1] = data_delay;
		}
	}

	return api_call(phy, adi_adrv9001_Ssi_Delay_Configure, phy->ssi_type, &delays);
}

int adrv9002_check_tx_test_pattern(const struct adrv9002_rf_phy *phy, const int chann)
{
	int ret;
	const struct adrv9002_chan *chan = &phy->tx_channels[chann].channel;
	adi_adrv9001_SsiTestModeData_e test_data = phy->ssi_type == ADI_ADRV9001_SSI_TYPE_CMOS ?
						ADI_ADRV9001_SSI_TESTMODE_DATA_RAMP_NIBBLE :
						ADI_ADRV9001_SSI_TESTMODE_DATA_PRBS7;
	struct adi_adrv9001_TxSsiTestModeCfg cfg = {0};
	struct adi_adrv9001_TxSsiTestModeStatus status = {0};
	adi_adrv9001_SsiDataFormat_e data_fmt = ADI_ADRV9001_SSI_FORMAT_16_BIT_I_Q_DATA;

	cfg.testData = test_data;

	ret = api_call(phy, adi_adrv9001_Ssi_Tx_TestMode_Status_Inspect,
		       chan->number, phy->ssi_type, data_fmt, &cfg, &status);
	if (ret)
		return ret;

	dev_dbg(&phy->spi->dev, "[c%d]: d_e:%u, f_f:%u f_e:%u, s_e:%u", chan->number,
		status.dataError, status.fifoFull, status.fifoEmpty, status.strobeAlignError);

	/* only looking for data errors for now */
	if (status.dataError)
		return 1;

	if (!phy->rx2tx2)
		return 0;

	/* on rx2tx2 we will only get here on index 0 so the following is fine */
	chan = &phy->tx_channels[chann + 1].channel;
	if (!chan->enabled)
		return 0;

	memset(&status, 0, sizeof(status));
	ret = api_call(phy, adi_adrv9001_Ssi_Tx_TestMode_Status_Inspect,
		       chan->number, phy->ssi_type, data_fmt, &cfg, &status);
	if (ret)
		return ret;

	dev_dbg(&phy->spi->dev, "[c%d]: d_e:%u, f_f:%u f_e:%u, s_e:%u", chan->number,
		status.dataError, status.fifoFull, status.fifoEmpty, status.strobeAlignError);

	if (status.dataError)
		return 1;

	return 0;
}

int adrv9002_intf_test_cfg(const struct adrv9002_rf_phy *phy, const int chann, const bool tx,
			   const bool stop)
{
	int ret;
	const struct adrv9002_chan *chan;
	adi_adrv9001_SsiDataFormat_e data_fmt = ADI_ADRV9001_SSI_FORMAT_16_BIT_I_Q_DATA;

	dev_dbg(&phy->spi->dev, "cfg test stop:%u, ssi:%d, c:%d, tx:%d\n", stop,
		phy->ssi_type, chann, tx);

	if (tx) {
		struct adi_adrv9001_TxSsiTestModeCfg cfg = {0};

		chan = &phy->tx_channels[chann].channel;

		if (stop)
			cfg.testData = ADI_ADRV9001_SSI_TESTMODE_DATA_NORMAL;
		else if (phy->ssi_type == ADI_ADRV9001_SSI_TYPE_LVDS)
			/*
			 * Some low rate profiles don't play well with prbs15. The reason is
			 * still unclear. We suspect that the chip error checker might have
			 * some time constrains and cannot reliable validate prbs15 full
			 * sequences in the test time. Using a shorter sequence fixes the
			 * problem...
			 */
			cfg.testData = ADI_ADRV9001_SSI_TESTMODE_DATA_PRBS7;
		else
			/* CMOS */
			cfg.testData = ADI_ADRV9001_SSI_TESTMODE_DATA_RAMP_NIBBLE;

		ret = api_call(phy, adi_adrv9001_Ssi_Tx_TestMode_Configure,
			       chan->number, phy->ssi_type, data_fmt, &cfg);
		if (ret)
			return ret;

		if (!phy->rx2tx2)
			return 0;

		/* on rx2tx2 we will only get here on index 0 so the following is fine */
		chan = &phy->tx_channels[chann + 1].channel;
		if (!chan->enabled)
			return 0;

		ret = api_call(phy, adi_adrv9001_Ssi_Tx_TestMode_Configure,
			       chan->number, phy->ssi_type, data_fmt, &cfg);
		if (ret)
			return ret;

	} else {
		struct adi_adrv9001_RxSsiTestModeCfg cfg = {0};

		chan = &phy->rx_channels[chann].channel;

		if (stop)
			cfg.testData = ADI_ADRV9001_SSI_TESTMODE_DATA_NORMAL;
		else if (phy->ssi_type == ADI_ADRV9001_SSI_TYPE_LVDS)
			cfg.testData = ADI_ADRV9001_SSI_TESTMODE_DATA_PRBS15;
		else
			/* CMOS */
			cfg.testData = ADI_ADRV9001_SSI_TESTMODE_DATA_RAMP_NIBBLE;

		ret = api_call(phy, adi_adrv9001_Ssi_Rx_TestMode_Configure,
			       chan->number, phy->ssi_type, data_fmt, &cfg);
		if (ret)
			return ret;

		if (!phy->rx2tx2)
			return 0;

		/* on rx2tx2 we will only get here on index 0 so the following is fine */
		chan = &phy->rx_channels[chann + 1].channel;
		if (!chan->enabled)
			return 0;

		ret = api_call(phy, adi_adrv9001_Ssi_Rx_TestMode_Configure,
			       chan->number, phy->ssi_type, data_fmt, &cfg);
		if (ret)
			return ret;
	}

	return 0;
}

static int adrv9002_intf_tuning(const struct adrv9002_rf_phy *phy)
{
	struct adi_adrv9001_SsiCalibrationCfg delays = {0};
	int ret;
	u8 clk_delay, data_delay;
	int i;

	for (i = 0; i < ARRAY_SIZE(phy->channels); i++) {
		struct adrv9002_chan *c = phy->channels[i];

		if (!c->enabled)
			continue;
		if (phy->rx2tx2 && c->idx) {
			/*
			 * In rx2tx2 we should treat both channels as the same. Hence, we will run
			 * the test simultaneosly for both and configure the same delays.
			 */
			if (c->port == ADI_RX) {
				/* RX0 must be enabled, hence we can safely skip further tuning */
				delays.rxClkDelay[c->idx] = delays.rxClkDelay[0];
				delays.rxIDataDelay[c->idx] = delays.rxIDataDelay[0];
				delays.rxQDataDelay[c->idx] = delays.rxQDataDelay[0];
				delays.rxStrobeDelay[c->idx] = delays.rxStrobeDelay[0];
			} else {
				/* TX0 must be enabled, hence we can safely skip further tuning */
				delays.txClkDelay[c->idx] = delays.txClkDelay[0];
				delays.txIDataDelay[c->idx] = delays.txIDataDelay[0];
				delays.txQDataDelay[c->idx] = delays.txQDataDelay[0];
				delays.txStrobeDelay[c->idx] = delays.txStrobeDelay[0];
			}

			continue;
		}

		ret = adrv9002_axi_intf_tune(phy, c->port == ADI_TX, c->idx, &clk_delay,
					     &data_delay);
		if (ret)
			return ret;

		if (c->port == ADI_RX) {
			dev_dbg(&phy->spi->dev, "RX: Got clk: %u, data: %u\n", clk_delay,
				data_delay);
			delays.rxClkDelay[c->idx] = clk_delay;
			delays.rxIDataDelay[c->idx] = data_delay;
			delays.rxQDataDelay[c->idx] = data_delay;
			delays.rxStrobeDelay[c->idx] = data_delay;
		} else {
			dev_dbg(&phy->spi->dev, "TX: Got clk: %u, data: %u\n", clk_delay,
				data_delay);
			delays.txClkDelay[c->idx] = clk_delay;
			delays.txIDataDelay[c->idx] = data_delay;
			delays.txQDataDelay[c->idx] = data_delay;
			delays.txStrobeDelay[c->idx] = data_delay;
		}
	}

	return api_call(phy, adi_adrv9001_Ssi_Delay_Configure, phy->ssi_type, &delays);

}

static void adrv9002_cleanup(struct adrv9002_rf_phy *phy)
{
	int i;

	for (i = 0; i < ADRV9002_CHANN_MAX; i++) {
		phy->rx_channels[i].orx_en = 0;
		/* make sure we have the ORx GPIO low */
		if (phy->rx_channels[i].orx_gpio)
			gpiod_set_value_cansleep(phy->rx_channels[i].orx_gpio, 0);
		phy->rx_channels[i].channel.enabled = 0;
		phy->rx_channels[i].channel.ext_lo = NULL;
		phy->tx_channels[i].channel.enabled = 0;
		phy->tx_channels[i].channel.ext_lo = NULL;
	}

	phy->profile_len = scnprintf(phy->profile_buf, sizeof(phy->profile_buf),
				     "No profile loaded...\n");
	memset(&phy->adrv9001->devStateInfo, 0,
	       sizeof(phy->adrv9001->devStateInfo));
}

static u32 adrv9002_get_arm_clk(const struct adrv9002_rf_phy *phy)
{
	struct adi_adrv9001_ClockSettings *clks = &phy->curr_profile->clocks;
	u32 sys_clk;

	/* HP clk PLL is 8.8GHz and LP is 4.4GHz */
	if (clks->clkPllVcoFreq_daHz == ADRV9002_HP_CLK_PLL_DAHZ)
		sys_clk = clks->clkPllVcoFreq_daHz / 48 * 10;
	else
		sys_clk = clks->clkPllVcoFreq_daHz / 24 * 10;

	return DIV_ROUND_CLOSEST(sys_clk, clks->armPowerSavingClkDiv);
}

void adrv9002_en_delays_ns_to_arm(const struct adrv9002_rf_phy *phy,
				  const struct adi_adrv9001_ChannelEnablementDelays *d_ns,
				  struct adi_adrv9001_ChannelEnablementDelays *d)
{
	u32 arm_clk = adrv9002_get_arm_clk(phy);

	d->fallToOffDelay = DIV_ROUND_CLOSEST_ULL((u64)arm_clk * d_ns->fallToOffDelay, 1000000000);
	d->guardDelay = DIV_ROUND_CLOSEST_ULL((u64)arm_clk * d_ns->guardDelay, 1000000000);
	d->holdDelay = DIV_ROUND_CLOSEST_ULL((u64)arm_clk * d_ns->holdDelay, 1000000000);
	d->riseToAnalogOnDelay = DIV_ROUND_CLOSEST_ULL((u64)arm_clk * d_ns->riseToAnalogOnDelay,
						       1000000000);
	d->riseToOnDelay = DIV_ROUND_CLOSEST_ULL((u64)arm_clk * d_ns->riseToOnDelay, 1000000000);
}

void adrv9002_en_delays_arm_to_ns(const struct adrv9002_rf_phy *phy,
				  const struct adi_adrv9001_ChannelEnablementDelays *d,
				  struct adi_adrv9001_ChannelEnablementDelays *d_ns)
{
	u32 arm_clk = adrv9002_get_arm_clk(phy);

	d_ns->fallToOffDelay = DIV_ROUND_CLOSEST_ULL(d->fallToOffDelay * 1000000000ULL, arm_clk);
	d_ns->guardDelay = DIV_ROUND_CLOSEST_ULL(d->guardDelay * 1000000000ULL, arm_clk);
	d_ns->holdDelay = DIV_ROUND_CLOSEST_ULL(d->holdDelay * 1000000000ULL, arm_clk);
	d_ns->riseToAnalogOnDelay = DIV_ROUND_CLOSEST_ULL(d->riseToAnalogOnDelay * 1000000000ULL,
							  arm_clk);
	d_ns->riseToOnDelay = DIV_ROUND_CLOSEST_ULL(d->riseToOnDelay * 1000000000ULL, arm_clk);
}

static const char *const lo_maps[] = {
	"Unknown",
	"L01",
	"L02",
	"AUX LO"
};

static const char *const duplex[] = {
	"TDD",
	"FDD"
};

static const char *const ssi[] = {
	"Disabled",
	"CMOS",
	"LVDS"
};

static const char *const mcs[] = {
	"Disabled",
	"Enabled",
	"Enabled RFPLL Phase"
};

static const char *const rx_gain_type[] = {
	"Correction",
	"Compensated"
};

static void adrv9002_fill_profile_read(struct adrv9002_rf_phy *phy)
{
	struct adi_adrv9001_DeviceSysConfig *sys = &phy->curr_profile->sysConfig;
	struct adi_adrv9001_ClockSettings *clks = &phy->curr_profile->clocks;
	struct adi_adrv9001_RxSettings *rx = &phy->curr_profile->rx;
	struct adi_adrv9001_RxChannelCfg *rx_cfg = rx->rxChannelCfg;
	struct adi_adrv9001_TxSettings *tx = &phy->curr_profile->tx;

	phy->profile_len = scnprintf(phy->profile_buf, sizeof(phy->profile_buf),
				     "Device clk(Hz): %d\n"
				     "Clk PLL VCO(Hz): %lld\n"
				     "ARM Power Saving Clk Divider: %d\n"
				     "RX1 LO: %s\n"
				     "RX2 LO: %s\n"
				     "TX1 LO: %s\n"
				     "TX2 LO: %s\n"
				     "RX1 Gain Table Type: %s\n"
				     "RX2 Gain Table Type: %s\n"
				     "RX Channel Mask: 0x%x\n"
				     "TX Channel Mask: 0x%x\n"
				     "Duplex Mode: %s\n"
				     "FH enable: %d\n"
				     "MCS mode: %s\n"
				     "SSI interface: %s\n", clks->deviceClock_kHz * 1000,
				     clks->clkPllVcoFreq_daHz * 10ULL, clks->armPowerSavingClkDiv,
				     lo_maps[clks->rx1LoSelect], lo_maps[clks->rx2LoSelect],
				     lo_maps[clks->tx1LoSelect], lo_maps[clks->tx2LoSelect],
				     rx_gain_type[rx_cfg[ADRV9002_CHANN_1].profile.gainTableType],
				     rx_gain_type[rx_cfg[ADRV9002_CHANN_2].profile.gainTableType],
				     rx->rxInitChannelMask, tx->txInitChannelMask,
				     duplex[sys->duplexMode], sys->fhModeOn, mcs[sys->mcsMode],
				     ssi[phy->ssi_type]);
}

int adrv9002_init(struct adrv9002_rf_phy *phy, struct adi_adrv9001_Init *profile)
{
	int ret, c;
	struct adrv9002_chan *chan;

	adrv9002_cleanup(phy);
	/*
	 * Disable all the cores as it might interfere with init calibrations.
	 */
	for (c = 0; c < ARRAY_SIZE(phy->channels); c++) {
		chan = phy->channels[c];

		if (phy->rx2tx2 && chan->idx > ADRV9002_CHANN_1)
			break;
		/* nothing else to do if there's no TX2 */
		if (chan->port == ADI_TX && chan->idx >= phy->chip->n_tx)
			break;

		adrv9002_axi_interface_enable(phy, chan->idx, chan->port == ADI_TX, false);
	}

	phy->curr_profile = profile;
	ret = adrv9002_setup(phy);
	if (ret) {
		/* try one more time */
		ret = adrv9002_setup(phy);
		if (ret)
			goto error;
	}

	adrv9002_set_clk_rates(phy);

	ret = adrv9002_ssi_configure(phy);
	if (ret)
		goto error;

	/* re-enable the cores */
	for (c = 0; c < ARRAY_SIZE(phy->channels); c++) {
		chan = phy->channels[c];

		if (phy->rx2tx2 && chan->idx > ADRV9002_CHANN_1)
			break;

		if (!chan->enabled)
			continue;

		adrv9002_axi_interface_enable(phy, chan->idx, chan->port == ADI_TX, true);
	}

	ret = adrv9002_intf_tuning(phy);
	if (ret) {
		dev_err(&phy->spi->dev, "Interface tuning failed: %d\n", ret);
		goto error;
	}

	adrv9002_fill_profile_read(phy);

	return 0;
error:
	/*
	 * Leave the device in a reset state in case of error. There's not much we can do if
	 * the API call fails, so we are just being verbose about it...
	 */
	api_call(phy, adi_adrv9001_HwOpen, &adrv9002_spi);
	adrv9002_cleanup(phy);

	return ret;
}

static ssize_t adrv9002_stream_bin_write(struct file *filp, struct kobject *kobj,
					 struct bin_attribute *bin_attr, char *buf, loff_t off,
					 size_t count)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(kobj_to_dev(kobj));
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);

	mutex_lock(&phy->lock);
	if (!off)
		phy->stream_size = 0;
	memcpy(phy->stream_buf + off, buf, count);
	phy->stream_size += count;
	mutex_unlock(&phy->lock);

	return count;
}

static ssize_t adrv9002_profile_bin_write(struct file *filp, struct kobject *kobj,
					  struct bin_attribute *bin_attr, char *buf, loff_t off,
					  size_t count)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(kobj_to_dev(kobj));
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	int ret;

	if (off == 0)
		memset(phy->bin_attr_buf, 0, bin_attr->size);

	memcpy(phy->bin_attr_buf + off, buf, count);

	if (!strnstr(phy->bin_attr_buf, "\n}", off + count))
		return count;

	dev_dbg(&phy->spi->dev, "%s:%d: size %lld\n", __func__, __LINE__,
		off + count);

	mutex_lock(&phy->lock);

	memset(&phy->profile, 0, sizeof(phy->profile));
	ret = api_call(phy, adi_adrv9001_profileutil_Parse, &phy->profile,
		       phy->bin_attr_buf, off + count);
	if (ret)
		goto out;

	ret = adrv9002_init(phy, &phy->profile);
out:
	mutex_unlock(&phy->lock);

	return (ret < 0) ? ret : count;
}

static ssize_t adrv9002_profile_bin_read(struct file *filp, struct kobject *kobj,
					 struct bin_attribute *bin_attr, char *buf, loff_t pos,
					 size_t count)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(kobj_to_dev(kobj));
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);
	ssize_t len;

	mutex_lock(&phy->lock);
	len = memory_read_from_buffer(buf, count, &pos, phy->profile_buf, phy->profile_len);
	mutex_unlock(&phy->lock);

	return len;
}

static ssize_t adrv9002_fh_bin_table_write(struct adrv9002_rf_phy *phy, char *buf, loff_t off,
					   size_t count, int hop, int table)
{
	struct adrv9002_fh_bin_table *tbl = &phy->fh_table_bin_attr[hop * 2 + table];
	/* this is only static to avoid  -Wframe-larger-than on ARM */
	static adi_adrv9001_FhHopFrame_t hop_tbl[ADI_ADRV9001_FH_MAX_HOP_TABLE_SIZE];
	char *p, *line;
	int entry = 0, ret, max_sz = ARRAY_SIZE(hop_tbl);

	mutex_lock(&phy->lock);
	if (!phy->curr_profile->sysConfig.fhModeOn) {
		dev_err(&phy->spi->dev, "Frequency hopping not enabled\n");
		mutex_unlock(&phy->lock);
		return -ENOTSUPP;
	}

	if (hop && phy->fh.mode != ADI_ADRV9001_FHMODE_LO_RETUNE_REALTIME_PROCESS_DUAL_HOP) {
		dev_err(&phy->spi->dev, "HOP2 not supported! FH mode not in dual hop.\n");
		mutex_unlock(&phy->lock);
		return -ENOTSUPP;
	}

	if (!off)
		memset(tbl->bin_table, 0, sizeof(tbl->bin_table));

	memcpy(tbl->bin_table + off, buf, count);

	if (phy->fh.mode == ADI_ADRV9001_FHMODE_LO_RETUNE_REALTIME_PROCESS_DUAL_HOP)
		max_sz /= 2;

	p = tbl->bin_table;
	while ((line = strsep(&p, "\n")) && p) {
		u64 lo;
		u32 rx10_if, rx20_if, rx1_gain, tx1_atten, rx2_gain, tx2_atten;

		 /* skip comment lines or blank lines */
		if (line[0] == '#' || !line[0])
			continue;
		if (strstr(line, "table>"))
			continue;

		ret = sscanf(line, "%llu,%u,%u,%u,%u,%u,%u", &lo, &rx10_if, &rx20_if, &rx1_gain,
			     &tx1_atten, &rx2_gain, &tx2_atten);
		if (ret != ADRV9002_FH_TABLE_COL_SZ) {
			dev_err(&phy->spi->dev, "Failed to parse hop:%d table:%d line:%s\n",
				hop, table, line);
			mutex_unlock(&phy->lock);
			return -EINVAL;
		}

		if (entry > max_sz) {
			dev_err(&phy->spi->dev, "Hop:%d table:%d too big:%d\n", hop, table, entry);
			mutex_unlock(&phy->lock);
			return -EINVAL;
		}

		if (lo < ADI_ADRV9001_FH_MIN_CARRIER_FREQUENCY_HZ ||
		    lo > ADI_ADRV9001_FH_MAX_CARRIER_FREQUENCY_HZ) {
			dev_err(&phy->spi->dev, "Invalid value for lo:%llu, in table entry:%d\n",
				lo, entry);
			mutex_unlock(&phy->lock);
			return -EINVAL;
		}

		hop_tbl[entry].hopFrequencyHz = lo;
		hop_tbl[entry].rx1OffsetFrequencyHz = rx10_if;
		hop_tbl[entry].rx2OffsetFrequencyHz = rx10_if;
		hop_tbl[entry].rx1GainIndex = rx1_gain;
		hop_tbl[entry].tx1Attenuation_fifthdB = tx1_atten;
		hop_tbl[entry].rx2GainIndex = rx1_gain;
		hop_tbl[entry].tx2Attenuation_fifthdB = tx2_atten;
		entry++;
	}

	dev_dbg(&phy->spi->dev, "Load hop:%d table:%d with %d entries\n", hop, table, entry);
	ret = api_call(phy, adi_adrv9001_fh_HopTable_Static_Configure,
		       phy->fh.mode, hop, table, hop_tbl, entry);
	mutex_unlock(&phy->lock);

	return ret ? ret : count;
}

static int adrv9002_profile_load(struct adrv9002_rf_phy *phy)
{
	int ret;
	const struct firmware *fw;
	const char *profile;
	void *buf;

	if (phy->ssi_type == ADI_ADRV9001_SSI_TYPE_CMOS)
		profile = phy->chip->cmos_profile;
	else
		profile = phy->chip->lvd_profile;

	ret = request_firmware(&fw, profile, &phy->spi->dev);
	if (ret)
		return ret;

	buf = kzalloc(fw->size, GFP_KERNEL);
	if (!buf) {
		release_firmware(fw);
		return -ENOMEM;
	}

	memcpy(buf, fw->data, fw->size);
	ret = api_call(phy, adi_adrv9001_profileutil_Parse, &phy->profile, buf, fw->size);
	release_firmware(fw);
	kfree(buf);

	return ret;
}

static void adrv9002_clk_disable(void *data)
{
	struct clk *clk = data;

	clk_disable_unprepare(clk);
}

static void adrv9002_of_clk_del_provider(void *data)
{
	struct device *dev = data;

	of_clk_del_provider(dev->of_node);
}

#define ADRV9002_HOP_TABLE_BIN_ATTR(h, t, hop, table) \
static ssize_t adrv9002_fh_hop##h##_bin_table_##t##_write(struct file *filp,			\
							  struct kobject *kobj,			\
							  struct bin_attribute *bin_attr,	\
							  char *buf, loff_t off, size_t count)	\
{												\
	struct iio_dev *indio_dev = dev_to_iio_dev(kobj_to_dev(kobj));				\
	struct adrv9002_rf_phy *phy = iio_priv(indio_dev);					\
												\
	return adrv9002_fh_bin_table_write(phy, buf, off, count, hop, table);			\
}												\
static BIN_ATTR(frequency_hopping_hop##h##_table_##t, 0222, NULL,				\
		adrv9002_fh_hop##h##_bin_table_##t##_write, PAGE_SIZE)				\

ADRV9002_HOP_TABLE_BIN_ATTR(1, a, ADI_ADRV9001_FH_HOP_SIGNAL_1, ADI_ADRV9001_FHHOPTABLE_A);
ADRV9002_HOP_TABLE_BIN_ATTR(1, b, ADI_ADRV9001_FH_HOP_SIGNAL_1, ADI_ADRV9001_FHHOPTABLE_B);
ADRV9002_HOP_TABLE_BIN_ATTR(2, a, ADI_ADRV9001_FH_HOP_SIGNAL_2, ADI_ADRV9001_FHHOPTABLE_A);
ADRV9002_HOP_TABLE_BIN_ATTR(2, b, ADI_ADRV9001_FH_HOP_SIGNAL_2, ADI_ADRV9001_FHHOPTABLE_B);
static BIN_ATTR(stream_config, 0222, NULL, adrv9002_stream_bin_write, ADRV9002_STREAM_BINARY_SZ);
static BIN_ATTR(profile_config, 0644, adrv9002_profile_bin_read, adrv9002_profile_bin_write,
		ADRV9002_PROFILE_MAX_SZ);

int adrv9002_post_init(struct adrv9002_rf_phy *phy)
{
	struct adi_common_ApiVersion api_version;
	struct adi_adrv9001_ArmVersion arm_version;
	struct adi_adrv9001_SiliconVersion silicon_version;
	struct adi_adrv9001_StreamVersion stream_version;
	int ret, c;
	struct spi_device *spi = phy->spi;
	struct iio_dev *indio_dev = phy->indio_dev;
	const char * const clk_names[NUM_ADRV9002_CLKS] = {
		[RX1_SAMPL_CLK] = "-rx1_sampl_clk",
		[RX2_SAMPL_CLK] = "-rx2_sampl_clk",
		[TX1_SAMPL_CLK] = "-tx1_sampl_clk",
		[TX2_SAMPL_CLK] = "-tx2_sampl_clk",
		[TDD1_INTF_CLK] = "-tdd1_intf_clk",
		[TDD2_INTF_CLK] = "-tdd2_intf_clk"
	};
	const struct bin_attribute *hop_attrs[] = {
		&bin_attr_frequency_hopping_hop1_table_a,
		&bin_attr_frequency_hopping_hop1_table_b,
		&bin_attr_frequency_hopping_hop2_table_a,
		&bin_attr_frequency_hopping_hop2_table_b
	};

	/* register channels clocks */
	for (c = 0; c < ADRV9002_CHANN_MAX; c++) {
		struct adrv9002_rx_chan *rx = &phy->rx_channels[c];
		struct adrv9002_chan *tx = &phy->tx_channels[c].channel;

		rx->channel.clk = adrv9002_clk_register(phy, clk_names[c],
							CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED,
							c);
		if (IS_ERR(rx->channel.clk))
			return PTR_ERR(rx->channel.clk);

		/*
		 * We do not support more TXs so break now. Note that in rx2tx2 mode, we
		 * set the clk pointer of a non existing channel but since it won't ever
		 * be used it's not a problem so let's keep the code simple in here.
		 */
		if (c >= phy->chip->n_tx)
			break;

		tx->clk = adrv9002_clk_register(phy, clk_names[c + TX1_SAMPL_CLK],
						CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED,
						c + TX1_SAMPL_CLK);
		if (IS_ERR(tx->clk))
			return PTR_ERR(tx->clk);

		rx->tdd_clk = adrv9002_clk_register(phy, clk_names[c + TDD1_INTF_CLK],
						    CLK_GET_RATE_NOCACHE | CLK_IGNORE_UNUSED,
						    c + TDD1_INTF_CLK);
		if (IS_ERR(rx->tdd_clk))
			return PTR_ERR(rx->tdd_clk);

		if (phy->rx2tx2) {
			/* just point RX2/TX2 to RX1/TX1*/
			phy->rx_channels[c + 1].channel.clk = rx->channel.clk;
			phy->tx_channels[c + 1].channel.clk = tx->clk;
			break;
		}
	}

	ret = adrv9002_profile_load(phy);
	if (ret)
		return ret;

	ret = adrv9002_init(phy, &phy->profile);
	if (ret)
		return ret;

	phy->clk_data.clks = phy->clks;
	phy->clk_data.clk_num = phy->n_clks;
	ret = of_clk_add_provider(spi->dev.of_node, of_clk_src_onecell_get,
				  &phy->clk_data);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&spi->dev, adrv9002_of_clk_del_provider,
				       &spi->dev);
	if (ret)
		return ret;

	indio_dev->dev.parent = &spi->dev;
	indio_dev->name = phy->chip->name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &adrv9002_phy_info;
	indio_dev->channels = phy->chip->channels;
	indio_dev->num_channels = phy->chip->num_channels;

	if (spi->irq) {
		const unsigned long mask = IRQF_TRIGGER_RISING | IRQF_ONESHOT;

		ret = devm_request_threaded_irq(&spi->dev, spi->irq, NULL,
						adrv9002_irq_handler, mask,
						indio_dev->name, indio_dev);
		if (ret)
			return ret;
	}

	ret = devm_iio_device_register(&spi->dev, indio_dev);
	if (ret < 0)
		return ret;

	phy->bin_attr_buf = devm_kzalloc(&phy->spi->dev, ADRV9002_PROFILE_MAX_SZ, GFP_KERNEL);
	if (!phy->bin_attr_buf)
		return -ENOMEM;

	ret = device_create_bin_file(&indio_dev->dev, &bin_attr_profile_config);
	if (ret < 0)
		return ret;

	phy->stream_buf = devm_kzalloc(&phy->spi->dev, ADRV9002_STREAM_BINARY_SZ, GFP_KERNEL);
	if (!phy->stream_buf)
		return -ENOMEM;

	ret = device_create_bin_file(&indio_dev->dev, &bin_attr_stream_config);
	if (ret < 0)
		return ret;

	for (c = 0; c < ARRAY_SIZE(hop_attrs); c++) {
		ret = device_create_bin_file(&indio_dev->dev, hop_attrs[c]);
		if (ret < 0)
			return ret;
	}

	api_call(phy, adi_adrv9001_ApiVersion_Get, &api_version);
	api_call(phy, adi_adrv9001_arm_Version, &arm_version);
	api_call(phy, adi_adrv9001_SiliconVersion_Get, &silicon_version);
	api_call(phy, adi_adrv9001_Stream_Version, &stream_version);

	dev_info(&spi->dev,
		 "%s Rev %d.%d, Firmware %u.%u.%u.%u,  Stream %u.%u.%u.%u,  API version: %u.%u.%u successfully initialized",
		 indio_dev->name, silicon_version.major, silicon_version.minor,
		 arm_version.majorVer, arm_version.minorVer, arm_version.maintVer,
		 arm_version.rcVer, stream_version.majorVer, stream_version.minorVer,
		 stream_version.maintVer, stream_version.buildVer, api_version.major,
		 api_version.minor, api_version.patch);

	adrv9002_debugfs_create(phy, iio_get_debugfs_dentry(indio_dev));

	return 0;
}

static const struct adrv9002_chip_info adrv9002_info[] = {
	[ID_ADRV9002] = {
		.channels = adrv9002_phy_chan,
		.num_channels = ARRAY_SIZE(adrv9002_phy_chan),
		.cmos_profile = "Navassa_CMOS_profile.json",
		.lvd_profile = "Navassa_LVDS_profile.json",
		.name = "adrv9002-phy",
		.n_tx = ADRV9002_CHANN_MAX,
	},
	[ID_ADRV9002_RX2TX2] = {
		.channels = adrv9002_phy_chan,
		.num_channels = ARRAY_SIZE(adrv9002_phy_chan),
		.cmos_profile = "Navassa_CMOS_profile.json",
		.lvd_profile = "Navassa_LVDS_profile.json",
		.name = "adrv9002-phy",
		.n_tx = ADRV9002_CHANN_MAX,
		.rx2tx2 = true,
	},
	[ID_ADRV9003] = {
		.channels = adrv9003_phy_chan,
		.num_channels = ARRAY_SIZE(adrv9003_phy_chan),
		.cmos_profile = "Navassa_CMOS_profile_adrv9003.json",
		.lvd_profile = "Navassa_LVDS_profile_adrv9003.json",
		.name = "adrv9003-phy",
		.n_tx = 1,
	},
	[ID_ADRV9003_RX2TX2] = {
		.channels = adrv9003_phy_chan,
		.num_channels = ARRAY_SIZE(adrv9003_phy_chan),
		.cmos_profile = "Navassa_CMOS_profile_adrv9003.json",
		.lvd_profile = "Navassa_LVDS_profile_adrv9003.json",
		.name = "adrv9003-phy",
		.n_tx = 1,
		.rx2tx2 = true,
	},
};

static int adrv9002_get_external_los(struct adrv9002_rf_phy *phy)
{
	static const char *const ext_los[] = { "ext_lo1", "ext_lo2" };
	struct device_node *np = phy->spi->dev.of_node;
	struct device *dev = &phy->spi->dev;
	int ret, lo;

	for (lo = 0; lo < ARRAY_SIZE(phy->ext_los); lo++) {
		phy->ext_los[lo].clk = devm_clk_get_optional(dev, ext_los[lo]);
		if (IS_ERR(phy->ext_los[lo].clk))
			return PTR_ERR(phy->ext_los[lo].clk);
		if (!phy->ext_los[lo].clk)
			continue;

		ret = clk_prepare_enable(phy->ext_los[lo].clk);
		if (ret)
			return ret;

		ret = devm_add_action_or_reset(dev, adrv9002_clk_disable, phy->ext_los[lo].clk);
		if (ret)
			return ret;

		ret = of_clk_get_scale(np, ext_los[lo], &phy->ext_los[lo].scale);
		if (ret) {
			phy->ext_los[lo].scale.div = 10;
			phy->ext_los[lo].scale.mult = 1;
		}
	}

	return 0;
}

static int adrv9002_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct adrv9002_rf_phy *phy;
	struct clk *clk = NULL;
	int ret, c;

	clk = devm_clk_get(&spi->dev, "adrv9002_ext_refclk");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*phy));
	if (!indio_dev)
		return -ENOMEM;

	phy = iio_priv(indio_dev);
	phy->indio_dev = indio_dev;
	phy->spi = spi;

	phy->chip = of_device_get_match_data(&spi->dev);
	if (!phy->chip)
		phy->chip = (const struct adrv9002_chip_info *)spi_get_device_id(spi)->driver_data;
	if (!phy->chip)
		return -EINVAL;

	/* in the future we might want to get rid of 'phy->rx2tx2' and just use chip_info  */
	if (phy->chip->rx2tx2)
		phy->rx2tx2 = true;

	mutex_init(&phy->lock);
	phy->adrv9001 = &phy->adrv9001_device;
	phy->hal.spi = spi;
	phy->adrv9001->common.devHalInfo = &phy->hal;

	/* initialize channel numbers and ports here since these will never change */
	for (c = 0; c < ADRV9002_CHANN_MAX; c++) {
		phy->rx_channels[c].channel.idx = c;
		phy->rx_channels[c].channel.number = c + ADI_CHANNEL_1;
		phy->rx_channels[c].channel.port = ADI_RX;
		phy->channels[c * 2] = &phy->rx_channels[c].channel;
		phy->tx_channels[c].channel.idx = c;
		phy->tx_channels[c].channel.number = c + ADI_CHANNEL_1;
		phy->tx_channels[c].channel.port = ADI_TX;
		phy->channels[c * 2 + 1] = &phy->tx_channels[c].channel;
	}

	phy->hal.reset_gpio = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(phy->hal.reset_gpio))
		return PTR_ERR(phy->hal.reset_gpio);

	ret = clk_prepare_enable(clk);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&spi->dev, adrv9002_clk_disable, clk);
	if (ret)
		return ret;

	ret = adrv9002_get_external_los(phy);
	if (ret)
		return ret;

	ret = adrv9002_parse_dt(phy);
	if (ret)
		return ret;

	if (phy->rx2tx2) {
		phy->ssi_sync = devm_gpiod_get(&spi->dev, "ssi-sync", GPIOD_OUT_LOW);
		if (IS_ERR(phy->ssi_sync))
			return PTR_ERR(phy->ssi_sync);
	}

	return adrv9002_register_axi_converter(phy);
}

static const struct of_device_id adrv9002_of_match[] = {
	{.compatible = "adi,adrv9002", .data = &adrv9002_info[ID_ADRV9002]},
	{.compatible = "adi,adrv9002-rx2tx2", .data = &adrv9002_info[ID_ADRV9002_RX2TX2]},
	{.compatible = "adi,adrv9003", .data = &adrv9002_info[ID_ADRV9003]},
	{.compatible = "adi,adrv9003-rx2tx2", .data = &adrv9002_info[ID_ADRV9003_RX2TX2]},
	{}
};
MODULE_DEVICE_TABLE(of, adrv9002_of_match);

static const struct spi_device_id adrv9002_ids[] = {
	{"adrv9002", (kernel_ulong_t)&adrv9002_info[ID_ADRV9002]},
	{"adrv9002-rx2tx2", (kernel_ulong_t)&adrv9002_info[ID_ADRV9002_RX2TX2]},
	{"adrv9003", (kernel_ulong_t)&adrv9002_info[ID_ADRV9003]},
	{"adrv9003-rx2tx2", (kernel_ulong_t)&adrv9002_info[ID_ADRV9003_RX2TX2]},
	{}
};
MODULE_DEVICE_TABLE(spi, adrv9002_ids);

static struct spi_driver adrv9002_driver = {
	.driver = {
		.name	= "adrv9002",
		.of_match_table = adrv9002_of_match,
	},
	.probe		= adrv9002_probe,
	.id_table	= adrv9002_ids,
};
module_spi_driver(adrv9002_driver);

MODULE_AUTHOR("Michael Hennerich <michael.hennerich@analog.com>");
MODULE_AUTHOR("Nuno Sá <nuno.sa@analog.com>");
MODULE_DESCRIPTION("Analog Devices ADRV9002 ADC");
MODULE_LICENSE("GPL v2");
