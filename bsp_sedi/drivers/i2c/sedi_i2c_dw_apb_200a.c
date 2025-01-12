/*
 * Copyright (c) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "sedi_i2c_dw_apb_200a.h"
#include "sedi_driver_pm.h"
#include "sedi_driver_core.h"

#define I2C_FIFO_DEFAULT_WATERMARK (I2C_FIFO_DEPTH / 2U)

/*
 * Because the SEDI i2c' data_cmd register is 16bit, the STOP, RESTART
 * flags need to be set in high 8 bit.
 * The first byte and last byte should be manually set RESTART and STOP bit.
 */
#define I2C_DMA_REMOVE_HEAD_TAIL (2)
#define LBW_CLK_MHZ (sedi_pm_get_lbw_clock() / 1000000)

enum { I2C_SPEED_STANDARD = 0, I2C_SPEED_FAST, I2C_SPEED_FAST_PLUS, I2C_SPEED_HIGH, I2C_SPEED_MAX };

typedef enum {
	I2C_DMA_DIRECTION_TX = 0,
	I2C_DMA_DIRECTION_RX = 1,
	I2C_DMA_DIRECTION_RX_CMD = 2
} i2c_dma_diretion_t;

struct i2c_context {
	uint32_t base;

	sedi_i2c_capabilities_t capability;
	sedi_i2c_status_t status;
	sedi_i2c_event_cb_t cb_event;

	uint8_t *buf;
	uint32_t buf_size;
	uint32_t buf_index;
	uint32_t rx_cmd_index;

	volatile uint8_t pending;

	uint32_t isr_flag;
	uint32_t isr_data;

	int dma;
	int tx_dma_chan;
	int rx_dma_chan;
	int dma_is_tx;
	int dma_handshake;

	dma_memory_type_t tx_memory_type;
	dma_memory_type_t rx_memory_type;

	int speed;
};

static uint32_t regval_speed[I2C_SPEED_MAX] = { BIT_SPEED_STANDARD, BIT_SPEED_FAST,
						BIT_SPEED_FAST_PLUS, BIT_SPEED_HIGH };

static uint32_t regval_scl_hcnt[I2C_SPEED_MAX] = { 0 };

static uint32_t regval_scl_lcnt[I2C_SPEED_MAX] = { 0 };

static void init_i2c_prescale(void)
{
	if (regval_scl_hcnt[I2C_SPEED_STANDARD] == 0) {
		regval_scl_hcnt[I2C_SPEED_STANDARD] = LBW_CLK_MHZ * I2C_SS_SCL_HIGH / NS_PER_US;
		regval_scl_hcnt[I2C_SPEED_FAST] = LBW_CLK_MHZ * I2C_FS_SCL_HIGH / NS_PER_US;
		regval_scl_hcnt[I2C_SPEED_FAST_PLUS] = LBW_CLK_MHZ * I2C_FSP_SCL_HIGH / NS_PER_US;
		regval_scl_hcnt[I2C_SPEED_HIGH] = LBW_CLK_MHZ * I2C_HS_SCL_HIGH / NS_PER_US;

		regval_scl_lcnt[I2C_SPEED_STANDARD] = LBW_CLK_MHZ * I2C_SS_SCL_LOW / NS_PER_US;
		regval_scl_lcnt[I2C_SPEED_FAST] = LBW_CLK_MHZ * I2C_FS_SCL_LOW / NS_PER_US;
		regval_scl_lcnt[I2C_SPEED_FAST_PLUS] = LBW_CLK_MHZ * I2C_FSP_SCL_LOW / NS_PER_US;
		regval_scl_lcnt[I2C_SPEED_HIGH] = LBW_CLK_MHZ * I2C_HS_SCL_LOW / NS_PER_US;
	}
}

static void dw_i2c_enable(uint32_t base)
{
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)base;

	/* prevent tx empty interrupt after enable */
	i2c->intr_mask = 0;

	if (i2c->enable_status & BIT_STATUS_ENABLE) {
		return;
	}

	i2c->enable = 1;
	while (0 == (i2c->enable_status & BIT_STATUS_ENABLE))
		;
}

static int dw_i2c_disable(uint32_t base)
{
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)base;

	/* If already disabled, return */
	if ((i2c->enable & 0x1) == 0) {
		return 0;
	}

	i2c->dma_cr = 0;
	i2c->intr_mask = 0;

	if (i2c->status & BIT_STATUS_MASTER_ACTIVE) {
		i2c->enable |= BIT_ABORT_TRANSFER;
		while ((i2c->enable_status & BIT_ABORT_TRANSFER) != 0)
			;
	}

	while (i2c->status & BIT_STATUS_MASTER_ACTIVE)
		;

	i2c->enable = 0;
	while ((i2c->enable_status & BIT_STATUS_ENABLE))
		;

	return 0;
}

static int dw_i2c_config_addr(uint32_t base, uint16_t slave_addr)
{
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)base;

	if (slave_addr & SEDI_I2C_ADDRESS_10BIT) {
		i2c->tar = BIT_10BIT_MASTER_DYNAMIC;
	}

	i2c->tar = slave_addr;

	return 0;
}

static int dw_i2c_config_speed(uint32_t base, int speed)
{
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)base;

	if (i2c->enable_status & BIT_STATUS_ENABLE) {
		return -1;
	}

	i2c->con = CONFIG_MASTER_DEFAULT | regval_speed[speed];

	i2c->ss_scl_hcnt = regval_scl_hcnt[I2C_SPEED_STANDARD];
	i2c->ss_scl_lcnt = regval_scl_lcnt[I2C_SPEED_STANDARD];

	/* if speed is not fast or fast plus mode, the reg will not be used */
	i2c->fs_scl_hcnt = regval_scl_hcnt[speed];
	i2c->fs_scl_lcnt = regval_scl_lcnt[speed];

	i2c->hs_scl_hcnt = regval_scl_hcnt[I2C_SPEED_HIGH];
	i2c->hs_scl_lcnt = regval_scl_lcnt[I2C_SPEED_HIGH];

	if (speed == I2C_SPEED_HIGH) {
		i2c->fs_scl_hcnt = regval_scl_hcnt[I2C_SPEED_FAST];
		i2c->fs_scl_lcnt = regval_scl_lcnt[I2C_SPEED_FAST];
	}

	return 0;
}

static int dw_i2c_config_txfifo(uint32_t base, uint32_t watermark)
{
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)base;

	i2c->tx_tl = watermark;

	return 0;
}

static int dw_i2c_config_rxfifo(uint32_t base, uint32_t watermark)
{
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)base;

	i2c->rx_tl = watermark - 1;

	return 0;
}

static int dw_i2c_poll_write(uint32_t base, const uint8_t *buffer, uint32_t length, bool pending)
{
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)base;

	uint32_t cmd = BIT_CMD_WRITE;

	for (uint32_t i = 0; i < length; i++) {
		if ((i == length - 1) && (!pending)) {
			cmd |= BIT_CMD_STOP;
		}

		i2c->data_cmd = buffer[i] | cmd;
		/* Check abort and error */
		do {
			if (i2c->raw_intr_stat & CONFIG_INTR_ERROR) {
				return -1;
			}

		} while (0 == (i2c->status & BIT_STATUS_TX_EMPTY));
	}

	/* Wait for end */
	while (1 == (i2c->status & BIT_STATUS_ACTIVE)) {
	}

	return 0;
}

static int dw_i2c_poll_read(uint32_t base, uint8_t *buffer, uint32_t length, bool pending)
{
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)base;
	uint32_t cmd = BIT_CMD_READ;

	for (uint32_t i = 0; i < length; i++) {
		if ((i == length - 1) && (!pending)) {
			cmd |= BIT_CMD_STOP;
		}

		i2c->data_cmd = cmd;
		do {
			/* Check abort and error*/
			if (i2c->raw_intr_stat & CONFIG_INTR_ERROR) {
				return -1;
			}

		} while (0 == (i2c->status & BIT_STATUS_RX_NOT_EMPTY));

		buffer[i] = (uint8_t)(i2c->data_cmd & 0xFF);
	}

	/* Wait for end */
	while (1 == (i2c->status & BIT_STATUS_ACTIVE)) {
	}

	return 0;
}

static inline void dw_i2c_irq_config(uint32_t base, uint32_t config)
{
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)base;

	i2c->intr_mask = config;
}

static uint32_t dw_i2c_clear_interrupt(uint32_t base)
{
	i2c_dw_apb200a_regs_t *i2c = (void *)base;
	uint32_t value;
	uint32_t stat;

	stat = i2c->intr_stat;

	value = i2c->clr_rx_under;
	value = i2c->clr_rx_over;
	value = i2c->clr_tx_over;
	value = i2c->clr_tx_abrt;
	value = i2c->clr_stop_det;
	value = i2c->clr_intr;
	PARAM_UNUSED(value);

	return stat;
}

static void dw_i2c_dma_enable(uint32_t base, int fifo_depth)
{
	i2c_dw_apb200a_regs_t *i2c = (void *)base;

	i2c->dma_tdlr = fifo_depth;
	i2c->dma_rdlr = fifo_depth - 1;

	i2c->dma_cr = BIT_DMA_ENABLE;
}

/* force enter IDLE mode */
static void dw_i2c_abort(struct i2c_context *context)
{
	i2c_dw_apb200a_regs_t *i2c = (void *)context->base;
	uint32_t value;

	i2c->dma_cr = 0;
	i2c->intr_mask = 0;

	i2c->enable |= BIT_ABORT_TRANSFER;

	/* Waiting for abort operation finished, HW can clear */
	while ((i2c->raw_intr_stat & BIT_INTR_TX_ABORT) == 0) {
		;
	}

	while (i2c->rxflr != 0) {
		value = i2c->data_cmd;
	}

	dw_i2c_irq_config(context->base, 0);

	dw_i2c_config_txfifo(context->base, 0);
	dw_i2c_config_rxfifo(context->base, 0);

	/* Clear the abort state */
	dw_i2c_clear_interrupt(context->base);
	PARAM_UNUSED(value);
}

static uint32_t dw_i2c_dr_address(uint32_t base)
{
	i2c_dw_apb200a_regs_t *i2c = (void *)base;

	return (uint32_t)&i2c->data_cmd;
}

static uint32_t dw_i2c_abort_analysis(uint32_t base)
{
	i2c_dw_apb200a_regs_t *i2c = (void *)base;
	uint32_t abort_src, event = SEDI_I2C_EVENT_TRANSFER_INCOMPLETE;

	abort_src = i2c->tx_abrt_source;

	if (abort_src & BIT_ABORT_SOURCE_NO_ACK) {
		event |= SEDI_I2C_EVENT_ADDRESS_NACK;
	}

	if (abort_src & BIT_ABORT_ARB_LOST) {
		event |= SEDI_I2C_EVENT_ARBITRATION_LOST;
	}

	return event;
}

/******************************************************************************
 * SEDI interface
 *****************************************************************************/

#define SEDI_I2C_DRV_VERSION SEDI_DRIVER_VERSION_MAJOR_MINOR(1, 0)
#define SEDI_I2C_DMA_CHANNEL_UNUSED (-1)

static const sedi_driver_version_t driver_version = { SEDI_I2C_API_VERSION, SEDI_I2C_DRV_VERSION };

static sedi_i2c_capabilities_t driver_capabilities[SEDI_I2C_NUM] = { 0 };
/* Used for I2C DMA Rx */
static uint8_t dma_cmd __attribute__((aligned(32))) = 1;

static struct i2c_context contexts[SEDI_I2C_NUM];

sedi_driver_version_t sedi_i2c_get_version(void)
{
	return driver_version;
}

int sedi_i2c_get_capabilities(IN sedi_i2c_t i2c_device, sedi_i2c_capabilities_t *cap)
{
	struct i2c_context *context = &contexts[i2c_device];
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)(context->base);

	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);

	if (sedi_dev_is_self_owned(SEDI_DEVID_I2C0 + i2c_device)) {
		driver_capabilities[i2c_device].is_available = 1;
	} else {
		driver_capabilities[i2c_device].is_available = 0;
	}

	driver_capabilities[i2c_device].rx_buffer_depth = i2c->comp_param_1.rx_buffer_depth + 1;
	driver_capabilities[i2c_device].tx_buffer_depth = i2c->comp_param_1.tx_buffer_depth + 1;
	driver_capabilities[i2c_device].dma = i2c->comp_param_1.has_dma;

	*cap = driver_capabilities[i2c_device];

	return SEDI_DRIVER_OK;
}

int32_t sedi_i2c_init(IN sedi_i2c_t i2c_device,
		IN sedi_i2c_event_cb_t cb_event, IN uint32_t base)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);

	struct i2c_context *context = &contexts[i2c_device];

	if (!sedi_dev_is_self_owned(SEDI_DEVID_I2C0 + i2c_device)) {
		return SEDI_DRIVER_ERROR_NO_DEV;
	}

	context->cb_event = cb_event;
	context->tx_dma_chan = SEDI_I2C_DMA_CHANNEL_UNUSED;
	context->rx_dma_chan = SEDI_I2C_DMA_CHANNEL_UNUSED;

	init_i2c_prescale();

	context->base = base;

	return SEDI_DRIVER_OK;
}

int32_t sedi_i2c_uninit(IN sedi_i2c_t i2c_device)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);

	struct i2c_context *context = &contexts[i2c_device];

	context->cb_event = NULL;

	return SEDI_DRIVER_OK;
}

int32_t sedi_i2c_set_power(IN sedi_i2c_t i2c_device, IN sedi_power_state_t state)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);

	struct i2c_context *context = &contexts[i2c_device];
	sedi_devid_t id = SEDI_DEVID_I2C0 + i2c_device;
	int32_t ret = SEDI_DRIVER_OK;

	switch (state) {
	case SEDI_POWER_FULL:
		sedi_pm_set_device_power(id, state);
		dw_i2c_irq_config(context->base, 0);
		dw_i2c_config_speed(context->base, context->speed);
		break;
	case SEDI_POWER_SUSPEND:
	case SEDI_POWER_FORCE_SUSPEND:
	case SEDI_POWER_LOW:
		dw_i2c_irq_config(context->base, 0);
		sedi_pm_set_device_power(id, state);
		break;
	case SEDI_POWER_OFF:
	default:
		ret = SEDI_DRIVER_ERROR_UNSUPPORTED;
		break;
	}

	return ret;
}

static void callback_tx_dma_transfer(const sedi_dma_t dma, const int chan, const int event,
				     void *param)
{
	struct i2c_context *context = &contexts[(int)param];
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)(context->base);
	uint8_t last_byte;
	int pending;
	int result;

	PARAM_UNUSED(dma);
	PARAM_UNUSED(chan);

	result = event;
	last_byte = context->buf[context->buf_size - 1];
	pending = context->pending;

	i2c->dma_cr &= ~BIT_DMA_TX_ENABLE;
	context->buf_index = context->buf_size;
	context->tx_dma_chan = SEDI_I2C_DMA_CHANNEL_UNUSED;

	if (pending) {
		/* If no STOP, all data sent by DMA, enable TX_EMPTY interrupt
		 * to get stop condition.
		 */
		i2c->tx_tl = 0;
		i2c->intr_mask |= BIT_INTR_TX_EMPTY;
	} else {
		/* WA for I2C DMA transfer, wait until FIFO not full*/
		/* If need STOP, last data sent by interrupt */
		while (i2c->txflr == I2C_FIFO_DEPTH) {
		}
		i2c->data_cmd = last_byte | BIT_CMD_STOP;
	}
	PARAM_UNUSED(result);
}

static void callback_rx_dma_transfer(const sedi_dma_t dma, const int chan, const int event,
				     void *param)
{
	struct i2c_context *context = &contexts[(int)param];
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)(context->base);

	PARAM_UNUSED(dma);
	PARAM_UNUSED(chan);
	PARAM_UNUSED(event);

	/* Disable the Rx dma request */
	i2c->dma_cr &= ~BIT_DMA_RX_ENABLE;
	/* Let stop detect interrupt end transfer */
	context->buf_index = context->buf_size;
	context->rx_dma_chan = SEDI_I2C_DMA_CHANNEL_UNUSED;
}

static void callback_rx_cmd_dma_transfer(const sedi_dma_t dma, const int chan, const int event,
					 void *param)
{
	struct i2c_context *context = &contexts[(int)param];
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)(context->base);

	PARAM_UNUSED(dma);
	PARAM_UNUSED(chan);
	PARAM_UNUSED(event);

	/* Send the last command, which is different from previous one*/
	while (i2c->txflr == I2C_FIFO_DEPTH) {
	}
	if (context->pending) {
		i2c->data_cmd = BIT_CMD_READ;
	} else {
		i2c->data_cmd = BIT_CMD_READ | BIT_CMD_STOP;
	}

	context->tx_dma_chan = SEDI_I2C_DMA_CHANNEL_UNUSED;
}

static int config_and_enable_dma_channel(sedi_i2c_t i2c_dev, int dma, int handshake, int chan,
					 uint32_t src, uint32_t dst, uint32_t len,
					 i2c_dma_diretion_t dir)
{
	int ret;
	int dma_dir;
	int dma_per_dir;
	dma_transfer_width_t wid = DMA_TRANS_WIDTH_8;
	struct i2c_context *context = &contexts[i2c_dev];

	if (dir == I2C_DMA_DIRECTION_TX) {
		dma_dir = DMA_MEMORY_TO_PERIPHERAL;
		dma_per_dir = DMA_HS_PER_TX;
		ret = sedi_dma_init(dma, chan, callback_tx_dma_transfer, (void *)i2c_dev);
		DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);
		ret = sedi_dma_set_power(dma, chan, SEDI_POWER_FULL);
		DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);
		/* Set memory type */
		ret = sedi_dma_control(dma, chan, SEDI_CONFIG_DMA_SR_MEM_TYPE,
				       context->tx_memory_type);
		DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);
	} else if (dir == I2C_DMA_DIRECTION_RX) {
		dma_dir = DMA_PERIPHERAL_TO_MEMORY;
		dma_per_dir = DMA_HS_PER_RX;
		ret = sedi_dma_init(dma, chan, callback_rx_dma_transfer, (void *)i2c_dev);
		DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);
		ret = sedi_dma_set_power(dma, chan, SEDI_POWER_FULL);
		DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);
		ret = sedi_dma_control(dma, chan, SEDI_CONFIG_DMA_DT_MEM_TYPE,
				       context->rx_memory_type);
		DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);
	} else {
		dma_dir = DMA_PERIPHERAL_TO_PERIPHERAL;
		dma_per_dir = DMA_HS_PER_TX;
		wid = DMA_TRANS_WIDTH_8;
		ret = sedi_dma_init(dma, chan, callback_rx_cmd_dma_transfer, (void *)i2c_dev);
		DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);
		ret = sedi_dma_set_power(dma, chan, SEDI_POWER_FULL);
		DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);
	}

	ret = sedi_dma_control(dma, chan, SEDI_CONFIG_DMA_BURST_LENGTH, DMA_BURST_TRANS_LENGTH_1);
	DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);

	ret = sedi_dma_control(dma, chan, SEDI_CONFIG_DMA_SR_TRANS_WIDTH, wid);
	DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);

	ret = sedi_dma_control(dma, chan, SEDI_CONFIG_DMA_DT_TRANS_WIDTH, wid);
	DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);

	ret = sedi_dma_control(dma, chan, SEDI_CONFIG_DMA_HS_DEVICE_ID, handshake);
	DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);

	ret = sedi_dma_control(dma, chan, SEDI_CONFIG_DMA_HS_POLARITY, DMA_HS_POLARITY_HIGH);
	DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);

	ret = sedi_dma_control(dma, chan, SEDI_CONFIG_DMA_DIRECTION, dma_dir);
	DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);

	ret = sedi_dma_control(dma, chan, SEDI_CONFIG_DMA_HS_DEVICE_ID_PER_DIR, dma_per_dir);
	DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);

	ret = sedi_dma_start_transfer(dma, chan, src, dst, len);
	DBG_CHECK(0 == ret, SEDI_DRIVER_ERROR);

	return ret;
}

int32_t sedi_i2c_master_write_dma(IN sedi_i2c_t i2c_device, IN uint32_t dma, IN uint32_t dma_chan,
				  IN uint32_t addr, IN uint8_t *data, IN uint32_t num,
				  IN bool pending)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(0 != (addr & BIT_MASK_ADDR), SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(NULL != data, SEDI_DRIVER_ERROR_PARAMETER);

	int ret;
	struct i2c_context *context = &contexts[i2c_device];
	uint32_t size;

	if (num <= SEDI_I2C_DMA_LENGTH_LIMIT) {
		return SEDI_DRIVER_ERROR_PARAMETER;
	}

	if (context->status.busy) {
		return SEDI_DRIVER_ERROR_BUSY;
	}

	/* Clear all status first */
	dw_i2c_clear_interrupt(context->base);

	ret = dw_i2c_config_addr(context->base, addr);
	if (ret != 0) {
		return SEDI_DRIVER_ERROR;
	}

	/* Enable I2C */
	dw_i2c_enable(context->base);

	context->status.busy = 1U;
	context->status.direction = 0U;
	/* Reset event to default */
	context->status.event = SEDI_I2C_EVENT_TRANSFER_NONE;
	context->pending = pending;
	context->buf = (void *)data;
	context->buf_size = num;
	context->dma_is_tx = true;
	context->buf_index = 0;
	context->dma = dma;
	context->tx_dma_chan = dma_chan;

	/* No STOP flag, all data sent by DMA */
	if (pending) {
		size = num;
	} else {
		size = num - 1;
	}
	config_and_enable_dma_channel(i2c_device, dma, context->dma_handshake, dma_chan,
				      (uint32_t)data, dw_i2c_dr_address(context->base), size,
				      I2C_DMA_DIRECTION_TX);

	dw_i2c_dma_enable(context->base, 1);

	dw_i2c_irq_config(context->base, CONFIG_INTR_ERROR | BIT_INTR_STOP_DET);

	return SEDI_DRIVER_OK;
}

int32_t sedi_i2c_master_read_dma(IN sedi_i2c_t i2c_device, IN uint32_t dma, IN uint32_t dma_chan,
				 IN uint32_t cmd_dma_chan, IN uint32_t addr, OUT uint8_t *data,
				 IN uint32_t num, IN bool pending)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(0 != (addr & BIT_MASK_ADDR), SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(NULL != data, SEDI_DRIVER_ERROR_PARAMETER);

	if (num <= SEDI_I2C_DMA_LENGTH_LIMIT) {
		return SEDI_DRIVER_ERROR_PARAMETER;
	}

	int ret;
	struct i2c_context *context = &contexts[i2c_device];
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)(context->base);

	if (context->status.busy) {
		return SEDI_DRIVER_ERROR_BUSY;
	}

	/* Clear all status first */
	dw_i2c_clear_interrupt(context->base);

	ret = dw_i2c_config_addr(context->base, addr);
	if (ret != 0) {
		return SEDI_DRIVER_ERROR;
	}

	/* Enable I2C */
	dw_i2c_enable(context->base);

	context->status.busy = 1U;
	context->status.direction = 1U;
	/* Reset event to default */
	context->status.event = SEDI_I2C_EVENT_TRANSFER_NONE;
	context->pending = pending;
	context->buf = data;
	context->buf_size = num;
	context->dma_is_tx = false;
	context->buf_index = 0;
	context->dma = dma;
	context->tx_dma_chan = cmd_dma_chan;
	context->rx_dma_chan = dma_chan;

	/* First write the command into register */
	i2c->data_cmd = BIT_CMD_READ | BIT_CMD_RESTART;

	/* Clean the cmd */
	sedi_core_clean_dcache_by_addr((uint32_t *)(&dma_cmd), sizeof(dma_cmd));

	/* Command is written for next read per request */
	config_and_enable_dma_channel(i2c_device, dma, context->dma_handshake, cmd_dma_chan,
				      (uint32_t)&dma_cmd, dw_i2c_dr_address(context->base) + 1,
				      num - 2, I2C_DMA_DIRECTION_RX_CMD);

	/* data transfer */
	config_and_enable_dma_channel(i2c_device, dma, context->dma_handshake, dma_chan,
				      dw_i2c_dr_address(context->base), (uint32_t)data, num,
				      I2C_DMA_DIRECTION_RX);

	dw_i2c_dma_enable(context->base, 1);

	dw_i2c_irq_config(context->base, CONFIG_INTR_ERROR | BIT_INTR_STOP_DET);

	return SEDI_DRIVER_OK;
}

int32_t sedi_i2c_master_write_async(IN sedi_i2c_t i2c_device, IN uint32_t addr, IN uint8_t *data,
				    IN uint32_t num, IN bool pending)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(0 != (addr & BIT_MASK_ADDR), SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(NULL != data, SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(0 != num, SEDI_DRIVER_ERROR_PARAMETER);

	struct i2c_context *context = &contexts[i2c_device];

	if (context->status.busy) {
		return SEDI_DRIVER_ERROR_BUSY;
	}

	/* Clear all status first */
	dw_i2c_clear_interrupt(context->base);
	/* Config slave address */
	dw_i2c_config_addr(context->base, addr);

	/* Set watermark */
	if (num < I2C_FIFO_DEFAULT_WATERMARK) {
		/* If less than half FIFO, just set watermark to transfer size
		 */
		dw_i2c_config_txfifo(context->base, num);
	} else {
		dw_i2c_config_txfifo(context->base, I2C_FIFO_DEFAULT_WATERMARK);
	}

	/* Enable I2C */
	dw_i2c_enable(context->base);

	/* Set internal state */
	context->status.busy = 1U;
	context->status.direction = 0U;
	/* Reset event to default */
	context->status.event = SEDI_I2C_EVENT_TRANSFER_NONE;

	context->buf = (uint8_t *)data;
	context->buf_size = num;
	context->rx_cmd_index = 0;
	context->buf_index = 0;
	context->pending = pending;

	dw_i2c_irq_config(context->base, CONFIG_INTR_SEND);

	return SEDI_DRIVER_OK;
}

int32_t sedi_i2c_master_read_async(IN sedi_i2c_t i2c_device, IN uint32_t addr, OUT uint8_t *data,
				   IN uint32_t num, IN bool pending)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(0 != (addr & BIT_MASK_ADDR), SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(NULL != data, SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(0 != num, SEDI_DRIVER_ERROR_PARAMETER);

	struct i2c_context *context = &contexts[i2c_device];

	if (context->status.busy) {
		return SEDI_DRIVER_ERROR_BUSY;
	}

	/* Clear all status first */
	dw_i2c_clear_interrupt(context->base);
	dw_i2c_config_addr(context->base, addr);

	/* Set watermark, receive needs both tx and rx */
	if (num < I2C_FIFO_DEFAULT_WATERMARK) {
		/* If less than half FIFO, just set watermark to transfer size
		 */
		dw_i2c_config_rxfifo(context->base, num);
		dw_i2c_config_txfifo(context->base, num);
	} else {
		dw_i2c_config_rxfifo(context->base, I2C_FIFO_DEFAULT_WATERMARK);
		dw_i2c_config_txfifo(context->base, I2C_FIFO_DEFAULT_WATERMARK);
	}

	/* Enable I2C */
	dw_i2c_enable(context->base);

	context->status.busy = 1U;
	context->status.direction = 1U;
	/* Reset event to default */
	context->status.event = SEDI_I2C_EVENT_TRANSFER_NONE;

	context->buf = (uint8_t *)data;
	context->buf_size = num;
	context->buf_index = 0;
	context->rx_cmd_index = 0;
	context->pending = pending;

	dw_i2c_irq_config(context->base, CONFIG_INTR_RECV);

	return SEDI_DRIVER_OK;
}

int32_t sedi_i2c_master_poll_write(IN sedi_i2c_t i2c_device, IN uint32_t addr, IN uint8_t *data,
				   IN uint32_t num, IN bool pending)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(0 != (addr & BIT_MASK_ADDR), SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(NULL != data, SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(0 != num, SEDI_DRIVER_ERROR_PARAMETER);

	int ret;
	struct i2c_context *context = &contexts[i2c_device];

	if (context->status.busy) {
		return SEDI_DRIVER_ERROR_BUSY;
	}

	ret = dw_i2c_config_addr(context->base, addr);

	context->status.busy = 1U;
	context->status.direction = 0U;
	/* Reset event to default */
	context->status.event = SEDI_I2C_EVENT_TRANSFER_NONE;

	dw_i2c_enable(context->base);

	ret = dw_i2c_poll_write(context->base, data, num, pending);

	if (ret != 0) {
		context->status.event = dw_i2c_abort_analysis(context->base);
	} else {
		context->status.event = SEDI_I2C_EVENT_TRANSFER_DONE;
	}
	context->status.busy = 0U;
	context->buf_index = num;

	return (ret == 0) ? SEDI_DRIVER_OK : SEDI_DRIVER_ERROR;
}

int32_t sedi_i2c_master_poll_read(IN sedi_i2c_t i2c_device, IN uint32_t addr, OUT uint8_t *data,
				  IN uint32_t num, IN bool pending)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(0 != (addr & BIT_MASK_ADDR), SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(NULL != data, SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(0 != num, SEDI_DRIVER_ERROR_PARAMETER);

	int ret;
	struct i2c_context *context = &contexts[i2c_device];

	if (context->status.busy) {
		return SEDI_DRIVER_ERROR_BUSY;
	}

	ret = dw_i2c_config_addr(context->base, addr);
	if (ret != 0) {
		return SEDI_DRIVER_ERROR;
	}

	/* Enable I2C */
	dw_i2c_enable(context->base);

	context->status.busy = 1U;
	context->status.direction = 1U;
	/* Reset event to default */
	context->status.event = SEDI_I2C_EVENT_TRANSFER_DONE;

	ret = dw_i2c_poll_read(context->base, data, num, pending);

	if (ret != 0) {
		context->status.event = dw_i2c_abort_analysis(context->base);
	}
	context->status.busy = 0U;
	context->buf_index = num;

	return (ret == 0) ? SEDI_DRIVER_OK : SEDI_DRIVER_ERROR;
}

int32_t sedi_i2c_get_data_count(IN sedi_i2c_t i2c_device)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);

	struct i2c_context *context = &contexts[i2c_device];

	return context->buf_index;
}

int32_t sedi_i2c_control(IN sedi_i2c_t i2c_device, IN uint32_t control, IN uint32_t arg)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);

	int ret = 0;
	struct i2c_context *context = &contexts[i2c_device];

	switch (control) {
	case SEDI_I2C_BUS_SPEED:
		if (arg == SEDI_I2C_BUS_SPEED_STANDARD) {
			context->speed = I2C_SPEED_STANDARD;
		} else if (arg == SEDI_I2C_BUS_SPEED_FAST) {
			context->speed = I2C_SPEED_FAST;
		} else if (arg == SEDI_I2C_BUS_SPEED_FAST_PLUS) {
			context->speed = I2C_SPEED_FAST_PLUS;
		} else if (arg == SEDI_I2C_BUS_SPEED_HIGH) {
			context->speed = I2C_SPEED_HIGH;
		} else {
			ret = SEDI_DRIVER_ERROR;
		}
		dw_i2c_disable(context->base);
		dw_i2c_config_speed(context->base, context->speed);
		break;

	/* force i2c enter IDLE mode */
	case SEDI_I2C_ABORT_TRANSFER:
#if 0
		/* TODO: get back after DMA driver is enabled */
		if (context->tx_dma_chan > SEDI_I2C_DMA_CHANNEL_UNUSED) {
			sedi_dma_abort_transfer(context->dma,
						context->tx_dma_chan);
			context->tx_dma_chan = SEDI_I2C_DMA_CHANNEL_UNUSED;
		}
		if (context->rx_dma_chan > SEDI_I2C_DMA_CHANNEL_UNUSED) {
			sedi_dma_abort_transfer(context->dma,
						context->rx_dma_chan);
			context->rx_dma_chan = SEDI_I2C_DMA_CHANNEL_UNUSED;
		}
#endif
		dw_i2c_abort(context);
		context->pending = 0;
		context->status.busy = 0;
		break;
	case SEDI_I2C_SET_TX_MEMORY_TYPE:
		context->tx_memory_type = arg;
		break;
	case SEDI_I2C_SET_RX_MEMORY_TYPE:
		context->rx_memory_type = arg;
		break;
	default:
		ret = SEDI_DRIVER_ERROR;
		break;
	}

	if (ret != 0) {
		return SEDI_DRIVER_ERROR;
	}

	return SEDI_DRIVER_OK;
}

int sedi_i2c_get_status(IN sedi_i2c_t i2c_device, sedi_i2c_status_t *status)
{
	DBG_CHECK(i2c_device < SEDI_I2C_NUM, SEDI_DRIVER_ERROR_PARAMETER);
	DBG_CHECK(NULL != status, SEDI_DRIVER_ERROR_PARAMETER);

	struct i2c_context *context = &contexts[i2c_device];

	*status = context->status;

	return SEDI_DRIVER_OK;
}

static void i2c_isr_recv(sedi_i2c_t i2c_device)
{
	struct i2c_context *context = &contexts[i2c_device];
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)(context->base);
	uint32_t data_remain;

	/* Receive all data from FIFO */
	uint32_t size = i2c->rxflr;

	if (context->rx_cmd_index < size) {
		uint32_t dummy;

		while (size) {
			dummy = i2c->data_cmd;
			size--;
		}
		(void)dummy;

		return;
	}

	while (size > 0) {
		context->buf[context->buf_index] = (i2c->data_cmd & 0xFF);
		context->buf_index++;
		size--;
	}

	/* If need to change watermark */
	data_remain = context->buf_size - context->buf_index;
	if ((data_remain <= i2c->rx_tl) && (data_remain != 0)) {
		i2c->rx_tl = data_remain - 1;
	}
}

/* Used for sending cmd for I2C read operation */
static void i2c_isr_ask_data(sedi_i2c_t i2c_device)
{
	struct i2c_context *context = &contexts[i2c_device];
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)(context->base);

	uint32_t tx_fifo_space = 0, rx_fifo_space = 0;
	uint32_t rx_pending = context->rx_cmd_index - context->buf_index;
	uint32_t size = context->buf_size - context->rx_cmd_index;
	uint32_t fifo_size = 0;
	uint32_t data = BIT_CMD_READ;
	bool last_data = false;

	/* No need to send anymore */
	if (context->rx_cmd_index == context->buf_size) {
		return;
	}

	/* Decide how many space there are */
	if (I2C_FIFO_DEPTH - i2c->txflr <= rx_pending) {
		return;
	}

	tx_fifo_space = I2C_FIFO_DEPTH - i2c->txflr - rx_pending;

	/* To prevent RX FIFO overflow, need to make sure command number less
	 * than the space in RX FIFO.
	 */
	rx_fifo_space = I2C_FIFO_DEPTH - i2c->rxflr;

	if (tx_fifo_space < rx_fifo_space) {
		fifo_size = tx_fifo_space;
	} else {
		fifo_size = rx_fifo_space;
	}

	if (size <= fifo_size) {
		last_data = true;
	} else {
		size = fifo_size;
	}

	while (size > 0) {
		/* If it is first data, need RESTART */
		if (context->rx_cmd_index == 0) {
			data |= BIT_CMD_RESTART;
		}

		/* Last data need STOP flag */
		if ((size == 1) && last_data && (!(context->pending))) {
			data |= BIT_CMD_STOP;
			/*  Disable tx empty interrupt */
			i2c->intr_mask &= ~BIT_INTR_TX_EMPTY;
		}

		i2c->data_cmd = data;
		context->rx_cmd_index++;
		size--;

		/* Reset data */
		data = BIT_CMD_READ;
	}
}

static void i2c_isr_send(sedi_i2c_t i2c_device)
{
	struct i2c_context *context = &contexts[i2c_device];
	i2c_dw_apb200a_regs_t *i2c = (i2c_dw_apb200a_regs_t *)(context->base);

	uint32_t tx_fifo_space = 0;
	uint32_t size = context->buf_size - context->buf_index;
	uint32_t data = 0;
	uint8_t *buf = context->buf;
	bool last_data = false;

	if (context->buf_index == context->buf_size) {
		return;
	}

	/* Decide how many space there are */
	tx_fifo_space = I2C_FIFO_DEPTH - i2c->txflr;

	if (size <= tx_fifo_space) {
		last_data = true;
	} else {
		size = tx_fifo_space;
	}

	/* Not last data */
	while (size > 1) {
		data = buf[context->buf_index];
		i2c->data_cmd = data;
		size--;
		context->buf_index++;
	}

	/* Last data transfer  */
	data = buf[context->buf_index];

	/* If it is the last data for whole transfer */
	if (last_data) {
		/* NO need to send STOP, but need to change watermark
		 * to 0, this will have a interrupt while all data sent out
		 */
		if (context->pending) {
			i2c->tx_tl = 0;
		} else {
			data |= BIT_CMD_STOP;
			/* Disable tx empty interrupt */
			i2c->intr_mask &= ~BIT_INTR_TX_EMPTY;
		}
	}

	i2c->data_cmd = data;
	context->buf_index++;
}

static void i2c_isr_complete(sedi_i2c_t i2c_device, bool is_error)
{
	struct i2c_context *context = &contexts[i2c_device];
	i2c_dw_apb200a_regs_t *regs = (void *)context->base;
	uint32_t event, val;

	dw_i2c_irq_config(context->base, 0);

	dw_i2c_config_txfifo(context->base, 0);
	dw_i2c_config_rxfifo(context->base, 0);

	if (is_error) {
		event = dw_i2c_abort_analysis(context->base);
		/* Flush FIFO */
		dw_i2c_disable(context->base);
	} else {
		event = SEDI_I2C_EVENT_TRANSFER_DONE;
	}

	context->status.event = event;
	context->pending = 0;
	context->status.busy = 0;

	/* Clear all interrupt, this will clear abort source register*/
	val = regs->clr_intr;
	dw_i2c_clear_interrupt(context->base);

	if (context->cb_event) {
		context->cb_event(event);
	}
	PARAM_UNUSED(val);
}

void sedi_i2c_isr_handler(IN sedi_i2c_t i2c_device)
{
	struct i2c_context *context = &contexts[i2c_device];
	i2c_dw_apb200a_regs_t *regs = (void *)context->base;
	uint32_t stat, val;

	/* Return if enter into isr unexpectedly */
	if (context->status.busy == 0) {
		return;
	}

	/* Get interrupt status */
	stat = regs->intr_stat;

	/* If error happened, go to end*/
	if (stat & CONFIG_INTR_ERROR) {
		i2c_isr_complete(i2c_device, true);
		return;
	}

	/* check if there is a entity in rx fifo */
	if (stat & BIT_INTR_RX_FULL) {
		i2c_isr_recv(i2c_device);
		/* For receive with no STOP, while all data received, ended */
		if ((context->buf_index == context->buf_size) && (context->pending)) {
			i2c_isr_complete(i2c_device, false);
			return;
		}
	}

	/* For transfer need STOP flag, use STOP as transfer end condition */
	if ((stat & BIT_INTR_STOP_DET) && (context->pending == 0)) {
		/* Clear stop detect interrupt */
		val = regs->clr_stop_det;
		i2c_isr_complete(i2c_device, false);
		return;
	}
	/* For send with no STOP, while TX FIFO empty, ended */
	if ((context->pending) && (context->buf_index == context->buf_size) &&
	    (stat & BIT_INTR_TX_EMPTY)) {
		i2c_isr_complete(i2c_device, false);
		return;
	}

	/* TX empty condition */
	if (stat & BIT_INTR_TX_EMPTY) {
		/*If it is read operation, need to send cmd*/
		if (context->status.direction == 1) {
			i2c_isr_ask_data(i2c_device);
		} else {
			i2c_isr_send(i2c_device);
		}
	}
	PARAM_UNUSED(val);
}
