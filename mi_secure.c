#include "mi_secure.h"

#include <stdint.h>
#include <time.h>
#include "app_timer.h"
#include "pt.h"
#include "nrf_drv_twi_patched.h"
#include "nrf_gpio.h"
#include "nrf_queue.h"

#define NRF_LOG_MODULE_NAME "SCHD"
#include "nrf_log.h"
#include "nrf_log_ctrl.h"

#include "sha256_hkdf.h"
#include "ccm.h"
#include "mi_crypto.h"
#include "mi_error.h"
#include "mi_beacon.h"
#include "mi_psm.h"
#include "mi_config.h"
#include "ble_mi_secure.h"

#if defined(__CC_ARM)
  #pragma anon_unions
#elif defined(__ICCARM__)
  #pragma language=extended
#elif defined(__GNUC__)
  /* anonymous unions are enabled by default */
#endif



typedef struct {
    uint8_t  vid;
    uint16_t hw_ver;
    uint16_t sw_ver;
    uint16_t protocol_ver;
    uint8_t  cfg;
    uint8_t  reserve[2];
    uint16_t sn[16];
	uint8_t  pad[2];
} msc_info_t;

typedef struct {
	uint32_t expire_time;
	uint32_t reserved[3];
} shared_key_t;

#define PROTOCOL_VERSION   0x0201
#define PROFILE_PIN        25
#define PAIRCODE_NUMS      6

#define PRINT_MSC_INFO     0
#define PRINT_MAC          0
#define PRINT_DEV_PUBKEY   0
#define PRINT_SHA256       0
#define PRINT_SIGN         0
#define PRINT_LTMK         0

#define MSC_XFER(CMD, INPUT, INPUT_L, OUTPUT, OUTPUT_L)                         \
(msc_xfer_control_block_t) {    .cmd      = CMD,                                \
                                .p_para   = INPUT,                              \
                                .para_len = INPUT_L,                            \
                                .p_data   = OUTPUT,                             \
                                .data_len = OUTPUT_L }

#define SET_DATA_VAILD(x)        (x = 1)
#define SET_DATA_INVAILD(x)      (x = 0)
#define DATA_IS_VAILD_P(x)       (x == 1)
#define DATA_IS_INVAILD_P(x)     (x == 0)

#define  RTC_TIME_DRIFT  300

APP_TIMER_DEF(mi_schd_timer);

static struct {
	uint8_t msc_info   :1 ;
	uint8_t app_pub    :1 ;
	uint8_t dev_pub    :1 ;
	uint8_t eph_key    :1 ;
	uint8_t dev_sha    :1 ;
	uint8_t dev_sign   :1 ;
	uint8_t LTMK       :1 ;
	uint8_t session_key:1 ;

	uint8_t MKPK       :1 ;
	uint8_t dev_cert   :1 ;
	uint8_t manu_cert  :1 ;
	uint8_t encrypt_reg_data   :1 ;
	uint8_t encrypt_login_data :1 ;
	uint8_t encrypt_share_data :1 ;
	
} flags;

static uint8_t app_pub[64];
static uint8_t msc_info[12];
static uint8_t dev_pub[64];
static uint8_t dev_sha[32];
static uint8_t eph_key[32];
static uint8_t LTMK[32];
static session_ctx_t session_key;
static uint8_t dev_sign[64];

struct {
	uint8_t id;
	uint8_t cipher[32];
	uint8_t mic[4];
	uint8_t pad[3];
} MKPK;

struct {
	uint8_t cipher[64];
	uint8_t mic[4];
} encrypt_reg_data;

struct {
	union {
		uint8_t cipher[4];
		uint32_t crc32;
	};
	uint8_t mic[4];
} encrypt_login_data;

struct {
	uint8_t cipher[12+16];
	uint8_t mic[4];
} shared_info;

struct {
	uint8_t cipher[sizeof(shared_info)];
	uint8_t mic[4];
} encrypt_share_data;

session_ctx_t cloud_key;
static uint8_t nonce[12] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                         0x19, 0x1a, 0x1b};

msc_info_t tmp_info;

struct {
	uint8_t pt1 :1;
	uint8_t pt2 :1;
	uint8_t pt3 :1;
	uint8_t reserve: 5;
} pt_flags;

struct {
	uint16_t dev;
	uint16_t manu;
	uint16_t root;
} m_certs_len;

uint8_t dev_cert[512];
uint8_t manu_cert[512];

const uint8_t reg_salt[] = "smartcfg-setup-salt";
const uint8_t reg_info[] = "smartcfg-setup-info";
const uint8_t log_salt[] = "smartcfg-login-salt";
const uint8_t log_info[] = "smartcfg-login-info";
const uint8_t share_salt[] = "smartcfg-share-salt";
const uint8_t share_info[] = "smartcfg-share-info";
const uint8_t cloud_salt[] = "smartcfg-cloud-salt";
const uint8_t cloud_info[] = "smartcfg-cloud-info";
const uint8_t mk_salt[] = "smartcfg-masterkey-salt";
const uint8_t mk_info[] = "smartcfg-masterkey-info";

static uint32_t schd_time;
static uint32_t schd_stat;
static uint32_t schd_interval = 64;
static pt_t pt1, pt2, pt3, pt4;

/*** Pseduo timer ***/
typedef struct {
	int start;
	int interval;
} timer_t;

static void timer_set(timer_t * t, int interval_ms)
{ t->interval = (interval_ms << 5) / schd_interval; t->start = schd_time; }

static int timer_expired(timer_t * t, void (*handler)(void))
{
	int expired = (schd_time - t->start) >= (int)t->interval;
	if (expired == true && handler != NULL)
		handler();
	return expired;
}

/*** Ring buffer ***/
typedef struct {
	uint8_t * buf;
	uint8_t mask;
	uint8_t rd_ptr;
	uint8_t wr_ptr;
} queue_t;

int queue_init(queue_t *q, uint8_t *buf, uint8_t size)
{
	if (buf == NULL || q == NULL)
		return MI_ERROR_INVALID_PARAM;

	if (!IS_POWER_OF_TWO(size))
		return MI_ERROR_DATA_SIZE;

	q->buf = buf;
	q->mask = size - 1;
	q->rd_ptr = 0;
	q->wr_ptr = 0;
	return 0;
}

int enqueue(queue_t *q, uint8_t in)
{
	if (((q->wr_ptr - q->rd_ptr) & q->mask) == q->mask) {
		return MI_ERROR_NO_MEM;
	}
	
	q->buf[q->wr_ptr++] = in;
	q->wr_ptr &= q->mask;
	
	return 0;
}

int dequeue(queue_t *q, uint8_t *out)
{
	if (((q->wr_ptr - q->rd_ptr) & q->mask) > 0) {
		*out = q->buf[q->rd_ptr++];
		q->rd_ptr &= q->mask;
		return 0;
	} else
		return MI_ERROR_NOT_FOUND;
}

extern fast_xfer_t fast_control_block;
extern reliable_xfer_t rxfer_control_block;

static void mi_scheduler(void * p_context);
static void sys_procedure(uint32_t type);
static void reg_procedure(void);
static void admin_login_procedure(void);
static void shared_login_procedure(void);
static int monitor(pt_t *pt);

static uint32_t key_id;
static mi_author_stat_t mi_authorization_status;
static mi_schd_event_handler_t m_user_event_handler;
static mi_kbd_input_get_t m_pair_code_get;
static uint8_t m_is_registered;

static __ALIGN(4) struct {
	uint8_t did[8];
	uint8_t beacon_key[16];
	uint8_t cloud_key[16];

#ifdef ENC_LTMK
	uint8_t rand_key[16];
#endif

} mi_sysinfo;

uint32_t get_mi_reg_stat(void)
{
	return m_is_registered;
}

static void set_mi_reg_stat(uint8_t stat)
{
	m_is_registered = stat != 0 ? 1 : 0;
}

uint32_t get_mi_key_id(void)
{
	return key_id;
}
void set_mi_authorization(mi_author_stat_t status)
{
	mi_authorization_status = status;
}

uint32_t get_mi_authorization(void)
{
	return mi_authorization_status;
}


uint32_t mi_scheduler_init(uint32_t interval, mi_schd_event_handler_t handler, mi_kbd_input_get_t recorder)
 {
	int32_t errno;
	schd_interval = interval;
	errno = app_timer_create(&mi_schd_timer, APP_TIMER_MODE_REPEATED, mi_scheduler);
	APP_ERROR_CHECK(errno);

	if (handler != NULL)
		m_user_event_handler = handler;

	if (recorder != NULL)
		m_pair_code_get = recorder;
	
	nrf_gpio_cfg_output(MJSC_POWER_PIN);
	nrf_gpio_pin_clear(MJSC_POWER_PIN);

	nrf_gpio_cfg_output(PROFILE_PIN);
	nrf_gpio_pin_clear(PROFILE_PIN);
	return 0;
}

uint32_t mi_scheduler_start(uint32_t auth_stat)
{
	int32_t errno;

	if (schd_stat == 0) {
		schd_stat = auth_stat;
		schd_time = 0;
	} else
		return -1;

	PT_INIT(&pt1);
	PT_INIT(&pt2);
	PT_INIT(&pt3);
	PT_INIT(&pt4);

	memset((char*)&flags, 0, sizeof(flags));
	memset((char*)&pt_flags, 0xFF, sizeof(pt_flags));
	memset(&rxfer_control_block, 0, sizeof(rxfer_control_block));
	rxfer_control_block.state = RXFER_WAIT_CMD;

	NRF_LOG_WARNING(" START %X\n\n", schd_stat);

	mi_scheduler(&schd_stat);
	app_timer_stop(mi_schd_timer);
	errno = app_timer_start(mi_schd_timer, schd_interval, &schd_stat);
	APP_ERROR_CHECK(errno);

	return errno;
}

static uint32_t mi_scheduler_stop(int type)
{
	int32_t errno;
	errno = app_timer_stop(mi_schd_timer);
	APP_ERROR_CHECK(errno);
	return errno;
}

#ifdef M_TEST
void aes_ecb_test();
void aes_ccm_test();
void aes_ccm_test2();
static uint8_t msg[20] = "helloworld";
static uint8_t cipher[20] = {0};
static uint8_t plain[20] = {0};
uint8_t test_str[] = {12, 1,2,3,4,5,6,7,8,9,0,1,2,
					   1, 0,
					  16, 0,'x',0xD,0xE,0xA,0xD,0xB,0xE,0xE,0xF,'a','b','c','d','e',30,
                      16, 1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6};

int test_thd(pt_t *pt)
{
	PT_BEGIN(pt);
	static int i;
	static uint16_t len;
	session_ctx_t key = {0};
	memcpy(key.app_key, "DUMMY KEY", 10);
	memcpy(key.dev_key, "DUMMY KEY", 10);
	memcpy(key.dev_iv , "DUMMY KEY", 4);
	memcpy(key.app_iv , "DUMMY KEY", 4);
	mi_crypto_init(&key);

	mi_session_encrypt(msg, 10, msg);
	NRF_LOG_RAW_HEXDUMP_INFO(msg, 16);

	mi_session_decrypt(msg, 10+6, cipher);
	NRF_LOG_RAW_HEXDUMP_INFO(cipher, 16);

	i = 1000;
	nrf_gpio_pin_set(PROFILE_PIN);
	while(i--)
	aes_ccm_test();
	nrf_gpio_pin_clear(PROFILE_PIN);
	
	i = 1000;
	nrf_gpio_pin_set(PROFILE_PIN);
	while(i--)
	aes_ccm_test2();
	nrf_gpio_pin_clear(PROFILE_PIN);

	i = 1;
	nrf_gpio_pin_set(PROFILE_PIN);
	while(i--)
	sha256_hkdf(   test_str,          32,
		  (void *)share_salt,         sizeof(share_salt)-1,
	      (void *)share_info,         sizeof(share_info)-1,
	    (void *)&session_key,         64);
	nrf_gpio_pin_clear(PROFILE_PIN);
	msc_control_block = MSC_XFER(MSC_AESCCM_ENC, test_str, sizeof(test_str), cipher, 20);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	NRF_LOG_HEXDUMP_INFO(cipher, 20);

	PT_WAIT_UNTIL(pt, 0);
	PT_END(pt);
}
#endif

static void mi_scheduler(void * p_context)
{
	schd_time++;
	uint32_t proc_type = *(uint32_t*)p_context;
	
#ifdef M_TEST

//	fast_xfer_test(&pt1);
//	reliable_xfer_test(&pt2);
	test_thd(&pt3);

#else
	
	nrf_gpio_pin_set(PROFILE_PIN);
	
	switch (proc_type & 0xF0UL) {
	case SYS_TYPE:
		sys_procedure(proc_type);
		break;

	case REG_TYPE:
		reg_procedure();
		break;

	case LOG_TYPE:
		admin_login_procedure();
		break;

	case SHARED_TYPE:
		shared_login_procedure();
		break;
	}
	
	monitor(&pt4);

	nrf_gpio_pin_clear(PROFILE_PIN);

#endif
}

static uint16_t find_lost_sn(reliable_xfer_t *pxfer)
{
	static uint16_t checked_sn = 1;
	uint8_t (*p_pkt)[18] = (void*)pxfer->pdata;

	p_pkt += checked_sn - 1;
	// <!> vulnerable check : word-read unaligned address may cause mem-hardfault
	// TODO:  refine the big data transfer protocol
	//        or just think about what if we lost the last packet ?
	while ( checked_sn <= pxfer->rx_num && ((uint16_t*)p_pkt)[0] != 0 ) {
		checked_sn++;
		p_pkt++;
	}

	if (checked_sn > pxfer->rx_num) {
		checked_sn = 1;
		return 0;
	}
	else {
		return checked_sn;
	}
}

pt_t pt_resend;
static int pthd_resend(pt_t *pt, reliable_xfer_t *pxfer)
{
	PT_BEGIN(pt);

	static uint16_t sn;

	while(1) {
		sn = find_lost_sn(pxfer);
		if (sn == 0) {
			PT_WAIT_UNTIL(pt, reliable_xfer_ack(A_SUCCESS) == NRF_SUCCESS);
			PT_EXIT(pt);
		}
		else {
			NRF_LOG_ERROR("lost packet %d.\n", sn);
			PT_WAIT_UNTIL(pt, reliable_xfer_ack(A_LOST, sn) == NRF_SUCCESS);
			PT_WAIT_UNTIL(pt, pxfer->curr_sn == sn);
		}
	}

	PT_END(pt);
}

pt_t pt_send;
static int pthd_send(pt_t *pt, reliable_xfer_t *pxfer)
{
	PT_BEGIN(pt);

	static uint16_t sn;
	sn = 1;

	while(sn <= pxfer->tx_num) {
		PT_WAIT_UNTIL(pt, reliable_xfer_data(pxfer, sn) == NRF_SUCCESS);
		sn++;
	}

	pxfer->state = RXFER_WAIT_ACK;

	while(pxfer->mode == MODE_ACK && pxfer->ack != A_SUCCESS) {
		PT_WAIT_UNTIL(pt, pxfer->curr_sn != 0 || pxfer->ack == A_SUCCESS);
		if (pxfer->ack == A_SUCCESS) {
			break;
		}
		else if(pxfer->curr_sn <= pxfer->tx_num) {
			PT_WAIT_UNTIL(pt, reliable_xfer_data(pxfer, pxfer->curr_sn) == NRF_SUCCESS);
		}
		pxfer->curr_sn = 0;
	}
	PT_END(pt);
}

static int format_rx_cb(reliable_xfer_t *pxfer, void *p_rxd, uint16_t rxd_bytes)
{
	uint8_t last_bytes = rxd_bytes % 18;
	pxfer->pdata = p_rxd;
	pxfer->max_rx_num = CEIL_DIV(rxd_bytes, 18);
	pxfer->last_bytes  = last_bytes == 0 ? 18 : last_bytes;
	return 0;
}

static int format_tx_cb(reliable_xfer_t *pxfer, void *p_txd, uint16_t txd_bytes)
{
	uint8_t last_bytes = txd_bytes % 18;
	pxfer->pdata = p_txd;
	pxfer->tx_num = CEIL_DIV(txd_bytes, 18);
	pxfer->last_bytes  = last_bytes == 0 ? 18 : last_bytes;
	return 0;
}


static pt_t pt_r_rx_thd;
static int rxfer_rx_thd(pt_t *pt, reliable_xfer_t *pxfer, uint8_t data_type)
{
	static uint8_t retries_num;
	static timer_t timeout_timer;

	PT_BEGIN(pt);

	/* Recive data */
	PT_WAIT_UNTIL(pt, pxfer->rx_num != 0 && pxfer->cmd == data_type);
	if (pxfer->rx_num <= pxfer->max_rx_num && pxfer->pdata != NULL) {
		PT_WAIT_UNTIL(pt, reliable_xfer_ack(A_READY) == NRF_SUCCESS);
/*
		for (retries_num = 0; retries_num < 3; retries_num++) {
			if (reliable_xfer_ack(A_READY) == NRF_SUCCESS)
				break;
			else
				PT_YIELD(pt);
		}
		if (retries_num == 3) PT_EXIT(pt);
 */	
		pxfer->state = RXFER_RXD;
	} else {
		PT_WAIT_UNTIL(pt, reliable_xfer_ack(A_CANCEL) == NRF_SUCCESS);
		pxfer->rx_num = 0;
		PT_RESTART(pt);
	}
	timer_set(&timeout_timer, 2000);
	PT_WAIT_UNTIL(pt, pxfer->rx_num == pxfer->curr_sn || timer_expired(&timeout_timer, NULL));

	PT_SPAWN(pt, &pt_resend, pthd_resend(&pt_resend, pxfer));

	pxfer->state = RXFER_WAIT_CMD;
	pxfer->rx_num = 0;
	pxfer->pdata  = 0;

	PT_END(pt);
}

static pt_t pt_r_tx_thd;
static int rxfer_tx_thd(pt_t *pt, reliable_xfer_t *pxfer, uint8_t data_type)
{
	PT_BEGIN(pt);

	/* Send data */
	PT_WAIT_UNTIL(pt, reliable_xfer_cmd(data_type, pxfer->tx_num) == NRF_SUCCESS);

	pxfer->state = RXFER_WAIT_ACK;
	pxfer->mode  = -1;
	PT_WAIT_UNTIL(pt, pxfer->mode == MODE_ACK);

	if (pxfer->ack != A_READY)
		PT_RESTART(pt);

	PT_SPAWN(pt, &pt_send, pthd_send(&pt_send, pxfer));

	pxfer->state = RXFER_WAIT_CMD;
	pxfer->tx_num = 0;
	PT_END(pt);
}

typedef enum {
	// Info
	MSC_INFO       = 0x01,
	MSC_ID,

	// Sign
	MSC_SIGN       = 0x10,
	MSC_VERIFY     = 0x11,

	MSC_ECDHE      = 0x14,

	MSC_DEV_CERT   = 0x20,
	MSC_MANU_CERT,
	MSC_ROOT_CERT,
	MSC_CERTS_LEN  = 0x28,

	MSC_PUBKEY     = 0x3B,
	
	MSC_WR_MKPK    = 0x40,
	MSC_RD_MKPK,
	MSC_ERASE,
	
	MSC_RANDOM     = 0x50,
	MSC_STATUS     = 0x52,

	MSC_AESCCM_ENC = 0x60,
	MSC_AESCCM_DEC,
} msc_cmd_t;

typedef struct {
	msc_cmd_t     cmd;
	uint16_t para_len;
	uint16_t data_len;
	uint8_t   *p_para;
	uint8_t   *p_data;
	uint8_t    status;
} msc_xfer_control_block_t;

#define MSC_ADDR   0x2A
#define MSC_SCL    28

#define MSC_POWER_ON()  do { msc_power_on(MJSC_POWER_PIN);PT_YIELD(pt);} while(0)
#define MSC_POWER_OFF()  do { msc_power_off(MJSC_POWER_PIN);} while(0)

extern volatile bool m_twi0_xfer_done;
extern const nrf_drv_twi_t TWI0;
static nrf_drv_twi_xfer_desc_t twi0_xfer;
static uint8_t twi_buf[515];
msc_xfer_control_block_t msc_control_block;

static void msc_power_on(uint32_t pin)
{
	if (pin != 0xFFFFFFFF)
		nrf_gpio_pin_set(pin);
	
	
}

static void msc_power_off(uint32_t pin)
{
	if (pin != 0xFFFFFFFF)
		nrf_gpio_pin_clear(pin);
}

static uint8_t calc_data_xor(uint8_t *pdata, uint16_t len)
{
	uint8_t chk = 0;
	while(len--)
		chk ^= *pdata++;
	return chk;
}

static int msc_encode_twi_buf(msc_xfer_control_block_t *p_cb)
{
	uint16_t para_len = p_cb->p_para == NULL ? 0 : p_cb->para_len;
	uint16_t cmd_len  = para_len + 1;

	if (cmd_len > 512) {
		NRF_LOG_ERROR("MSC para len error.\n");
		return 1;
	}
	
	twi_buf[0] = cmd_len >> 8;
	twi_buf[1] = cmd_len & 0xFF;
	twi_buf[2] = p_cb->cmd;

	memcpy(twi_buf+3, p_cb->p_para, para_len);
	
	twi_buf[3+para_len] = calc_data_xor(twi_buf, para_len + 3);
	
	return 0;
} 

static int msc_decode_twi_buf(msc_xfer_control_block_t *p_cb)
{
	uint16_t len = (twi_buf[0]<<8) | twi_buf[1];        // contain data + status
	uint16_t data_len = len - sizeof(p_cb->status);
	
		
	if(data_len != p_cb->data_len) {
		NRF_LOG_ERROR("MSC return data len error.\n");
		return 1;
	}

	uint8_t  chk = calc_data_xor(twi_buf, 2+len);

	if (chk != twi_buf[2+len]) {
		p_cb->status = 255;
		return 2;
	}

    if (p_cb->status == 0x0f) {
		NRF_LOG_ERROR("MSC received invaild packet.\n");
        return 3;
    }

	p_cb->status = twi_buf[2+data_len];

	if (p_cb->p_data != NULL)
		memcpy(p_cb->p_data, twi_buf+2, data_len);
	
	return 0;
}

static pt_t pt_msc_thd;
int msc_thread(pt_t *pt, msc_xfer_control_block_t *p_cb)
{
	uint32_t err_code;

	PT_BEGIN(pt);	
	static uint8_t  retry_times = 0;
	msc_encode_twi_buf(p_cb);
	NRF_LOG_INFO("Start MSC cmd 0x%02X @ schd_time %d\n", p_cb->cmd, schd_time);
	/* 4 = 2bytes lengh + 1byte cmd + 1byte chk  */
	twi0_xfer = (nrf_drv_twi_xfer_desc_t)NRF_DRV_TWI_XFER_DESC_TX(MSC_ADDR, twi_buf, p_cb->para_len+4);
	m_twi0_xfer_done = false;
	err_code = nrf_drv_twi_xfer(&TWI0, &twi0_xfer, 0);
	APP_ERROR_CHECK(err_code);
	PT_WAIT_UNTIL(pt, m_twi0_xfer_done);

	NRF_LOG_INFO("Waiting...  @ schd_time %d\n", schd_time);
	PT_WAIT_UNTIL(pt, nrf_gpio_pin_read(MSC_SCL));
	NRF_LOG_INFO("Ready now.  @ schd_time %d\n", schd_time);
	
	/* 4 = 2bytes lengh + 1byte status + 1byte chk  */
	twi0_xfer = (nrf_drv_twi_xfer_desc_t)NRF_DRV_TWI_XFER_DESC_RX(MSC_ADDR, twi_buf, p_cb->data_len+4);
	m_twi0_xfer_done = false;
	err_code = nrf_drv_twi_xfer(&TWI0, &twi0_xfer, 0);
	APP_ERROR_CHECK(err_code);
	PT_WAIT_UNTIL(pt, m_twi0_xfer_done);
	
	msc_decode_twi_buf(p_cb);
	if (p_cb->status != 0 ) {
		if (retry_times < 5) {
			retry_times++;
			NRF_LOG_ERROR("CMD 0x%02X Error 0x%02X\n RETRY...\n", p_cb->cmd, p_cb->status);
			PT_RESTART(pt);
		} else {
			retry_times = 0;
			NRF_LOG_ERROR("Cann't run MSC CMD 0x%02X\n", p_cb->cmd);
			/* Blocking here, 
			   TODO: add error status handler    */
			PT_WAIT_UNTIL(pt, 0);
		}
		
	}

	NRF_LOG_INFO("Finish MSC cmd 0x%02X @ schd_time %d\n\n", p_cb->cmd, schd_time);
	p_cb->cmd = NULL;

	PT_END(pt);
}

static void schd_evt_handler(schd_evt_t evt_id)
{
	switch (evt_id) {
	case SCHD_EVT_REG_SUCCESS:
		break;

	case SCHD_EVT_REG_FAILED:
		break;

	case SCHD_EVT_ADMIN_LOGIN_SUCCESS:
		break;

	case SCHD_EVT_ADMIN_LOGIN_FAILED:
		break;

	case SCHD_EVT_SHARE_LOGIN_SUCCESS:
		break;

	case SCHD_EVT_SHARE_LOGIN_FAILED:
		break;

	case SCHD_EVT_TIMEOUT:
		break;

	case SCHD_EVT_KEY_NOT_FOUND:
		break;

	case SCHD_EVT_KEY_FOUND:
		break;

	case SCHD_EVT_KEY_DEL_FAIL:
		break;

	case SCHD_EVT_KEY_DEL_SUCC:
		break;
	}

	schd_stat = 0;
	mi_scheduler_stop(0);

	if (m_user_event_handler != NULL)
		m_user_event_handler(evt_id);
}

static queue_t schd_evt_queue;
static uint8_t evt_buf[8];
static int monitor(pt_t *pt)
{
	static timer_t proc_timeout;
	uint8_t schd_evt;
	uint8_t errno;

	PT_BEGIN(pt);
	// All procedure timeout times are 20 seconds
	timer_set(&proc_timeout, 20000);
	queue_init(&schd_evt_queue, evt_buf, sizeof(evt_buf));

	while(1) {
		if (timer_expired(&proc_timeout, NULL) == true) {
			enqueue(&schd_evt_queue, SCHD_EVT_TIMEOUT);
		}

		// process event
		do {
			schd_evt = 0;
			errno = dequeue(&schd_evt_queue, &schd_evt);
			if (errno != MI_ERROR_NOT_FOUND)
				schd_evt_handler(schd_evt);
		} while(errno != MI_ERROR_NOT_FOUND); 

		PT_YIELD(pt);
	}

	PT_END(pt);
}

static int psm_restore(pt_t *pt)
{
	uint8_t errno;

	PT_BEGIN(pt);
	
	errno = mi_psm_record_read(0xBEEF, (uint8_t*)&mi_sysinfo, sizeof(mi_sysinfo));
	if (errno == MI_ERROR_NOT_FOUND) {
		set_mi_reg_stat(false);
		PT_YIELD(pt);
		enqueue(&schd_evt_queue, SCHD_EVT_KEY_NOT_FOUND);
		PT_EXIT(pt);
	} else {
		set_mi_reg_stat(true);
		NRF_LOG_RAW_INFO("Found the KEYINFO.\n");
	}
	
	set_beacon_key(mi_sysinfo.beacon_key);

	MSC_POWER_ON();
#if ENC_LTMK
	MKPK.id = 0;
	msc_control_block = MSC_XFER(MSC_RD_MKPK, &MKPK.id, 1, (uint8_t*)MKPK.cipher, 32+4);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));

	errno = 
	aes_ccm_auth_decrypt(mi_sysinfo.rand_key,
	                               nonce,  12,
	                                NULL,  0,
	                         MKPK.cipher,  32,
	                                LTMK,
	                            MKPK.mic,  4);
	APP_ERROR_CHECK(errno);
#else
	MKPK.id = 1;
	msc_control_block = MSC_XFER(MSC_RD_MKPK, &MKPK.id, 1, (uint8_t*)LTMK, 32);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
#endif
	MSC_POWER_OFF();

	enqueue(&schd_evt_queue, SCHD_EVT_KEY_FOUND);
	PT_END(pt);
}

static int psm_delete(pt_t *pt)
{
	uint8_t errno;

	PT_BEGIN(pt);

	set_mi_reg_stat(false);
	errno = mi_psm_reset();
	if (errno == MI_SUCCESS) {
		NRF_LOG_RAW_INFO("KEYINFO has been deleted! \n" );
		PT_YIELD(pt);
		enqueue(&schd_evt_queue, SCHD_EVT_KEY_DEL_SUCC);
	} else {
		NRF_LOG_RAW_INFO("KEYINFO deleted failed! errno: %d\n", errno);
		PT_YIELD(pt);
		enqueue(&schd_evt_queue, SCHD_EVT_KEY_DEL_FAIL);
	}
	
	PT_END(pt);
}

static int err_thd(pt_t *pt, uint32_t errno)
{
	PT_BEGIN(pt);

	PT_WAIT_UNTIL(pt, opcode_send(errno) == MI_SUCCESS);
	PT_YIELD(pt);
	enqueue(&schd_evt_queue, errno == ERR_REGISTERED ? SCHD_EVT_KEY_FOUND : SCHD_EVT_KEY_NOT_FOUND);

	PT_END(pt);
}

static void sys_procedure(uint32_t type)
{
	switch (type) {
	case SYS_KEY_RESTORE:
		if (pt_flags.pt1 == 1)
			pt_flags.pt1 = PT_SCHEDULE(psm_restore(&pt1));
		break;

	case SYS_KEY_DELETE:
		if (pt_flags.pt1 == 1)
			pt_flags.pt1 = PT_SCHEDULE(psm_delete(&pt1));
		break;
		
	}
}

static int reg_msc(pt_t *pt)
{
	PT_BEGIN(pt);
	MSC_POWER_ON();

	msc_control_block = MSC_XFER(MSC_PUBKEY, NULL, 0, dev_pub, 64);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	SET_DATA_VAILD(flags.dev_pub);

	msc_control_block = MSC_XFER(MSC_INFO, NULL, 0, (void*)&tmp_info, 26);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	tmp_info.protocol_ver = PROTOCOL_VERSION;
	memcpy(msc_info+8, (uint8_t*)&tmp_info.sw_ver, 4);
	msc_info[0] = 1;
	SET_DATA_VAILD(flags.msc_info);

	msc_control_block = MSC_XFER(MSC_CERTS_LEN, NULL, 0, (void*)&m_certs_len, sizeof(m_certs_len));
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));

	m_certs_len.dev  = __REV16(m_certs_len.dev);
	m_certs_len.manu = __REV16(m_certs_len.manu);
	
	msc_control_block = MSC_XFER(MSC_DEV_CERT, NULL, 0, dev_cert, m_certs_len.dev);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	SET_DATA_VAILD(flags.dev_cert);

	msc_control_block = MSC_XFER(MSC_MANU_CERT, NULL, 0, manu_cert, m_certs_len.manu);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	SET_DATA_VAILD(flags.manu_cert);

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.app_pub));
	msc_control_block = MSC_XFER(MSC_ECDHE, app_pub, 64, eph_key, 32);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	SET_DATA_VAILD(flags.eph_key);

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.dev_sha));
	msc_control_block = MSC_XFER(MSC_SIGN, dev_sha, 32, dev_sign, 64);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	SET_DATA_VAILD(flags.dev_sign);

	MSC_POWER_OFF();
#if (PRINT_SIGN == 1)
	NRF_LOG_HEXDUMP_INFO(dev_sign, 64);
#endif	
	
	PT_END(pt);
}

static uint8_t certs_is_sent = 0;

static int reg_ble(pt_t *pt)
{
	PT_BEGIN(pt);

	certs_is_sent = 0;

	format_rx_cb(&rxfer_control_block, app_pub, sizeof(app_pub));
	PT_SPAWN(pt, &pt_r_rx_thd, rxfer_rx_thd(&pt_r_rx_thd, &rxfer_control_block, DEV_PUBKEY));
	SET_DATA_VAILD(flags.app_pub);
	NRF_LOG_INFO(NRF_LOG_COLOR_CODE_BLUE"app_pub recived ""@ schd_time %d\n", schd_time);
	
	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.msc_info));

	format_tx_cb(&rxfer_control_block, msc_info, sizeof(msc_info) + sizeof(dev_pub));
	PT_SPAWN(pt, &pt_r_tx_thd, rxfer_tx_thd(&pt_r_tx_thd, &rxfer_control_block, DEV_PUBKEY));
	NRF_LOG_INFO(NRF_LOG_COLOR_CODE_BLUE"dev_pub sent ""@ schd_time %d\n", schd_time);

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.dev_cert));
	format_tx_cb(&rxfer_control_block, dev_cert, m_certs_len.dev);
	PT_SPAWN(pt, &pt_r_tx_thd, rxfer_tx_thd(&pt_r_tx_thd, &rxfer_control_block, DEV_CERT));
	NRF_LOG_INFO(NRF_LOG_COLOR_CODE_BLUE"dev_cert sent ""@ schd_time %d\n", schd_time);
	
	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.manu_cert));
	format_tx_cb(&rxfer_control_block, manu_cert, m_certs_len.manu);
	PT_SPAWN(pt, &pt_r_tx_thd, rxfer_tx_thd(&pt_r_tx_thd, &rxfer_control_block, DEV_MANU_CERT));
	NRF_LOG_INFO(NRF_LOG_COLOR_CODE_BLUE"manu_cert sent ""@ schd_time %d\n", schd_time);
	
	certs_is_sent = 1;

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.dev_sign));
	format_tx_cb(&rxfer_control_block, &dev_sign, sizeof(dev_sign));
	PT_SPAWN(pt, &pt_r_tx_thd, rxfer_tx_thd(&pt_r_tx_thd, &rxfer_control_block, DEV_SIGNATURE));
	NRF_LOG_INFO(NRF_LOG_COLOR_CODE_BLUE"encrypt_reg_data sent ""@ schd_time %d\n", schd_time);

	PT_END(pt);
}

static int reg_auth(pt_t *pt)
{
	static uint8_t pair_code[PAIRCODE_NUMS];
	static uint8_t pair_code_num;
	PT_BEGIN(pt);
	
	PT_WAIT_UNTIL(pt, certs_is_sent && DATA_IS_VAILD_P(flags.eph_key));

	/* Flush the input buffer */
	uint8_t tmp[16];
	while (m_pair_code_get(tmp, 16));

	NRF_LOG_RAW_INFO(NRF_LOG_COLOR_CODE_GREEN"Please input your pair code ( MUST be 6 digits ) : \n");

	pair_code_num = 0;
	while (pair_code_num < PAIRCODE_NUMS) {
		pair_code_num += m_pair_code_get(pair_code + pair_code_num, PAIRCODE_NUMS - pair_code_num);
		PT_YIELD(pt);
	}

	sha256_hkdf(     eph_key,         sizeof(eph_key),
	               pair_code,         sizeof(pair_code),
	        (void *)reg_info,         sizeof(reg_info)-1,
	                    LTMK,         32);
	
	mbedtls_sha256_context sha256_ctx;
	mbedtls_sha256_init(&sha256_ctx);
	mbedtls_sha256_starts(&sha256_ctx, 0 );
	mbedtls_sha256_update(&sha256_ctx, LTMK, 32);
	mbedtls_sha256_finish(&sha256_ctx, dev_sha);
	SET_DATA_VAILD(flags.dev_sha);

#if (PRINT_MSC_INFO   == 1)
	NRF_LOG_RAW_INFO("\nMSC info\t");
	NRF_LOG_RAW_HEXDUMP_INFO(msc_info, 12);
#endif
#if (PRINT_DEV_PUBKEY == 1)
	NRF_LOG_RAW_INFO("DEV_PUBKEY\t");
	NRF_LOG_HEXDUMP_INFO(dev_pub, 16);
#endif
#if (PRINT_SHA256     == 1)
	NRF_LOG_RAW_INFO("SHA256\t");
	NRF_LOG_HEXDUMP_INFO(dev_sha, 32);
#endif

	PT_WAIT_UNTIL(pt, opcode_recv() != REG_START);
	if (opcode_recv() != REG_VERIFY_SUCC) {
		NRF_LOG_ERROR("Authenticated failed.\n");
		PT_WAIT_UNTIL(pt, opcode_send(REG_FAILED) == NRF_SUCCESS);
		enqueue(&schd_evt_queue, SCHD_EVT_REG_FAILED);
		PT_EXIT(pt);
	}

#if PRINT_LTMK
	NRF_LOG_RAW_HEXDUMP_INFO(LTMK, 32);
	PT_YIELD(pt);
#endif

	MSC_POWER_ON();

#if ENC_LTMK
	PT_WAIT_UNTIL(pt, sd_rand_application_vector_get(mi_sysinfo.rand_key, 16) == NRF_SUCCESS);

	MKPK.id = 0;
	aes_ccm_encrypt_and_tag(mi_sysinfo.rand_key,
			                  nonce, 12,
							   NULL, 0,
					           LTMK, 32,
					    MKPK.cipher,
					       MKPK.mic, 4);
	msc_control_block = MSC_XFER(MSC_WR_MKPK, (void*)&MKPK, 1+32+4, NULL, 0);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
#else
	MKPK.id = 1;
	memcpy(MKPK.cipher, LTMK, 32);
	msc_control_block = MSC_XFER(MSC_WR_MKPK, (void*)&MKPK, 1+32, NULL, 0);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
#endif
	MSC_POWER_OFF();

	sha256_hkdf(        LTMK,         sizeof(LTMK),
		  (void *)cloud_salt,         sizeof(cloud_salt)-1,
		  (void *)cloud_info,         sizeof(cloud_info)-1,
		  (void *)&cloud_key,         sizeof(cloud_key));
	
	memcpy(mi_sysinfo.did,        msc_info, 8);
	memcpy(mi_sysinfo.beacon_key, cloud_key.app_key, 16);
	memcpy(mi_sysinfo.cloud_key,  cloud_key.dev_key, 16);
	
	uint8_t errno = mi_psm_record_write(0xBEEF, (uint8_t*)&mi_sysinfo, sizeof(mi_sysinfo));
	if (errno != MI_SUCCESS) {
		PT_WAIT_UNTIL(pt, opcode_send(REG_FAILED) == NRF_SUCCESS);
		NRF_LOG_RAW_INFO("KEYINFO STORE FAILED: %d\n", errno);
		enqueue(&schd_evt_queue, SCHD_EVT_REG_FAILED);
		
	 } else {
		set_mi_reg_stat(true);
		set_beacon_key(mi_sysinfo.beacon_key);
		PT_WAIT_UNTIL(pt, opcode_send(REG_SUCCESS) == NRF_SUCCESS);
		NRF_LOG_RAW_INFO("REG SUCCESS: %d\n", schd_time);
		enqueue(&schd_evt_queue, SCHD_EVT_REG_SUCCESS);
	}

	PT_END(pt);
}

static void reg_procedure()
{
	if (m_is_registered == true) {
		if (pt_flags.pt1 == 1)
			pt_flags.pt1 = PT_SCHEDULE(err_thd(&pt1, ERR_REGISTERED));

		return;
	}

	if (pt_flags.pt1 == 1)
		pt_flags.pt1 = PT_SCHEDULE(reg_msc(&pt1));

	if (pt_flags.pt2 == 1)
		pt_flags.pt2 = PT_SCHEDULE(reg_ble(&pt2));

	if (pt_flags.pt3 == 1)
		pt_flags.pt3 = PT_SCHEDULE(reg_auth(&pt3));
	
}


static int admin_msc(pt_t *pt)
{
	PT_BEGIN(pt);

	MSC_POWER_ON();
	msc_control_block = MSC_XFER(MSC_PUBKEY, NULL, 0, dev_pub, 64);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	SET_DATA_VAILD(flags.dev_pub);

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.app_pub));

	msc_control_block = MSC_XFER(MSC_ECDHE, app_pub, 64, eph_key, 32);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	SET_DATA_VAILD(flags.eph_key);
	MSC_POWER_OFF();

	PT_END(pt);
}

static int admin_ble(pt_t *pt)
{
	PT_BEGIN(pt);
	
	format_rx_cb(&rxfer_control_block, app_pub, sizeof(app_pub));
	PT_SPAWN(pt, &pt_r_rx_thd, rxfer_rx_thd(&pt_r_rx_thd, &rxfer_control_block, DEV_PUBKEY));
	SET_DATA_VAILD(flags.app_pub);

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.dev_pub));
	format_tx_cb(&rxfer_control_block, dev_pub, sizeof(dev_pub));
	PT_SPAWN(pt, &pt_r_tx_thd, rxfer_tx_thd(&pt_r_tx_thd, &rxfer_control_block, DEV_PUBKEY));

	format_rx_cb(&rxfer_control_block, &encrypt_login_data, sizeof(encrypt_login_data));
	PT_SPAWN(pt, &pt_r_rx_thd, rxfer_rx_thd(&pt_r_rx_thd, &rxfer_control_block, DEV_LOGIN_INFO));
	SET_DATA_VAILD(flags.encrypt_login_data);

	PT_END(pt);
}

static int admin_auth(pt_t *pt)
{
	uint32_t errno;
	uint32_t crc32;
	PT_BEGIN(pt);

#if PRINT_LTMK
	NRF_LOG_RAW_HEXDUMP_INFO(LTMK, 32);
	PT_YIELD(pt);
#endif
	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.eph_key));
	
	sha256_hkdf(     eph_key,         sizeof(eph_key) + sizeof(LTMK),
	        (void *)log_salt,         sizeof(log_salt)-1,
	        (void *)log_info,         sizeof(log_info)-1,
	    (void *)&session_key,         sizeof(session_key));

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.encrypt_login_data));

	errno = 
	aes_ccm_auth_decrypt(session_key.app_key,
	                               nonce,  sizeof(nonce),
	                                NULL,  0,
	           encrypt_login_data.cipher,  sizeof(encrypt_login_data.cipher),
	    (void*)&encrypt_login_data.crc32,
	              encrypt_login_data.mic,  4);

	crc32 = soft_crc32(dev_pub, sizeof(dev_pub), 0);

  	if (crc32 == encrypt_login_data.crc32) {
		NRF_LOG_INFO("ADMIN LOG SUCCESS: %d\n", schd_time);
		key_id = 0;
		mi_crypto_init(&session_key);
		set_mi_authorization(OWNER_AUTHORIZATION);
		PT_WAIT_UNTIL(pt, opcode_send(LOG_SUCCESS) == NRF_SUCCESS);
		enqueue(&schd_evt_queue, SCHD_EVT_ADMIN_LOGIN_SUCCESS);
	} else {
		NRF_LOG_ERROR("ADMIN LOG FAILED. %d\n", errno);
		PT_WAIT_UNTIL(pt, opcode_send(LOG_FAILED) == NRF_SUCCESS);
		enqueue(&schd_evt_queue, SCHD_EVT_ADMIN_LOGIN_FAILED);
	}

	PT_END(pt);
}

static void admin_login_procedure()
{
	if (m_is_registered != true) {
		if (pt_flags.pt1 == 1)
			pt_flags.pt1 = PT_SCHEDULE(err_thd(&pt1, ERR_NOT_REGISTERED));

		return;
	}

	if (pt_flags.pt1 == 1)
		pt_flags.pt1 = PT_SCHEDULE(admin_msc(&pt1));

	if (pt_flags.pt2 == 1)
		pt_flags.pt2 = PT_SCHEDULE(admin_ble(&pt2));

	if (pt_flags.pt3 == 1)
		pt_flags.pt3 = PT_SCHEDULE(admin_auth(&pt3));
}


static int verify_share_info(void * pinfo, uint8_t * p_LTMK)
{
	time_t curr_time = time(NULL);
	uint32_t errno;
	uint8_t adata[9];

	struct {
		uint8_t       nonce[12];
		shared_key_t  key;
		uint8_t       key_mic[4];
	} virtual_key;

	memcpy(&virtual_key, pinfo, sizeof(virtual_key));
	memcpy(adata, mi_sysinfo.did, 8);
	adata[8] = 0x01;

	errno = aes_ccm_auth_decrypt(mi_sysinfo.cloud_key,
	                 virtual_key.nonce, 12,
	                             adata,  9,
	           (void*)&virtual_key.key, 16,
	           (void*)&virtual_key.key,
	               virtual_key.key_mic,  4);

	if (errno != 0) {
		NRF_LOG_ERROR("Invaild virtual key:%d\n", errno);
		virtual_key.key.expire_time = 0;
		return 2;
	}
	NRF_LOG_INFO("Local  UTC %s", nrf_log_push(ctime(&curr_time)));
	NRF_LOG_INFO("Expire UTC %s", nrf_log_push(ctime(&virtual_key.key.expire_time)));
	
	
	if (virtual_key.key.expire_time <= curr_time + RTC_TIME_DRIFT) {
		NRF_LOG_ERROR("virtual key expired.\n");
		return 1;
	} else {
		memcpy(&key_id, &virtual_key.nonce[8], 4);
		return 0;
	}
}

static int shared_msc(pt_t *pt)
{
	PT_BEGIN(pt);

	MSC_POWER_ON();
	msc_control_block = MSC_XFER(MSC_PUBKEY, NULL, 0, dev_pub, 64);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	SET_DATA_VAILD(flags.dev_pub);

	if (schd_stat == SHARED_LOG_START_W_CERT) {
		msc_control_block = MSC_XFER(MSC_CERTS_LEN, NULL, 0, (void*)&m_certs_len, sizeof(m_certs_len));
		PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));

		m_certs_len.dev  = __REV16(m_certs_len.dev);
		m_certs_len.manu = __REV16(m_certs_len.manu);
		
		msc_control_block = MSC_XFER(MSC_DEV_CERT, NULL, 0, dev_cert, m_certs_len.dev);
		PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
		SET_DATA_VAILD(flags.dev_cert);

		msc_control_block = MSC_XFER(MSC_MANU_CERT, NULL, 0, manu_cert, m_certs_len.manu);
		PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
		SET_DATA_VAILD(flags.manu_cert);
	}

	mbedtls_sha256_context sha256_ctx;
	mbedtls_sha256_init(&sha256_ctx);
	mbedtls_sha256_starts(&sha256_ctx, 0 );
	mbedtls_sha256_update(&sha256_ctx, dev_pub, 64);
	mbedtls_sha256_finish(&sha256_ctx, dev_sha);

	msc_control_block = MSC_XFER(MSC_SIGN, dev_sha, 32, dev_sign, 64);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	SET_DATA_VAILD(flags.dev_sign);

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.app_pub));
	msc_control_block = MSC_XFER(MSC_ECDHE, app_pub, 64, eph_key, 32);
	PT_SPAWN(pt, &pt_msc_thd, msc_thread(&pt_msc_thd, &msc_control_block));
	SET_DATA_VAILD(flags.eph_key);
	MSC_POWER_OFF();

	PT_END(pt);
}

static int shared_ble(pt_t *pt)
{
	PT_BEGIN(pt);
	
	format_rx_cb(&rxfer_control_block, app_pub, sizeof(app_pub));
	PT_SPAWN(pt, &pt_r_rx_thd, rxfer_rx_thd(&pt_r_rx_thd, &rxfer_control_block, DEV_PUBKEY));
	SET_DATA_VAILD(flags.app_pub);

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.dev_pub));
	format_tx_cb(&rxfer_control_block, dev_pub, sizeof(dev_pub));
	PT_SPAWN(pt, &pt_r_tx_thd, rxfer_tx_thd(&pt_r_tx_thd, &rxfer_control_block, DEV_PUBKEY));

	if (schd_stat == SHARED_LOG_START_W_CERT) {
		PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.dev_cert));
		format_tx_cb(&rxfer_control_block, dev_cert, m_certs_len.dev);
		PT_SPAWN(pt, &pt_r_tx_thd, rxfer_tx_thd(&pt_r_tx_thd, &rxfer_control_block, DEV_CERT));
		NRF_LOG_INFO("dev_cert send "NRF_LOG_COLOR_CODE_BLUE"@ schd_time %d\n", schd_time);
		
		PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.manu_cert));
		format_tx_cb(&rxfer_control_block, manu_cert, m_certs_len.manu);
		PT_SPAWN(pt, &pt_r_tx_thd, rxfer_tx_thd(&pt_r_tx_thd, &rxfer_control_block, DEV_MANU_CERT));
		NRF_LOG_INFO("manu_cert send "NRF_LOG_COLOR_CODE_BLUE"@ schd_time %d\n", schd_time);
	}

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.dev_sign));
	format_tx_cb(&rxfer_control_block, &dev_sign, sizeof(dev_sign));
	PT_SPAWN(pt, &pt_r_tx_thd, rxfer_tx_thd(&pt_r_tx_thd, &rxfer_control_block, DEV_SIGNATURE));
	NRF_LOG_INFO("dev_sign send "NRF_LOG_COLOR_CODE_BLUE"@ schd_time %d\n", schd_time);

	format_rx_cb(&rxfer_control_block, &encrypt_share_data, sizeof(encrypt_share_data));
	PT_SPAWN(pt, &pt_r_rx_thd, rxfer_rx_thd(&pt_r_rx_thd, &rxfer_control_block, DEV_SHARE_INFO));
	SET_DATA_VAILD(flags.encrypt_share_data);
	PT_END(pt);
}

static int shared_auth(pt_t *pt)
{
	PT_BEGIN(pt);
	uint32_t errno;

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.eph_key));
	sha256_hkdf(     eph_key,         sizeof(eph_key),
	      (void *)share_salt,         sizeof(share_salt)-1,
	      (void *)share_info,         sizeof(share_info)-1,
	    (void *)&session_key,         sizeof(session_key));

	PT_WAIT_UNTIL(pt, DATA_IS_VAILD_P(flags.encrypt_share_data));
	errno = aes_ccm_auth_decrypt(session_key.app_key,
	                               nonce,  sizeof(nonce),
	                                NULL,  0,
	           encrypt_share_data.cipher,  sizeof(encrypt_share_data.cipher),
	                 (void*)&shared_info,
	              encrypt_share_data.mic,  sizeof(encrypt_share_data.mic));

	if (errno != 0 ) {
		NRF_LOG_ERROR("Invaild encrypt share info.\n");
		PT_EXIT(pt);
	}
	
// verify the virtual key

	if (verify_share_info(&shared_info, LTMK) != 0) {
		NRF_LOG_ERROR("SHARED LOG FAILED: %d\n", schd_time);
		PT_WAIT_UNTIL(pt, opcode_send(SHARED_LOG_FAILED) == NRF_SUCCESS);
		enqueue(&schd_evt_queue, SCHD_EVT_SHARE_LOGIN_FAILED);
	} else {
		NRF_LOG_INFO("SHARED LOG SUCCESS: %d\n", schd_time);
		mi_crypto_init(&session_key);
		set_mi_authorization(SHARE_AUTHORIZATION);
		PT_WAIT_UNTIL(pt, opcode_send(SHARED_LOG_SUCCESS) == NRF_SUCCESS);
		enqueue(&schd_evt_queue, SCHD_EVT_SHARE_LOGIN_SUCCESS);
	}

	PT_END(pt);
}

static void shared_login_procedure()
{
	if (m_is_registered != true) {
		if (pt_flags.pt1 == 1)
			pt_flags.pt1 = PT_SCHEDULE(err_thd(&pt1, ERR_NOT_REGISTERED));

		return;
	}

	if (pt_flags.pt1 == 1)
		pt_flags.pt1 = PT_SCHEDULE(shared_msc(&pt1));

	if (pt_flags.pt2 == 1)
		pt_flags.pt2 = PT_SCHEDULE(shared_ble(&pt2));

	if (pt_flags.pt3 == 1)
		pt_flags.pt3 = PT_SCHEDULE(shared_auth(&pt3));
}

