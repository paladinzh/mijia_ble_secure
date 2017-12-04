/**@file
 *
 * @defgroup ble_mi Xiaomi Service
 * @{
 * @ingroup  ble_sdk_srv
 * @brief    Xiaomi Service implementation.
 *
 * @details The Xiaomi Service is a simple GATT-based service with many characteristics.
 *          Data received from the peer is passed to the application, and the data received
 *          from the application of this service is sent to the peer as Handle Value
 *          Notifications. This module demonstrates how to implement a custom GATT-based
 *          service and characteristics using the SoftDevice. The service
 *          is used by the application to send and receive pub_key and MSC Cert to and from the
 *          peer.
 *
 * @note The application must propagate SoftDevice events to the Xiaomi Service module
 *       by calling the ble_mi_on_ble_evt() function from the ble_stack_handler callback.
 */

#ifndef BLE_MI_SECURE_H__
#define BLE_MI_SECURE_H__

#include "ble.h"
#include "ble_srv_common.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__CC_ARM)
  #pragma anon_unions
#elif defined(__ICCARM__)
  #pragma language=extended
#elif defined(__GNUC__)
  /* anonymous unions are enabled by default */
#endif

#define MODE_CMD  0
#define MODE_ACK  1

#define BLE_UUID_MI_SERVICE 0xFE95                      /**< The UUID of the Xiaomi Service. */
#define BLE_MI_MAX_DATA_LEN (GATT_MTU_SIZE_DEFAULT - 3) /**< Maximum length of data (in bytes) that can be transmitted to the peer by the Xiaomi  service module. */

typedef enum {
	PUBKEY = 0x10,
} fast_xfer_data_t;

typedef struct {
	uint8_t            remain_len;
	fast_xfer_data_t   type;
	uint8_t            data[1];
} fast_xfer_frame_t;

typedef struct {
	uint8_t            remain_len;
	fast_xfer_data_t   type;
	uint8_t            data[18];
} fast_xfer_tx_frame_t;

typedef struct {
	uint8_t           curr_len;
	uint8_t           full_len;
	uint8_t              avail;
	fast_xfer_data_t      type;
	uint8_t          data[255];
} fast_xfer_t;

typedef enum {
	DEV_LIST = 0x00,
	DEV_CERT,
	DEV_MANU_CERT,
	DEV_PUBKEY,
	DEV_SIGNATURE,
	DEV_LOGIN_INFO,
	DEV_SHARE_INFO
} fctrl_cmd_t;

typedef enum {
	A_SUCCESS = 0x00,
	A_READY,
	A_BUSY,
	A_TIMEOUT,
	A_CANCEL,
	A_LOST
} fctrl_ack_t;

typedef struct {
	uint8_t mode;
	uint8_t type;
	uint8_t arg[2];
} reliable_fctrl_t;

typedef struct {
	uint16_t sn;
	union {
		uint8_t          data[18];
		reliable_fctrl_t     ctrl;
	};
} reliable_xfer_frame_t;

typedef enum {
	RXFER_READY = 0x01,
	RXFER_BUSY,

	RXFER_WAIT_CMD,
	RXFER_WAIT_ACK,

	RXFER_TXD,
	RXFER_RXD,

	RXFER_DONE,
	RXFER_ERROR = 0xFF
} rxfer_stat_t;

typedef struct {
	uint16_t    max_tx_num;
	uint16_t        tx_num;
	uint16_t    max_rx_num;
	uint16_t        rx_num;
	uint16_t       curr_sn;
	uint8_t           mode;
	uint8_t            cmd;
	uint8_t            ack;
	uint8_t         *pdata;
	uint8_t     last_bytes;
	rxfer_stat_t     state;
} reliable_xfer_t;

/**@brief Xiaomi Service event handler type. */
typedef void (*ble_mi_data_handler_t) (uint8_t * p_data, uint16_t length);

/**@brief Xiaomi Service initialization structure.
 *
 * @details This structure contains the initialization information for the service. The application
 * must fill this structure and pass it to the service using the @ref ble_mi_init
 *          function.
 */
typedef struct
{
    ble_mi_data_handler_t data_handler; /**< Event handler to be called for handling received data. */
} ble_mi_init_t;

/**@brief Xiaomi Service structure.
 *
 * @details This structure contains status information related to the service.
 */
typedef struct {
	uint8_t                  uuid_type;               /**< UUID type for Xiaomi Service Base UUID. */
	uint16_t                 service_handle;          /**< Handle of Xiaomi Service (as provided by the SoftDevice). */

	ble_gatts_char_handles_t version_handles;         /**< Handles related to the characteristic (as provided by the SoftDevice). */
	ble_gatts_char_handles_t ctrl_point_handles;      /**< Handles related to the characteristic (as provided by the SoftDevice). */
	ble_gatts_char_handles_t secure_handles;
	ble_gatts_char_handles_t fast_xfer_handles;              
              
	uint16_t                 conn_handle;             /**< Handle of the current connection (as provided by the SoftDevice). BLE_CONN_HANDLE_INVALID if not in a connection. */
	bool                     is_notification_enabled; /**< Variable to indicate if the peer has enabled notification of the RX characteristic.*/
	ble_mi_data_handler_t    data_handler;            /**< Event handler to be called for handling received data. */
} ble_mi_t;

/**@brief Function for initializing the Xiaomi Service.
 *
 * @param[in] p_mi_s_init  Information needed to initialize the service.
 *
 * @retval NRF_SUCCESS If the service was successfully initialized. Otherwise, an error code is returned.
 * @retval NRF_ERROR_NULL If either of the pointers p_mi_s or p_mi_s_init is NULL.
 */
uint32_t ble_mi_init(const ble_mi_init_t * p_mi_s_init);

/**@brief Function for handling the Xiaomi Service's BLE events.
 *
 * @details The Xiaomi Service expects the application to call this function each time an
 * event is received from the SoftDevice. This function processes the event if it
 * is relevant and calls the Xiaomi Service event handler of the
 * application if necessary.
 *
 * @param[in] p_ble_evt   Event received from the SoftDevice.
 */
void ble_mi_on_ble_evt(ble_evt_t * p_ble_evt);

/**@brief Function for sending Auth status to the peer.
 *
 * @details This function sends the input status as an AUTH characteristic notification to the
 *          peer.
 *
 * @param[in] status    Status to be sent.
 *
 * @retval NRF_SUCCESS If the status was sent successfully. Otherwise, an error code is returned.
 */
uint32_t opcode_send(uint32_t status);
uint32_t opcode_recv(void);

/**@brief Function for setting version characteristic.
 *
 * @details  Version format: "1.2.3_0001"
 *           A.B.C is mijia ble svr protocol version. e.g. "1.2.3"
 *           The number after underline is user firmware version. e.g."0001"
 *
 *
 * @param[in] in    Point to the version string. 
 *
 * @retval NRF_SUCCESS If the status was sent successfully. Otherwise, an error code is returned.
 */
uint32_t version_set(uint8_t *in);

/**@brief Function for getting version characteristic.
 *
 * @details  Version format: "1.2.3_0001"
 *           A.B.C is mijia ble svr protocol version. e.g. "1.2.3"
 *           The number after underline is user firmware version. e.g."0001"
 *
 *
 * @param[out] out    Point to the version string. 
 *
 * @retval NRF_SUCCESS If the status was sent successfully. Otherwise, an error code is returned.
 */
void version_get(uint8_t *out);


int fast_xfer_recive(fast_xfer_t *pxfer);
int fast_xfer_send(fast_xfer_t *pxfer);

int reliable_xfer_cmd(fctrl_cmd_t cmd, ...);
int reliable_xfer_ack(fctrl_ack_t ack, ...);
int reliable_xfer_data(reliable_xfer_t *pxfer, uint16_t sn);

#ifdef __cplusplus
}
#endif

#endif // BLE_MI_SECURE_H__

/** @} */
