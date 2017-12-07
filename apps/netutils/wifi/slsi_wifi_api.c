/****************************************************************************
 *
 * Copyright 2017 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/

#include <tinyara/config.h>

#include "utils/includes.h"
#include "utils/common.h"
#include "common/wpa_ctrl.h"
#include "common/ieee802_11_defs.h"
#include "common/ieee802_11_common.h"
#include "common/defs.h"

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <mqueue.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <slsi_wifi_api.h>
#include <slsi_wifi_nvmap.h>

#include "slsi_wifi_private.h"

extern int wpa_supplicant_main(int argc, char *argv[]);
/* Reuse SSID decoder from wpa_supplicant: mind requires buf to be len+1 long to avoid mem corruption*/
extern size_t printf_decode(u8 *buf, size_t maxlen, const char *str);
extern void printf_encode(char *txt, size_t maxlen, const u8 *data, size_t len);

#define IEEE80211_CAP_IBSS              0x0002

/* Events received - used to handle connect failed cases */
#define SLSI_EVENT_ASSOCIATION_REQ_FAILED "Association request to the driver failed"

#define SLSI_WIFI_WEP_ASCII_KEY_MIN     5
#define SLSI_WIFI_WEP_ASCII_KEY_MAX     13
#define SLSI_WIFI_WEP_HEX_KEY_MIN       10
#define SLSI_WIFI_WEP_HEX_KEY_MAX       26
#define SLSI_WIFI_WPA_ASCII_KEY_MIN     8
#define SLSI_WIFI_WPA_ASCII_KEY_MAX     (SLSI_PASSPHRASE_LEN - 1)

/* Scan interval in specified in slsi_wifi_api.h.
 * Below values are use to boundary check */
#define SLSI_SCAN_INTERVAL_MIN   10
#define SLSI_SCAN_INTERVAL_MAX   60

/* Defines related to callback thread and mqueue */
#define SLSI_CALLBACK_MQUEUE    "callback_queue"
#define SLSI_CALLBACK_MGS_COUNT 10
#define SLSI_CALLBACK_MGS_SIZE  sizeof(slsi_callback_t) + sizeof(slsi_msg_callback_t)

typedef enum {
	SLSI_WIFIAPI_STATE_NOT_STARTED,
	SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING,	//SLSI_WIFIAPI_STATE_STA_ENABLED
	SLSI_WIFIAPI_STATE_STA_CONNECTING,
	SLSI_WIFIAPI_STATE_STA_CONNECTED,
	SLSI_WIFIAPI_STATE_STA_DISCONNECTING,
	SLSI_WIFIAPI_STATE_AP_ENABLING,
	SLSI_WIFIAPI_STATE_AP_ENABLED,
	SLSI_WIFIAPI_STATE_AP_CONNECTED,
	SLSI_WIFIAPI_STATE_AP_DISABLING,
	SLSI_WIFIAPI_STATE_TERMINATING,
	SLSI_WIFIAPI_STATE_RECOVERING,
	SLSI_WIFIAPI_STATE_P2P,
	SLSI_WIFIAPI_STATE_COUNT
} slsi_api_states_t;

#if SLSI_API_DEBUG == 1
/* Used in conjunction with WiFi_InterFace_ID for debug print */
static char *slsi_wifi_interface_id[4] = {
	"WIFI_NONE",
	"WIFI_STATION_IF",
	"WIFI_SOFT_AP_IF",
	"WIFI_P2P_IF"
};
#endif

char *slsi_state_strings[SLSI_WIFIAPI_STATE_COUNT] = {
	"STATE_NOT_STARTED",
	"STATE_SUPPLICANT_RUNNING",	//SLSI_WIFIAPI_STATE_STA_ENABLED
	"STATE_STA_CONNECTING",
	"STATE_STA_CONNECTED",
	"STATE_STA_DISCONNECTING",
	"STATE_AP_ENABLING",
	"STATE_AP_ENABLED",
	"STATE_AP_CONNECTED",
	"STATE_AP_DISABLING",
	"STATE_TERMINATING",
	"STATE_RECOVERING",
	"STATE_P2P"
};

#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY

typedef struct slsi_recovery_data {
	slsi_api_states_t old_state;
	slsi_api_states_t recovering_state;
	WiFi_InterFace_ID_t old_interface_type;
	slsi_network_link_callback_t link_up;	// callback function
	slsi_network_link_callback_t link_down;	// callback function
	slsi_scan_result_callback_t scan_result_handler;	// callback function
	char remote_bssid[18];		// char string e.g. xx:xx:xx:xx:xx:xx
	slsi_ap_config_t ap_config;	// contains security pointer!
	uint8_t ssid[SLSI_SSID_LEN + 1];	// 802.11 spec defined unspecified or uint8
	int8_t ssid_len;			// length of ssid - # of valid octets
	uint8_t bssid[18];			// char string e.g. xx:xx:xx:xx:xx:xx
	slsi_security_config_t *security;
} slsi_recovery_data_t;

static slsi_recovery_data_t g_recovery_data;
static pthread_t g_recovery_thread;
static int g_recovery_running = 0;
static sem_t g_sem_recover;
#endif							// CONFIG_SCSC_WLAN_AUTO_RECOVERY
// END Recovery

static slsi_api_states_t g_state = SLSI_WIFIAPI_STATE_NOT_STARTED;
static struct wpa_ctrl *g_ctrl_conn;
static int g_wpa_attached = 0;
static int g_running = 0;
static int g_scanning = 0;
static int g_recovering = 0;
static int g_callback_running = 0;
static mqd_t g_send_cbmqfd = NULL;
static mqd_t g_recv_cbmqfd = NULL;
static pthread_t g_callback_thread;
static pthread_t g_monitoring_thread;
static sem_t g_sem_terminate;
static sem_t g_sem_ap_mode;
static sem_t g_sem_disconnect;
static sem_t g_sem_api_block;
static pthread_mutex_t mutex_state;
static bool g_mutex_initialized = FALSE;
static slsi_scan_result_callback_t g_scan_result_handler;
static slsi_network_link_callback_t g_link_up;
static slsi_network_link_callback_t g_link_down;
static slsi_ap_config_t *g_ap_config;
static pid_t g_task = 0;
static char *g_network_id = NULL;
static uint8_t g_num_sta_connected = 0;
static char *g_active_ifname = NULL;
static slsi_wifi_nv_data_t *g_slsi_wifi_nv_data;
static char g_country_code[3] = { 0 };
static char *g_scan_network_id = NULL;
static void *g_nvram;

#ifdef CONFIG_SLSI_WIFI_FILESYSTEM_SUPPORT
#ifdef CONFIG_SLSI_WIFI_P2P_API
static const char g_supplicant_conf[] = "ctrl_interface=udp\n p2p_disabled=1";
static const char g_supplicant_conf_p2p[] = "ctrl_interface=udp\n device_name=T200\n " "config_methods='push_button keypad display'";
/*
 * Available methods: usba ethernet label display ext_nfc_token
 * int_nfc_token nfc_interface push_button keypad
 * virtual_display physical_display
 * virtual_push_button physical_push_button.
 * */
#else
static const char g_supplicant_conf[] = "ctrl_interface=udp";
#endif
#endif

/* We need this to be a static array, as the supplicant needs it after we exit from slsi_init()*/
static char *sup_argv[10];		// Enough to contain all modes

#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
#define LOCK_API sem_wait(&g_sem_api_block)
#else
#define LOCK_API (void)0
#endif

#define LOCKUNLOCK_CRITICAL { \
    EPRINT("LOCKUNLOCK_CRITICAL - %s\n", __func__); \
    if(SLSI_API_VERBOSE) fflush(stdout); \
    pthread_mutex_trylock(&mutex_state); \
    pthread_mutex_unlock(&mutex_state); \
}

#define ENTER_CRITICAL { \
    VPRINT("ENTER_CRITICAL in API - %s\n", __func__); \
    if(!g_mutex_initialized){ \
        VPRINT("Initializing the mutex\n"); \
        if(pthread_mutex_init(&mutex_state, NULL) != OK){ \
            EPRINT("Could not initialize mutex\n"); \
        }else { \
            VPRINT("Mutex ready\n"); \
            g_mutex_initialized = TRUE; \
        } \
    } \
    if(SLSI_API_VERBOSE) fflush(stdout); \
    if(g_mutex_initialized){ \
        int err = pthread_mutex_lock(&mutex_state); \
       if(err != OK){ \
           EPRINT("mutex lock failed with error %d",err); \
       } \
       if((g_state == SLSI_WIFIAPI_STATE_RECOVERING || g_recovering ) && g_mutex_initialized){ \
           VPRINT("sem_wait recovering - %s\n", __func__); \
            if(SLSI_API_VERBOSE) fflush(stdout); \
               LOCK_API;} \
    } \
}

#define LEAVE_CRITICAL { \
    VPRINT("LEAVE_CRITICAL in API - %s\n", __func__); \
    if(SLSI_API_VERBOSE) fflush(stdout); \
    if(g_mutex_initialized) { \
        pthread_mutex_unlock(&mutex_state); \
    } \
}

/*
 * Prototype functions
 */
static int8_t slsi_get_bssid(char **bssid);
static int8_t slsi_init(WiFi_InterFace_ID_t interface_id, const slsi_ap_config_t *ap_config);
static void slsi_deinit(void);
static int8_t slsi_set_security(const slsi_security_config_t *sec_config, const char *network_id);
static int8_t slsi_stop_ap(void);
static int8_t slsi_api_start(WiFi_InterFace_ID_t interface_id, const slsi_ap_config_t *ap_config);
static int8_t slsi_join_network(uint8_t *ssid, int ssid_len, uint8_t *bssid, const slsi_security_config_t *sec_config);
static void slsi_wpa_reopen(void);
static int8_t slsi_terminate_supplicant(void);
static int8_t slsi_check_status(uint8_t *ssid, int8_t *ssid_len, char *bssid);
static int8_t slsi_get_network(uint8_t *ssid, uint8_t ssid_len, char **network_id);
static int8_t slsi_recv_pending(char **result);
static void slsi_remove_network(char *network_id);
static void slsi_init_nvram(void);
static uint8_t slsi_stop_supplicant(void);
static int8_t slsi_wpa_close(bool terminate);
static int8_t slsi_start_scan(void);
int8_t slsi_get_country_code(char *country_code);
static WiFi_InterFace_ID_t slsi_get_op_mode(void);
static void slsi_set_scan_interval(uint8_t interval);
static int8_t slsi_disable_all_networks(void);

#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
static slsi_ap_config_t *slsi_get_ap_config(void);
#endif

/*
 * Private functions
 */
void slsi_msg_cb(char *msg, size_t len)
{
	DPRINT("SLSI_API msg_cb - %s\n", msg);
}

#ifdef CONFIG_TEST_ENGINE
te_func_t tefn = { &slsi_send_request, &slsi_get_bssid, &slsi_set_security, &slsi_get_network };
#endif

/**
 * Used to send request to WPA supplicant
 * Param:
 *  cmd         Command string
 *  success     Used if command returns no buffer string to give result
 *              otherwise use NULL
 * Return:
 *  buf         Character array[WPA_BUFFER_SIZE] with result string
  */
char *slsi_send_request(char *ifname, char *cmd, int8_t *result)
{
	int retry = 0;
	if (g_ctrl_conn == NULL) {
		EPRINT("Ctrl iface not available\n");
		return NULL;
	}

	char *buf = (char *)zalloc(WPA_BUFFER_SIZE);
	if (buf == NULL) {
		return buf;
	}

	size_t len = WPA_BUFFER_SIZE - 1;
	DPRINT("SLSI_API command %s \n", cmd);
	if (ifname) {
		wpa_update_send_cmd_ifname(g_ctrl_conn, ifname);
	}
	for (retry = 0; retry < REQUEST_RETRY_COUNT; retry++) {
		int ret = wpa_ctrl_request(g_ctrl_conn, cmd, strlen(cmd), buf, &len, NULL);
		if (ret == -2) {
			EPRINT("%s command timed out. \n", cmd);
			if (result) {
				*result = SLSI_STATUS_ERROR;
			}
		} else if (ret < 0) {
			EPRINT("... command failed. \n");
			if (result) {
				*result = SLSI_STATUS_ERROR;
			}
		} else {
			buf[len] = '\0';
			VPRINT("Result length=%d\n%s", (int)len, buf);
			if (len > 0 && buf[len - 1] != '\n') {
				DPRINT("\n");
			}
			if (result) {
				if (strncmp(buf, "OK", strlen("OK")) == 0) {
					*result = SLSI_STATUS_SUCCESS;
				} else if (strncmp(buf, "FAIL", strlen("FAIL")) == 0) {
					DPRINT("SLSI_API command %s returned FAIL from supplicant \n", cmd);
					*result = SLSI_STATUS_COMMAND_FAILED;
				} else if (strncmp(buf, "UNKNOWN COMMAND", strlen("UNKNOWN COMMAND")) == 0) {
					*result = SLSI_STATUS_COMMAND_UNKNOWN;
				} else {
					*result = SLSI_STATUS_ERROR;
				}
			}
			break;
		}
		memset(buf, 0, WPA_BUFFER_SIZE);
	}
	return buf;
}

void slsi_send_command_str_digit(char *ifname, char *string, int digit, int8_t *result)
{
	char *pbuf = NULL;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };
	snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%d", string, digit);
	pbuf = slsi_send_request(ifname, command, result);
	if (pbuf) {
		free(pbuf);
		pbuf = NULL;
	}
}

void slsi_send_command_str(char *ifname, int8_t *result, const char *fmt, ...)
{
	char *pbuf = NULL;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };
	va_list(args);
	va_start(args, fmt);
	vsnprintf(command, WPA_COMMAND_MAX_SIZE, fmt, args);
	if (command[0] != '\0') {
		pbuf = slsi_send_request(ifname, command, result);
	}
	if (pbuf) {
		free(pbuf);
		pbuf = NULL;
	}
	va_end(args);
}

void slsi_send_command_str_upto_4(char *ifname, char *one, char *two, char *three, char *four, int8_t *result)
{
	char *pbuf = NULL;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };
	if (four && three && two && one) {
		snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %s%s", one, two, three, four);
	} else if (three && two && one) {
		snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %s", one, two, three);
	} else if (two && one) {
		snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s", one, two);
	} else if (one) {
		snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", one);
	}
	if (command[0] != '\0') {
		pbuf = slsi_send_request(ifname, command, result);
	}
	if (pbuf) {
		free(pbuf);
		pbuf = NULL;
	}
}

int8_t slsi_leave_network(char *ifname)
{
	int8_t result = SLSI_STATUS_ERROR;
	slsi_send_command_str_upto_4(ifname, WPA_COMMAND_DISCONNECT, NULL, NULL, NULL, &result);
	return result;
}

static int8_t slsi_save_config(void)
{
	int8_t result = SLSI_STATUS_ERROR;
	char *pbuf = NULL;
	pbuf = slsi_send_request(NULL, WPA_COMMAND_SAVE_CONFIG, &result);
	if (pbuf) {
		free(pbuf);
		pbuf = NULL;
	}
	return result;
}

static int8_t slsi_create_conffile(char *file, const char *file_config)
{
	uint8_t ret = SLSI_STATUS_ERROR;
	FILE *fp = NULL;
	fp = fopen(file, "r");
	if (fp == NULL) {
		DPRINT("Creating config file %s with default content '%s'\n", file, file_config);
		fp = fopen(file, "w+");
		if (fp == NULL) {
			EPRINT("write_file: ERROR failed to open %s for writing, errno=%d\n", file, errno);
		} else {
			size_t fres = fwrite(file_config, strlen(file_config), 1, fp);
			if (fres != 1) {
				EPRINT("write_file: ERROR failed to write to %s, errno=%d, ret=%d\n", file, errno, fres);
				ret = SLSI_STATUS_ERROR;
			} else {
				ret = SLSI_STATUS_SUCCESS;
			}
		}
	} else {
		DPRINT("Config file already exists %s\n", file);
		ret = SLSI_STATUS_SUCCESS;
	}
	if (fp != NULL) {
		fclose(fp);
	}
	return ret;
}

static int8_t slsi_init_filesystem(WiFi_InterFace_ID_t interface)
{
	uint8_t ret = SLSI_STATUS_ERROR;
#ifdef CONFIG_SLSI_WIFI_FILESYSTEM_SUPPORT
	FILE *fp = NULL;
	char *logfile = CONFIG_SLSI_WIFI_DIR "/" CONFIG_SLSI_WIFI_LOG_FILE_NAME;
	/* Create the Wi-Fi directory */
	int res = mkdir(CONFIG_SLSI_WIFI_DIR, 0666);
	if (res == 0 || get_errno() == EEXIST) {
		/* Clear supplicant log file */
		fp = fopen(logfile, "w+");
		DPRINT("Creating logfile %s\n", logfile);
		if (fp != NULL) {
			fclose(fp);
		}
		char *conffile = CONFIG_SLSI_WIFI_DIR "/" CONFIG_SLSI_WIFI_CONFIG_FILE_NAME;
		// create configuration file
		ret = slsi_create_conffile(conffile, g_supplicant_conf);

#ifdef CONFIG_SLSI_WIFI_P2P_API
		char *conffile2 = CONFIG_SLSI_WIFI_DIR "/" CONFIG_SLSI_WIFI_P2P_CONFIG_FILE_NAME;
		// create configuration file for p2p
		if (conffile2) {
			ret = slsi_create_conffile(conffile2, g_supplicant_conf_p2p);
		}
#endif

	}
#endif

	return ret;
}

static bool slsi_get_security_from_flags(const char *flag_str, slsi_security_config_t **sec, uint8_t *sec_count)
{
	bool ret = FALSE;			//NO security
	char *pos;
	char *end;
	char buffer[WPA_COMMAND_MAX_SIZE];
	memset(buffer, 0, WPA_COMMAND_MAX_SIZE);
	//identify how many mode security structs are needed
	//simply count [ in the flag string
	int x = 0;
	uint8_t mode_count = 0;
	if (flag_str != NULL) {
		while (flag_str[x] != '\0') {
			if (flag_str[x] == '[') {
				/* we should not count [P2P] as we do not support it,
				 * and [ESS] as this it not a security type */
				if ((flag_str[x + 1] != 'E') && (flag_str[x + 1] != 'P')) {
					mode_count++;
				} else {
					// Assumes that EAP can be in combination with supported security
					VPRINT("not counted: %s\n", &flag_str[x]);
				}
			}
			x++;
		}
		if (!mode_count) {
			// if flags is only ESS then accept as this is an open AP
			if (strncmp(flag_str, "[ESS]", strlen(flag_str)) == 0) {
				ret = TRUE;
			}
			goto out;
		} else {
			ret = TRUE;
			*sec_count = mode_count;
			*sec = zalloc(sizeof(slsi_security_config_t) * mode_count);
			if (*sec == NULL) {
				EPRINT("could not allocate memory for security mode block\n");
				ret = FALSE;
				goto out;
			}
		}

		slsi_security_config_t *tmpsec = *sec;	//point to first security struct in list
		uint8_t strlen = 0;
		pos = (char *)flag_str;
		for (x = 0; x < mode_count; x++) {
			bool wpa  = FALSE;
			bool wpa2 = FALSE;

			pos = strchr(pos, '[');
			if (pos == NULL) {
				break;
			}
			pos++;
			end = strchr(pos, ']');
			strlen = end - pos;	//includes room for null termination
			memcpy(&buffer, pos, strlen);
			buffer[strlen] = '\0';
			// Extract flags - e.g. flags=[WPA-PSK-CCMP+TKIP][WPA2-PSK-CCMP+TKIP][ESS]

			if (strstr(buffer, "WPA2-PSK")) {
				wpa2 = TRUE;
			} else if (strstr(buffer, "WPA-PSK")) {
				wpa = TRUE;
			} else if (strstr(buffer, "WEP")) {
				// It is not possible to distinguish WEP open and shared so both set
				tmpsec->secmode = SLSI_SEC_MODE_WEP | SLSI_SEC_MODE_WEP_SHARED;
			} else if (strstr(buffer, "WPA2-EAP")) {
				tmpsec->secmode = SLSI_SEC_MODE_EAP;
			} else if (strstr(buffer, "WPA-EAP")) {
				tmpsec->secmode = SLSI_SEC_MODE_EAP;
			} else if (strstr(buffer, "NONE")) {
				// deprecated
			} else if (strstr(buffer, "IBSS")) {
				// not necessary any security
			} else if (strstr(buffer, "ESS")) {
				tmpsec->secmode = SLSI_SEC_MODE_OPEN;
			} else if (strstr(buffer, "P2P")) {
				// P2P is not supported yet
			}

			if (wpa) {
				// find cipher related settings
				if (strstr(buffer, "CCMP+TKIP")) {
					tmpsec->secmode = SLSI_SEC_MODE_WPA_MIXED;
				} else if (strstr(buffer, "CCMP")) {
					tmpsec->secmode = SLSI_SEC_MODE_WPA_CCMP;
				} else if (strstr(buffer, "TKIP")) {
					tmpsec->secmode = SLSI_SEC_MODE_WPA_TKIP;
				}
				tmpsec++;		//go to next struct
			} else if (wpa2) {
				// find cipher related settings
				if (strstr(buffer, "CCMP+TKIP")) {
					tmpsec->secmode = SLSI_SEC_MODE_WPA2_MIXED;
				} else if (strstr(buffer, "CCMP")) {
					tmpsec->secmode = SLSI_SEC_MODE_WPA2_CCMP;
				} else if (strstr(buffer, "TKIP")) {
					tmpsec->secmode = SLSI_SEC_MODE_WPA2_TKIP;
				}
				tmpsec++;		//go to next struct
			}
		}
	}
out:
	return ret;
}

static uint8_t *slsi_hexstr_2_bytearray(char *str, size_t str_size, size_t *array_size)
{
	char *string = str;
	uint8_t *byte_array = NULL;
	size_t str_len;
	size_t array_len;
	size_t count = 0;
	size_t i;

	if (str == NULL) {
		VPRINT("str NULL \n");
		return NULL;
	}

	str_len = str_size;
	if (str_len % 2 != 0) {	// must be even
		VPRINT("str not even length\n");
		return NULL;
	}
	array_len = str_len / 2;
	byte_array = (uint8_t *)zalloc(array_len + 1);
	if (byte_array) {
		char a, b;
		VPRINT("length=%d, bytes= ", array_len);
		for (i = 0; i < str_len; i += 2) {
			a = string[i + 0];
			b = string[i + 1];
			byte_array[i / 2] = ((a <= '9' ? a - '0' : a - 'a' + 10) << 4) + (b <= '9' ? b - '0' : b - 'a' + 10);
			// printf used due to not wanting function name pre-appended
			if (SLSI_API_VERBOSE) {
				printf("%02x, ", byte_array[i / 2]);
			}

			count++;
		}
		// printf used due to not wanting function name pre-appended
		if (SLSI_API_VERBOSE) {
			printf("\n");
		}
	}
	*array_size = count;
	return byte_array;
}

static uint8_t *slsi_bytearray_2_hexstr(const uint8_t *bytes, size_t size, size_t *stringlen)
{

	uint8_t *hexstr = zalloc((size * 2) + 1);	//need room for null termination
	if (hexstr != NULL) {
		uint8_t *ptr = hexstr;
		size_t i = 0;
		for (i = 0; i < size; i++) {
			ptr += snprintf((char *)ptr, 3 /*xx + \0 */ , "%02X", bytes[i]);
		}
		*stringlen = strlen((char *)hexstr);
	} else {
		*stringlen = 0;
	}
	return hexstr;
}

static const uint8_t *slsi_get_ie(const uint8_t *byte_array, const size_t size, uint8_t ie_type)
{
	const uint8_t *end;
	const uint8_t *pos;
	VPRINT("Find IE type %02x \n", ie_type);
	// Find IE type
	pos = byte_array;
	end = pos + size;
	while (pos + 1 < end) {
		if (pos + 2 + pos[1] > end) {
			VPRINT(" not found \n");
			break;
		}
		if (pos[0] == ie_type) {
			VPRINT(" found\n");
			return pos;
		}
		pos += 2 + pos[1];
	}

	return NULL;
}

/**
 * Get BSS details from wpa_supplicant.
 * Returns true at success.
 */
static bool slsi_get_bss_info(const char *bssid, slsi_scan_info_t *info)
{
	char *pbuf = NULL;
	char *pos;
	char *end;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };

	/*
	   id=11
	   bssid=20:4e:7f:a5:44:0a
	   freq=2472
	   beacon_int=100
	   capabilities=0x0411
	   qual=-64
	   noise=0
	   level=0
	   tsf=0011221546694051
	   age=1
	   ie=0019636f65782d6d616e2d746573742d6e672d333430302d326734010882840b162430486c03010d2a01002f010030140100000fac040100000fac040100000fac020c0032040c121860dd7e0050f204104a00011010440001021041000100103b0001031047001030138638384c1a56f8ffe0746aedf8c51021000d4e4554474541522c20496e632e10230008574e44523334303010240008574e4452333430301042000230311054000800060050f204000110110008574e445233343030100800020084103c000103dd090010180200f0050000dd180050f2020101800003a4000027a4000042435e0062322f00
	   flags=[WPA2-PSK-CCMP][ESS]
	   ssid=coex-man-test-ng-3400-2g4
	   snr=0
	   est_throughput=1000
	 */

	snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s", WPA_COMMAND_BSS, bssid);
	pbuf = slsi_send_request(NULL, command, NULL);
	if (pbuf) {
		// Extract bssid
		pos = strstr(pbuf, WPA_PARAM_BSSID);
		if (pos == NULL) {
			goto errout;
		}
		pos += 6;
		end = strchr(pos, '\n');
		if (end == NULL) {
			goto errout;
		}
		*end = '\0';
		memcpy(&(info->bssid), pos, 18);
		pos = end + 1;
		// Extract channel
		pos = strstr(pos, WPA_PARAM_FREQ);
		if (pos == NULL) {
			goto errout;
		}
		pos += 5;
		end = strchr(pos, '\n');
		if (end == NULL) {
			goto errout;
		}
		*end = '\0';
		ieee80211_freq_to_chan(strtol(pos, &end, 10), &(info->channel));
		pos = end + 1;
		// Extract beacon interval
		pos = strstr(pos, WPA_PARAM_BEACON_INT);
		if (pos == NULL) {
			goto errout;
		}
		pos += 11;
		end = strchr(pos, '\n');
		if (end == NULL) {
			goto errout;
		}
		*end = '\0';
		info->beacon_period = strtol(pos, &end, 10);
		pos = end + 1;
		// Extract bss_type
		pos = strstr(pos, WPA_PARAM_CAPABILITIES);
		if (pos == NULL) {
			goto errout;
		}
		pos += 13;
		end = strchr(pos, '\n');
		if (end == NULL) {
			goto errout;
		}
		*end = '\0';
		int caps = strtol(pos, &end, 16);
		pos = end + 1;
		if (caps & IEEE80211_CAP_IBSS) {
			info->bss_type = 1;
		}
		// Extract qual as RSSI (set in t20_ops.c)
		pos = strstr(pos, WPA_PARAM_QUAL);
		if (pos == NULL) {
			goto errout;
		}
		pos += 5;
		end = strchr(pos, '\n');
		if (end == NULL) {
			goto errout;
		}
		*end = '\0';
		info->rssi = strtol(pos, &end, 10);
		pos = end + 1;
		// Extract ei data
		pos = strstr(pos, WPA_PARAM_IE);
		if (pos == NULL) {
			goto errout;
		}
		pos += 3;
		end = strchr(pos, '\n');
		if (end == NULL) {
			goto errout;
		}
		*end = '\0';
		{
			size_t length = end - pos;
			const uint8_t *ht_capab = NULL, *vendor_specific = NULL;
			size_t size = 0;
			uint8_t *bytes = (uint8_t *) slsi_hexstr_2_bytearray(pos, length, &size);
			if (bytes) {
				ht_capab = slsi_get_ie(bytes, size, WLAN_EID_HT_CAP);
				if (ht_capab) {
					// Extract phy_mode
					info->phy_mode = 1;

					if (ht_capab[1] == 26) {
						struct ieee80211_ht_capabilities *cap;
						cap = (struct ieee80211_ht_capabilities *)(ht_capab + 2);
						uint16_t ht_capab_info = le_to_host16(cap->ht_capabilities_info);
						VPRINT("ht_capab_info %04x found \n", ht_capab_info);
						// Extract ht_modes - green field
						info->ht_mode.ht_capab_info = ht_capab_info;
						if (SLSI_API_VERBOSE) {
							if ((ht_capab_info & HT_CAP_INFO_GREEN_FIELD) == HT_CAP_INFO_GREEN_FIELD) {
								VPRINT("ht_capab HT_CAP_INFO_GREEN_FIELD found\n");
							}
							// Extract ht_modes - short guard interval
							if ((ht_capab_info & HT_CAP_INFO_SHORT_GI20MHZ) == HT_CAP_INFO_SHORT_GI20MHZ) {
								VPRINT("ht_capab HT_CAP_INFO_SHORT_GI20MHZ found\n");
							}
						}
						// Extract ht_modes - ht_mcs
						size_t i;
						for (i = 0; i < IEEE80211_HT_MCS_MASK_LEN; i++) {
							VPRINT("ht_capab ht_mcs %02x found\n", cap->supported_mcs_set[i]);
							info->ht_mode.mcs_index[i] = (uint8_t) cap->supported_mcs_set[i];
						}
					}
				}

				uint8_t *tmpBytes = bytes;
				uint8_t *tmpEnd = bytes + length;
				// Extract wps_support
				slsi_vendor_ie_t *tmpvsie = NULL;
				while (tmpBytes + 1 < tmpEnd) {
					vendor_specific = slsi_get_ie(tmpBytes, size, WLAN_EID_VENDOR_SPECIFIC);
					if (vendor_specific) {
						slsi_vendor_ie_t *vsie = zalloc(sizeof(slsi_vendor_ie_t));
						if (info->vsie == NULL) {
							info->vsie = zalloc(sizeof(slsi_vendor_ie_t));
							if (!info->vsie) {
								EPRINT("could not allocate memory for vsie\n");
								break;
							} else {
								tmpvsie = info->vsie;
								tmpvsie->next = NULL;
							}
						}
						if (WPS_IE_VENDOR_TYPE == WPA_GET_BE32(&vendor_specific[2])) {
							VPRINT("IE data - WLAN_EID_VENDOR_SPECIFIC + WPS_IE_VENDOR_TYPE length=%d found\n", vendor_specific[1]);
							info->wps_support = 1;
						}
						tmpBytes += (2 + vendor_specific[1]);
						if (vsie) {
							memcpy(vsie->oui, &vendor_specific[2], 3);
							vsie->content_length = vendor_specific[1];
							vsie->content = zalloc(vsie->content_length - 3 /*OUI*/);
							memcpy(vsie->content, &vendor_specific[5], vsie->content_length - 3);

							if (!tmpvsie) {
								tmpvsie = vsie;
								tmpvsie->next = NULL;
							} else {
								tmpvsie->next = vsie;
								tmpvsie = tmpvsie->next;
							}
						}
					} else {
						break;
					}
				}
				if (bytes) {
					free(bytes);
					bytes = NULL;
				}
			}
		}
		pos = end + 1;
		// Extract flags containing security mode
		pos = strstr(pos, WPA_PARAM_FLAGS);
		if (pos == NULL) {
			goto errout;
		}
		pos += 6;
		end = strchr(pos, '\n');
		if (end == NULL) {
			goto errout;
		}
		*end = '\0';
		char *tmpflags = zalloc(end - pos + 1);
		if (tmpflags == NULL) {
			goto errout;
		}
		strlcpy(tmpflags, pos, end - pos + 1);
		bool device_supported = slsi_get_security_from_flags(tmpflags, &info->sec_modes, &info->num_sec_modes);
		if (tmpflags) {
			free(tmpflags);
			tmpflags = NULL;
		}
		if (!device_supported) {
			goto errout;
		}
		pos = end + 1;
		// Extract ssid
		pos = strstr(pos, WPA_PARAM_SSID);
		if (pos == NULL) {
			goto errout;
		}
		pos += 5;
		end = strchr(pos, '\n');
		if (end == NULL) {
			goto errout;
		}
		*end = '\0';
		info->ssid_len = printf_decode(info->ssid, SLSI_SSID_LEN + 1, pos);
		pos = end + 1;

	}
	if (pbuf) {
		free(pbuf);
		pbuf = NULL;
		return TRUE;
	}
errout:
	if (pbuf) {
		free(pbuf);
		pbuf = NULL;
	}
	return FALSE;
}

static int str_starts(const char *src, const char *match)
{
	return strncmp(src, match, strlen(match)) == 0;
}

static void slsi_scan_event_handler(const char *str, const char *event)
{
	VPRINT("SLSI_API scan_event_handler - looking for: %s\n", event);

	if (str_starts(str, event)) {
		DPRINT("\n wanted event received %s\n", str);
		if (str_starts(str, WPA_EVENT_SCAN_RESULTS)) {
			g_scanning = 0;
			/* Send MESSAGE to callback thread to initiate callback */
			slsi_send_mqueue(SLSI_CALLBACK_SCAN_RESULT, NULL);
			if (g_scan_network_id) {
				// remove network
				slsi_send_command_str_upto_4(NULL, WPA_COMMAND_REMOVE_NETWORK, g_scan_network_id, NULL, NULL, NULL);
			}
		}
	}
}

bool slsi_event_received(const char *str, const char *event)
{

	bool received = FALSE;
	if (str_starts(str, event)) {
		VPRINT("Event found: %s\n", event);
		received = TRUE;
	}
	return received;
}

void slsi_sta_disconnect_event_handler(const char *str, slsi_reason_t *reason)
{
	const char *s;
	VPRINT("SLSI_API - got %s\n", str);
	memset(reason, 0, sizeof(slsi_reason_t));

	s = strstr(str, WPA_PARAM_BSSID);
	if (s != NULL) {
		s += 6;
		memcpy(&(reason->bssid), s, 17);
		s += 17;
	} else {
		memset(reason->bssid, 0, sizeof(reason->bssid));
		s = str;
	}
	s = strstr(s, WPA_PARAM_REASON);
	if (s != NULL) {
		reason->reason_code = (uint32_t) strtol(s + 7, NULL, 10);
	} else {
		s = str;
	}
	s = strstr(s, WPA_PARAM_LOACLLY_GENERATED);
	if (s == NULL) {
		// Reason not found or locally generated
		reason->locally_generated = 0;
	} else {
		reason->locally_generated = (uint8_t) strtol(s + 18, NULL, 10);
	}
	VPRINT("SLSI_API reason_code: %d locally_generated: %d\n", reason->reason_code, reason->locally_generated);

	VPRINT("SLSI_API send link_down\n");
}

#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
static void slsi_clean_recover(void)
{
	// Terminate recovery thread
	g_recovery_running = 0;
	sem_post(&g_sem_recover);
	// Free join_config security
	if (g_recovery_data.security) {
		free(g_recovery_data.security);
		g_recovery_data.security = NULL;
	}
	// Free ap_config security
	slsi_ap_config_t *t_ap_config = &g_recovery_data.ap_config;
	if (t_ap_config->security) {
		free(t_ap_config->security);
		t_ap_config->security = NULL;
	}
	// Free ap_config vsie
	if (t_ap_config->vsie) {
		if (t_ap_config->vsie->content) {
			free(t_ap_config->vsie->content);
			t_ap_config->vsie->content = NULL;
		}
		free(t_ap_config->vsie);
		t_ap_config->vsie = NULL;
	}
	// Clean g_recovery_data
	memset(&g_recovery_data, 0, sizeof(slsi_recovery_data_t));
}

static void slsi_verify_back(void)
{
	g_link_up = g_recovery_data.link_up;
	g_link_down = g_recovery_data.link_down;
	if (g_recovery_data.scan_result_handler) {
		if (slsi_start_scan() != SLSI_STATUS_SUCCESS) {
			EPRINT("Initiate scan failed!\n");
		}
	}
	sem_post(&g_sem_api_block);
	g_recovering = 0;
	EPRINT("Back to original state \n");
}

static void slsi_verify_recovered(void)
{
	if (g_recovering) {
		if (g_recovery_data.recovering_state == g_state) {
			// Back to normal
			slsi_verify_back();

			if (g_recovery_data.recovering_state != g_recovery_data.old_state) {
				slsi_reason_t reason;
				memset(&reason, 0, sizeof(slsi_reason_t));
				switch (g_recovery_data.old_state) {
				case SLSI_WIFIAPI_STATE_STA_DISCONNECTING:
					VPRINT("SLSI_WIFIAPI_STATE_STA_DISCONNECTING \n");
					if (g_link_down) {
						VPRINT("sta was disconnecting - send link_down\n");
						slsi_msg_callback_t msg;
						msg.reason = reason;
						slsi_send_mqueue(SLSI_CALLBACK_LINK_DOWN, &msg);
					}
					break;
				case SLSI_WIFIAPI_STATE_STA_CONNECTING:
					VPRINT("SLSI_WIFIAPI_STATE_STA_CONNECTING \n");
					if (g_link_up) {
						VPRINT("sta was connecting - send link up\n");
						slsi_msg_callback_t msg;
						msg.reason = reason;
						slsi_send_mqueue(SLSI_CALLBACK_LINK_UP, &msg);
					}
					break;
				case SLSI_WIFIAPI_STATE_AP_CONNECTED:
					if (g_link_down) {
						VPRINT("ap was connected - send link_down\n");
						slsi_msg_callback_t msg;
						msg.reason = reason;
						slsi_send_mqueue(SLSI_CALLBACK_LINK_DOWN, &msg);
					}
					break;
				case SLSI_WIFIAPI_STATE_AP_ENABLING:
					VPRINT("ap was enabling \n");
					break;
				case SLSI_WIFIAPI_STATE_AP_DISABLING:
					VPRINT("ap was disabling \n");
					break;
				default:
					DPRINT("Recover current state: %s - recovering from: %s\n", slsi_state_strings[g_state], slsi_state_strings[g_recovery_data.old_state]);
					break;
				}
			}

		} else {
			DPRINT("Recover current state: %s - recovering from: %s\n", slsi_state_strings[g_state], slsi_state_strings[g_recovery_data.old_state]);
		}
	}
}

static void slsi_reinitiate_state(void)
{

	// Re-initiate state
	int8_t res_api = SLSI_STATUS_ERROR;
	DPRINT("Re-initiate - old_state: %s\n", slsi_state_strings[g_recovery_data.old_state]);
	switch (g_recovery_data.old_state) {
	case SLSI_WIFIAPI_STATE_NOT_STARTED:
		/* Should not happen - nothing to do! */
		slsi_verify_back();
		break;
	case SLSI_WIFIAPI_STATE_STA_DISCONNECTING:
		g_recovery_data.recovering_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
		res_api = slsi_api_start(SLSI_WIFI_STATION_IF, NULL);
		DPRINT("WiFiStart 0 returned result=%d \n", res_api);
		if (res_api != SLSI_STATUS_SUCCESS) {
			EPRINT("Not able to start Wi-Fi 0! \n");
		}
		slsi_verify_recovered();
		break;
	case SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING:
		/* Setup mode as station */
		res_api = slsi_api_start(SLSI_WIFI_STATION_IF, NULL);
		DPRINT("WiFiStart 1 returned result=%d \n", res_api);
		if (res_api != SLSI_STATUS_SUCCESS) {
			EPRINT("Not able to start Wi-Fi 1! \n");
		}
		slsi_verify_back();
		break;
	case SLSI_WIFIAPI_STATE_AP_CONNECTED:
	/* fall through */
	case SLSI_WIFIAPI_STATE_AP_ENABLING:
		g_recovery_data.recovering_state = SLSI_WIFIAPI_STATE_AP_ENABLED;
	/* fall through */
	case SLSI_WIFIAPI_STATE_AP_ENABLED: {
		/* Setup mode */
		slsi_ap_config_t *pl_ap_config = slsi_get_ap_config();
		res_api = slsi_api_start(g_recovery_data.old_interface_type, pl_ap_config);
		free(pl_ap_config);
		DPRINT("WiFiStart 2 returned result=%d \n", res_api);
		if (res_api != SLSI_STATUS_SUCCESS) {
			EPRINT("Not able to start Wi-Fi 2! \n");
		}
		break;
	}
	case SLSI_WIFIAPI_STATE_STA_CONNECTING:
		g_recovery_data.recovering_state = SLSI_WIFIAPI_STATE_STA_CONNECTED;
	/* fall through */
	case SLSI_WIFIAPI_STATE_STA_CONNECTED:
		res_api = slsi_api_start(g_recovery_data.old_interface_type, NULL);
		DPRINT("WiFiStart returned result=%d \n", res_api);
		if (res_api == SLSI_STATUS_SUCCESS) {
			DPRINT("Restarted Wi-Fi - initiate Join! \n");
			VPRINT("Saved ssid: %s\n", g_recovery_data.ssid);
			VPRINT("Saved bssid: %s\n", g_recovery_data.bssid);

			if (g_recovery_data.security) {
				VPRINT("Saved security settings: security mode %d\n", g_recovery_data.security->secmode);
			}
			res_api = slsi_join_network(g_recovery_data.ssid, g_recovery_data.ssid_len, g_recovery_data.bssid, g_recovery_data.security);
//                    WiFiNetworkJoin(g_recovery_data.ssid, g_recovery_data.ssid_len,
//                    g_recovery_data.bssid, g_recovery_data.security);
			if (res_api != SLSI_STATUS_SUCCESS) {
				EPRINT("Not able to join network! \n");
			}
		} else {
			EPRINT("Not able to start Wi-Fi 4! \n");
		}
		break;
	case SLSI_WIFIAPI_STATE_AP_DISABLING:
	/* fall through */
	case SLSI_WIFIAPI_STATE_TERMINATING:
		/* Assuming we are shutting down */
		g_state = SLSI_WIFIAPI_STATE_NOT_STARTED;
		slsi_verify_back();
		break;
	default:
		break;
	}
}

void slsi_recovery_thread_handler(void *param)
{

	g_recovery_running = 1;
	sem_init(&g_sem_recover, 0, 0);

	while (g_recovery_running) {
		sem_wait(&g_sem_recover);
		DPRINT("Recover semaphore released\n");
		// Re-initiate state
		if (g_recovering) {
			slsi_reinitiate_state();
		}
	}
	sem_destroy(&g_sem_recover);

	VPRINT("SLSI_API pthread_exit %d \n", g_recovery_thread);
	pthread_detach(g_recovery_thread);
	pthread_exit(0);
	g_recovery_thread = 0;
}

static bool slsi_recovery_handler(const char *str)
{

	bool handled = FALSE;

	if (slsi_event_received(str, WPA_EVENT_HANGED) && g_state != SLSI_WIFIAPI_STATE_TERMINATING) {
		EPRINT("SLSI-RECOVERY-EVENT received\n");
		// Initial entering here - start recovering handling
		LOCKUNLOCK_CRITICAL;
		VPRINT("Current state: %s\n", slsi_state_strings[g_state]);
		g_recovery_data.old_state = g_state;
		g_recovery_data.recovering_state = g_state;
		handled = TRUE;
		g_state = SLSI_WIFIAPI_STATE_RECOVERING;
		if (slsi_terminate_supplicant() != SLSI_STATUS_SUCCESS) {
			EPRINT("Not able to terminate supplicant! \n");
		}
	}
	return handled;
}
#endif							// CONFIG_SCSC_WLAN_AUTO_RECOVERY

void slsi_callback_thread_handler(void *param)
{
	g_callback_running = 1;
	FAR char msg_buffer[SLSI_CALLBACK_MGS_SIZE];

	struct mq_attr attr;
	attr.mq_maxmsg = SLSI_CALLBACK_MGS_COUNT;
	attr.mq_msgsize = SLSI_CALLBACK_MGS_SIZE;
	attr.mq_flags = 0;

	int nbytes;

	g_recv_cbmqfd = mq_open(SLSI_CALLBACK_MQUEUE, O_RDONLY | O_CREAT, 0666, &attr);
	if (g_recv_cbmqfd == (mqd_t) ERROR) {
		EPRINT("open mqueue failed, errno: %d\n", errno);
		pthread_exit((pthread_addr_t) 1);
	} else {
		while (g_callback_running) {
			nbytes = mq_receive(g_recv_cbmqfd, msg_buffer, SLSI_CALLBACK_MGS_SIZE, 0);
			if (nbytes < 0) {
				EPRINT("mqueue receive failed, errno: %d\n", errno);
			} else if (nbytes != attr.mq_msgsize) {
				EPRINT("mqueue wrong num bytes received: %d, errno: %d\n", nbytes, errno);
			} else if (nbytes == attr.mq_msgsize) {

				switch ((slsi_callback_t) msg_buffer[0]) {
				case SLSI_CALLBACK_SHUTDOWN: {
					DPRINT("SLSI_CALLBACK_SHUTDOWN event received \n");
					g_callback_running = 0;
					break;
				}
				case SLSI_CALLBACK_LINK_UP: {
					slsi_reason_t tmp_reason;
					memcpy(&tmp_reason, &msg_buffer[sizeof(slsi_callback_t)], sizeof(slsi_reason_t));
					if (g_link_up) {
						DPRINT("SLSI_CALLBACK_LINK_UP \n");
						g_link_up(&tmp_reason);
					}
					break;
				}
				case SLSI_CALLBACK_LINK_DOWN: {
					slsi_reason_t tmp_reason;
					memcpy(&tmp_reason, &msg_buffer[sizeof(slsi_callback_t)], sizeof(slsi_reason_t));
					if (g_link_down) {
						DPRINT("SLSI_CALLBACK_LINK_DOWN \n");
						g_link_down(&tmp_reason);
					}
					break;
				}
				case SLSI_CALLBACK_SCAN_RESULT: {
					if (g_scan_result_handler) {
						slsi_reason_t reason;
						memset(&reason, 0, sizeof(slsi_reason_t));
						DPRINT("SLSI_CALLBACK_SCAN_RESULT \n");
						g_scan_result_handler(&reason);
						g_scan_result_handler = NULL;
					}
#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
					g_recovery_data.scan_result_handler = NULL;
#endif
					break;
				}
#ifdef CONFIG_SLSI_WIFI_P2P_API
				case SLSI_CALLBACK_P2P_FOUND: {
					slsi_find_info_t tmp_find_info;
					memcpy(&tmp_find_info, &msg_buffer[sizeof(slsi_callback_t)], sizeof(slsi_find_info_t));
					if (g_find_results_handler) {
						DPRINT("SLSI_CALLBACK_P2P_FOUND \n");
						g_find_results_handler(&tmp_find_info);
					}
					break;
				}
				case SLSI_CALLBACK_P2P_LINK_UP: {
					slsi_reason_t tmp_reason;
					memcpy(&tmp_reason, &msg_buffer[sizeof(slsi_callback_t)], sizeof(slsi_reason_t));
					if (g_p2p_link_up) {
						DPRINT("SLSI_CALLBACK_P2P_LINK_UP \n");
						g_p2p_link_up(&tmp_reason);
					}
					break;
				}
				case SLSI_CALLBACK_P2P_LINK_DOWN: {
					slsi_reason_t tmp_reason;
					memcpy(&tmp_reason, &msg_buffer[sizeof(slsi_callback_t)], sizeof(slsi_reason_t));
					if (g_p2p_link_down) {
						DPRINT("SLSI_CALLBACK_P2P_LINK_DOWN \n");
						g_p2p_link_down(&tmp_reason);
					}
					break;
				}
#endif
				default:
					EPRINT("Message unknown: %d\n", msg_buffer);
					break;
				}
			}
		}
	}
	/* Close callback receiver mqueue */
	if (mq_close(g_recv_cbmqfd) < 0) {
		EPRINT("receiver_thread: ERROR mq_close failed\n");
	} else {
		DPRINT("Closed g_recv_cbmqfd mqueue \n");
		g_recv_cbmqfd = NULL;
	}

	VPRINT("SLSI_API pthread_exit %d \n", g_callback_thread);
	pthread_detach(g_callback_thread);
	pthread_exit(0);
}

static uint8_t slsi_open_sender_mqueue(void)
{
	uint8_t result = SLSI_STATUS_ERROR;
	struct mq_attr attr;

	/* Fill in attributes for message queue */
	attr.mq_maxmsg = SLSI_CALLBACK_MGS_COUNT;
	attr.mq_msgsize = SLSI_CALLBACK_MGS_SIZE;
	attr.mq_flags = 0;

	g_send_cbmqfd = mq_open(SLSI_CALLBACK_MQUEUE, O_WRONLY | O_CREAT, 0666, &attr);
	if (g_send_cbmqfd == (mqd_t) ERROR) {
		EPRINT("open mqueue failed, errno: %d\n", errno);
	} else {
		result = SLSI_STATUS_SUCCESS;
	}
	return result;
}

uint8_t slsi_send_mqueue(slsi_callback_t event, slsi_msg_callback_t *msg)
{
	uint8_t result = SLSI_STATUS_ERROR;
	FAR char msg_buffer[SLSI_CALLBACK_MGS_SIZE];
	memset(msg_buffer, 0, SLSI_CALLBACK_MGS_SIZE);
	memcpy(msg_buffer, (void *)&event, sizeof(slsi_callback_t));
	if (msg) {
		memcpy(msg_buffer + sizeof(slsi_callback_t), (void *)msg, sizeof(slsi_msg_callback_t));
	}

	int status = mq_send(g_send_cbmqfd, msg_buffer, SLSI_CALLBACK_MGS_SIZE, 42);
	if (status < 0) {
		EPRINT("send message: ERROR mq_send failure=%d\n", errno);
	} else {
		result = SLSI_STATUS_SUCCESS;
	}
	return result;
}

void slsi_monitor_thread_handler(void *param)
{
	g_running = 1;
	char *result = NULL;
	uint8_t join_count = 0;

	if (slsi_open_sender_mqueue() != SLSI_STATUS_SUCCESS) {
		g_running = 0;
	}

	while (g_running) {
		if (slsi_recv_pending(&result) && (result != NULL)) {
			char *tmp = result;
			slsi_reason_t reason;
			memset(&reason, 0, sizeof(slsi_reason_t));
			DPRINT("SLSI_API event - %s\n", result);
			result = strchr(result, '>');
			if (result == NULL) {
				if (tmp) {
					free(tmp);
					tmp = NULL;
				}
				continue;
			}
			result++;
#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
			if (slsi_recovery_handler(result) == FALSE)
#endif
			{
#ifdef CONFIG_SLSI_WIFI_P2P_API
				if (g_state == SLSI_WIFIAPI_STATE_P2P) {
					slsi_p2p_event_handle(result);
				} else {
#endif

					// Checking events received
					if (g_scanning) {
						slsi_scan_event_handler(result, WPA_EVENT_SCAN_RESULTS);
					}
					VPRINT("Switch - current state: %s\n", slsi_state_strings[g_state]);
					switch (g_state) {
					case SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING:
						// Handling reconnect after remote disconnect
						if (slsi_event_received(result, WPA_EVENT_CONNECTED)) {
							slsi_check_status(reason.ssid, &reason.ssid_len, reason.bssid);
							slsi_get_network(reason.ssid, reason.ssid_len, &g_network_id);
							g_state = SLSI_WIFIAPI_STATE_STA_CONNECTED;
							slsi_set_scan_interval(SLSI_SCAN_INTERVAL);	// connected so lets set scan interval back and save power
							if (g_link_up) {
								VPRINT("SLSI_API send link_up\n");
								slsi_msg_callback_t msg;
								msg.reason = reason;
								slsi_send_mqueue(SLSI_CALLBACK_LINK_UP, &msg);
								//g_link_up(&reason);
							}
						}
						// To ensure join counter is reset in switch mode
						join_count = 0;
						break;
					case SLSI_WIFIAPI_STATE_AP_ENABLING:
						// AP mode setup and select network
						if (slsi_event_received(result, AP_EVENT_ENABLED)) {
							g_state = SLSI_WIFIAPI_STATE_AP_ENABLED;
							sem_post(&g_sem_ap_mode);
						} else if (slsi_event_received(result, AP_EVENT_DISABLED)) {
							// The network setup failed
							g_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
							sem_post(&g_sem_ap_mode);
						} else {
							VPRINT("Info: Event not handled %s in current state %s\n", result, slsi_state_strings[g_state]);
						}
						break;
					case SLSI_WIFIAPI_STATE_AP_ENABLED:
						if (slsi_event_received(result, AP_STA_CONNECTED)) {
							g_num_sta_connected++;
							if (g_num_sta_connected == 1) {
								g_state = SLSI_WIFIAPI_STATE_AP_CONNECTED;
							}
							result += sizeof(AP_STA_CONNECTED) - 1;
							if (strlen(result) >= 17) {	// bssid is a 17 character string
								memcpy(&(reason.bssid), result, 17);	// Exclude null-termination
							}
							if (g_link_up) {
								VPRINT("SLSI_API slsi_link_event_handler send link_up\n");
								slsi_msg_callback_t msg;
								msg.reason = reason;
								slsi_send_mqueue(SLSI_CALLBACK_LINK_UP, &msg);
								//g_link_up(&reason);
							}
						} else {
							VPRINT("Info: Event not handled %s in current state %s\n", result, slsi_state_strings[g_state]);
						}
						break;
					case SLSI_WIFIAPI_STATE_AP_CONNECTED:
						if (slsi_event_received(result, AP_STA_DISCONNECTED)) {
							g_num_sta_connected--;
							if (g_num_sta_connected == 0) {
								g_state = SLSI_WIFIAPI_STATE_AP_ENABLED;
							}
							result += sizeof(AP_STA_DISCONNECTED) - 1;	// Exclude null-termination
							if (strlen(result) >= 17) {	// bssid is a 17 character string
								memcpy(&(reason.bssid), result, 17);
							}
							result = strstr(result, WPA_PARAM_REASON_CODE);
							if (result != NULL) {
								reason.reason_code = (uint32_t) strtol(result + strlen(WPA_PARAM_REASON_CODE), NULL, 10);
							}
							if (g_link_down) {
								VPRINT("SLSI_API slsi_handle_disconnect send link_down\n");
								slsi_msg_callback_t msg;
								msg.reason = reason;
								slsi_send_mqueue(SLSI_CALLBACK_LINK_DOWN, &msg);
							}
						} else {
							VPRINT("Info: Event not handled %s in current state %s\n", result, slsi_state_strings[g_state]);
						}
						break;
					case SLSI_WIFIAPI_STATE_AP_DISABLING:
						// AP stop sent
						if (slsi_event_received(result, AP_EVENT_DISABLED)) {
							g_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
							sem_post(&g_sem_ap_mode);
						} else if (slsi_event_received(result, AP_EVENT_ENABLED)) {
							g_state = SLSI_WIFIAPI_STATE_AP_ENABLED;
							sem_post(&g_sem_ap_mode);
						} else if (slsi_event_received(result, AP_STA_DISCONNECTED)) {
							g_num_sta_connected--;
							result += sizeof(AP_STA_DISCONNECTED) - 1;	// Exclude null-termination
							if (strlen(result) >= 17) {	// bssid is a 17 character string
								memcpy(&(reason.bssid), result, 17);
							}
							result = strstr(result, WPA_PARAM_REASON_CODE);
							if (result != NULL) {
								reason.reason_code = (uint32_t) strtol(result + strlen(WPA_PARAM_REASON_CODE), NULL, 10);
							}
							if (g_link_down) {
								VPRINT("SLSI_API slsi_handle_disconnect send link_down\n");
								slsi_msg_callback_t msg;
								msg.reason = reason;
								slsi_send_mqueue(SLSI_CALLBACK_LINK_DOWN, &msg);
							}
						} else {
							VPRINT("Info: Event not handled %s in current state %s\n", result, slsi_state_strings[g_state]);
						}
						break;
					case SLSI_WIFIAPI_STATE_STA_CONNECTING: {
						bool event_handled = FALSE;
						if (slsi_event_received(result, WPA_EVENT_CONNECTED)) {
							slsi_check_status(reason.ssid, &reason.ssid_len, reason.bssid);
							g_state = SLSI_WIFIAPI_STATE_STA_CONNECTED;
							// connected so lets set scan interval back to limit power consumption
							slsi_set_scan_interval(SLSI_SCAN_INTERVAL);
							event_handled = TRUE;
						} else if (slsi_event_received(result, WPA_EVENT_NETWORK_NOT_FOUND)) {
							/* Assumed to be because network with specification setup is not
							 * found in scan results - handle as error - disable network */
							if (join_count == SLSI_STA_JOIN_SCAN_ATTEMPT) {
								reason.reason_code = SLSI_REASON_NETWORK_CONFIGURATION_NOT_FOUND;
								event_handled = TRUE;
							} else {
								join_count++;
							}
						} else if (slsi_event_received(result, WPA_EVENT_TEMP_DISABLED)) {
							reason.reason_code = SLSI_REASON_NETWORK_AUTHENTICATION_FAILED;
							event_handled = TRUE;
						} else if (slsi_event_received(result, SLSI_EVENT_ASSOCIATION_REQ_FAILED)) {
							reason.reason_code = SLSI_REASON_ASSOCIATION_REQ_FAILED;
							event_handled = TRUE;
						} else if (slsi_event_received(result, WPA_EVENT_DISCONNECTED)) {
							reason.reason_code = SLSI_REASON_ASSOCIATION_REQ_FAILED;
							event_handled = TRUE;
						} else {
							VPRINT("Info: Event not handled %s in current state %s\n", result, slsi_state_strings[g_state]);
						}
						if (event_handled) {
							join_count = 0;
							if (reason.reason_code) {
								VPRINT("reason.reason_code=%d\n", reason.reason_code);
								// Connection failed change back state
								g_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
								if (g_network_id) {
									slsi_remove_network(g_network_id);
									free(g_network_id);
									g_network_id = NULL;
								}
							}
							if (g_link_up) {
								VPRINT("SLSI_API slsi_link_event_handler send link_up\n");
								slsi_msg_callback_t msg;
								msg.reason = reason;
								slsi_send_mqueue(SLSI_CALLBACK_LINK_UP, &msg);
								//g_link_up(&reason);
							}
						}
						break;
					}
					case SLSI_WIFIAPI_STATE_STA_CONNECTED:
						if (slsi_event_received(result, WPA_EVENT_DISCONNECTED)) {
							slsi_sta_disconnect_event_handler(result, &reason);
							g_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
							if (g_link_down) {
								VPRINT("SLSI_API slsi_link_event_handler send link_down\n");
								slsi_msg_callback_t msg;
								msg.reason = reason;
								slsi_send_mqueue(SLSI_CALLBACK_LINK_DOWN, &msg);
							}
							if (g_network_id) {
								free(g_network_id);
								g_network_id = NULL;
							}
							// TODO: clean join info for recovery
						} else {
							VPRINT("Info: Event not handled %s in current state %s\n", result, slsi_state_strings[g_state]);
						}
						break;
					case SLSI_WIFIAPI_STATE_STA_DISCONNECTING:
						if (slsi_event_received(result, WPA_EVENT_DISCONNECTED)) {
							slsi_sta_disconnect_event_handler(result, &reason);
							g_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
							/* start by disabling all previous networks to make sure
							 * they are not running simultaneous */
							uint8_t tmp_result = slsi_disable_all_networks();
							if (tmp_result != SLSI_STATUS_SUCCESS) {
								EPRINT("disable networks - failed\n");
							}
							if (g_network_id) {
								slsi_remove_network(g_network_id);
								free(g_network_id);
								g_network_id = NULL;
							}
							// Release sem_wait after finished removing the network
							VPRINT("Before post sem count %d\n", g_sem_disconnect.semcount);
							sem_post(&g_sem_disconnect);
							if (g_link_down) {
								VPRINT("SLSI_API slsi_link_event_handler send link_down\n");
								slsi_msg_callback_t msg;
								msg.reason = reason;
								slsi_send_mqueue(SLSI_CALLBACK_LINK_DOWN, &msg);
							}
							// TODO: clean join info for recovery
						} else {
							VPRINT("Info: Event not handled %s in current state %s\n", result, slsi_state_strings[g_state]);
						}
						break;
					case SLSI_WIFIAPI_STATE_TERMINATING:
						if (slsi_event_received(result, WPA_EVENT_TERMINATING)) {
							VPRINT("WPA_EVENT_TERMINATING Received\n");
							g_running = 0;
							sem_post(&g_sem_terminate);
						} else {
							VPRINT("Info: Event not handled %s in current state %s\n", result, slsi_state_strings[g_state]);
						}
						break;
					case SLSI_WIFIAPI_STATE_RECOVERING:
						if (slsi_event_received(result, WPA_EVENT_TERMINATING) == TRUE) {
							// Secondary entry here - continue shut down and re-initiate
							VPRINT("WPA_EVENT_TERMINATING Received in recover handler \n");
							pid_t r_task_id = -1;
							int status = -1;
							UNUSED(status);
							DPRINT("Wait for supplicant task to terminate task id=%d \n", g_task);
							r_task_id = waitpid(g_task, &status, NULL);

							if (r_task_id != g_task) {
								DPRINT("    result: %d, status: %d\n", r_task_id, status);
								VPRINT("Error waiting for task termination - sleep and continue \n");
							}
							// if monitor still running - stop running
							if (g_running) {
								VPRINT(" stop running\n");
								// Will end while loop in monitoring thread and exit thread
								g_running = 0;
							}
							// Disconnect wpa_ctrl connection
							if (slsi_wpa_close(TRUE) != SLSI_STATUS_SUCCESS) {
								EPRINT("Error: still running after disconnect. \n");
							}
							// CLean up local
							slsi_deinit();
#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
							g_recovering = 1;
							sem_post(&g_sem_recover);
#endif
						} else {
							VPRINT("Info: Event not handled %s in current state %s\n", result, slsi_state_strings[g_state]);
						}
						break;

					default:
						EPRINT("Should not happen as the state is not valid\n");
						break;
					}
#ifdef CONFIG_SLSI_WIFI_P2P_API
				}
#endif
			}
			if (tmp) {
				free(tmp);
				tmp = NULL;
			}
#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
			slsi_verify_recovered();
#endif
		}
	}
	/* Close callback sender mqueue */
	if (g_send_cbmqfd) {
		if (mq_close(g_send_cbmqfd) < 0) {
			EPRINT("close mqueue failed: %d\n", errno);
		} else {
			DPRINT("Closed g_send_cbmqfd mqueue \n");
			g_send_cbmqfd = NULL;
		}
	}

	VPRINT("SLSI_API pthread_exit %d \n", g_monitoring_thread);
	pthread_detach(g_monitoring_thread);
	pthread_exit(0);
}

/* Receive monitor message */
static int8_t slsi_recv_pending(char **result)
{
	if (g_ctrl_conn == NULL) {
		if (g_running) {
			slsi_wpa_reopen();
		}
		return 0;
	}
	while (wpa_ctrl_pending(g_ctrl_conn) > 0) {
		size_t len = WPA_BUFFER_SIZE - 1;
		char *buf = zalloc(len + 1);
		if (buf == NULL) {
			return 0;
		} else {
			if (wpa_ctrl_recv_monitor(g_ctrl_conn, buf, &len) == 0) {
				buf[len] = '\0';
				*result = buf;
				DPRINT("slsi_recv_pending received: %d bytes\n", len);
				return 1;
			} else {
				EPRINT("SLSI_API Could not read pending message.\n");
				UNUSED(buf);	//prevent lint warning
				if (buf) {
					free(buf);
					// prevent lint warning so no buf = NULL;
				}
			}
		}
	}
	if (wpa_ctrl_pending(g_ctrl_conn) < 0) {
		if (g_running) {
			slsi_wpa_reopen();
		}
		return 0;
	}
	return 0;
}

static int8_t slsi_wpa_open(char *network_path)
{
	int8_t result = SLSI_STATUS_ERROR;
	/* The control connection */
	VPRINT("SLSI_API wpa_ctrl_connect before open\n");
	g_ctrl_conn = wpa_ctrl_open(network_path);
	if (g_ctrl_conn == NULL) {
		EPRINT("SLSI_API Failed to connect to interface: %s \n", g_active_ifname);
		return result;
	} else {
		VPRINT("SLSI_API wpa_ctrl_connect success\n");
	}
	/* The monitor connection and thread setup */
	if (wpa_ctrl_attach(g_ctrl_conn) == 0) {
		g_wpa_attached = 1;
		VPRINT("SLSI_API wpa_ctrl_attach success\n");
	} else {
		EPRINT("SLSI_API Failed to open monitor connection " "through control interface\n");
		return result;
	}
	if (pthread_create(&g_monitoring_thread, NULL, (void *)slsi_monitor_thread_handler, NULL)) {
		EPRINT("SLSI_API Failed to create thread\n");
	} else {
		result = SLSI_STATUS_SUCCESS;
		pthread_setname_np(g_monitoring_thread, "Wi-Fi API monitor");
		VPRINT("SLSI_API Monitoring thread created successfully\n");
	}

	return result;
}

static int8_t slsi_wpa_close(bool terminate)
{
	int8_t result = SLSI_STATUS_ERROR;

	if (g_ctrl_conn != NULL) {
		if (g_wpa_attached && !terminate) {
			// If we are terminating the supplicant it is not possible to send detach
			if (wpa_ctrl_detach(g_ctrl_conn) == 0) {
				VPRINT("SLSI_API wpa_ctrl_detach success\n");
			}
			g_wpa_attached = 0;
			VPRINT("SLSI_API g_mon_conn closed\n");
		}
		wpa_ctrl_close(g_ctrl_conn);
		g_ctrl_conn = NULL;
		VPRINT("SLSI_API g_ctrl_conn closed\n");
		result = SLSI_STATUS_SUCCESS;
	} else {
		VPRINT("SLSI_API wpa_ctrl iface already closed!\n");
	}

	return result;
}

static void slsi_wpa_reopen(void)
{
	VPRINT("SLSI_API Trying to reconnect...\n");
	(void)slsi_wpa_close(FALSE);
	(void)slsi_wpa_open(g_active_ifname);
}

static int8_t slsi_get_network(uint8_t *ssid, uint8_t ssid_len, char **network_id)
{
	int8_t result = SLSI_STATUS_ERROR;
	char *pbuf = NULL;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };
	char ssid_formated[WPA_MAX_SSID_LEN];
	printf_encode(ssid_formated, WPA_MAX_SSID_LEN - 1, ssid, ssid_len);
	// List networks to see if already available
	snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", WPA_COMMAND_LIST_NETWORKS);
	VPRINT("SLSI_API get_network list: %s\n", command);
	pbuf = slsi_send_request(NULL, command, NULL);
	if (pbuf != NULL) {
		VPRINT("%s", pbuf);
	}
	if (pbuf) {
		char *pos, *tab, *name;
		pos = strchr(pbuf, '\n');
		if (pos == NULL) {
			goto errout;
		}
		pos = pos + 1;
		// Find network id if available
		name = strstr(pos, ssid_formated);
		if (name) {
			VPRINT("SLSI_API existing network found\n");
			tab = name - 1;
			*tab = '\0';		// Replace tab with END-OF-STRING
			pos = name - 2;		// NOTE: this only work if the ID is 2 digits or less.
			if (strncmp((pos - 1), "\n", 1) == 0) {
				// One digit ID
				*network_id = strdup(pos);
			} else if (strncmp((pos - 2), "\n", 1) == 0) {
				// Two digit ID
				*network_id = strdup(pos - 1);
			} else {
				EPRINT("SLSI_API - no newline - should not happen\n");
				goto errout;
			}
		} else {
			goto errout;
		}
		result = SLSI_STATUS_SUCCESS;
		if (pbuf) {
			free(pbuf);
			pbuf = NULL;
		}
	}
	return result;
errout:
	if (pbuf) {
		free(pbuf);
		pbuf = NULL;
	}
	return result;
}

static int8_t slsi_check_status(uint8_t *ssid, int8_t *ssid_len, char *bssid)
{
	int8_t result = SLSI_STATUS_ERROR;
	char *pbuf = NULL;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };
	char *pos, *end;
	*ssid_len = 0;

	// Check state of network possibly already connected
	snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", WPA_COMMAND_STATUS);
	VPRINT("SLSI_API check_status select: %s\n", command);
	pbuf = slsi_send_request(NULL, command, NULL);
	if (pbuf) {
		VPRINT("  Response: %s", pbuf);
		pos = strstr(pbuf, WPA_PARAM_WPA_STATE);
		if (pos == NULL) {
			goto errout;
		}
		pos += 10;
		if (str_starts(pos, WPA_STATE_DISCONNECTED)) {
			//g_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
		} else if (str_starts(pos, WPA_STATE_COMPLETED)) {
			pos = strstr(pbuf, WPA_PARAM_BSSID);
			if (pos != NULL) {
				pos += 6;
				end = strchr(pos, '\n');
				if (end == NULL) {
					goto errout;
				}
				*end = '\0';
				if (bssid != NULL) {
					memcpy(bssid, pos, 18);    // Including '\0'
				}
				pos = end + 1;
				/* NOTE as ssid is a substring of bssid, we need to skip past the bssid entry */
				pos = strstr(pos, WPA_PARAM_SSID);
			}
			if (pos != NULL) {
				pos += 5;
				end = strchr(pos, '\n');
				if (end == NULL) {
					goto errout;
				}
				*end = '\0';
				if (ssid != NULL) {
					*ssid_len = printf_decode(ssid, SLSI_SSID_LEN + 1, pos);
				}
			}
		} else {				//if(str_starts(pos,WPA_STATE_SCANNING)) {

		}
		result = SLSI_STATUS_SUCCESS;
		if (pbuf) {
			free(pbuf);
			pbuf = NULL;
		}
	}
	return result;

errout:
	if (pbuf) {
		free(pbuf);
		pbuf = NULL;
	}
	return result;
}

static int8_t slsi_set_security(const slsi_security_config_t *sec_config, const char *network_id)
{
	int8_t result = SLSI_STATUS_SECURITY_FAILED;
	// Set security for both AP mode and STA mode
	char *keymgmt = WPA_PARAM_KEY_MGMT_NONE;
	char *authalg = WPA_VALUE_AUTH_ALG_OPEN;

	if (sec_config == NULL) {
		slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SET_NETWORK, (char *)network_id, WPA_PARAM_KEY_MGMT_NONE, NULL, &result);
		if (result != SLSI_STATUS_SUCCESS) {
			goto errout;
		}
	} else {
		VPRINT("SLSI_API setup network with security settings: " "security mode %d\n", sec_config->secmode);

		if (sec_config->secmode == SLSI_SEC_MODE_OPEN) {
			keymgmt = WPA_PARAM_KEY_MGMT_NONE;
		} else if (sec_config->secmode == SLSI_SEC_MODE_WEP || sec_config->secmode == SLSI_SEC_MODE_WEP_SHARED) {
			if (slsi_get_op_mode() == SLSI_WIFI_SOFT_AP_IF) {
				DPRINT("SLSI_API set_security - WEP in AP mode - wrong parameter\n");
				result = SLSI_STATUS_PARAM_FAILED;
				goto errout;
			}
			keymgmt = WPA_PARAM_KEY_MGMT_WEP;
		} else if (sec_config->secmode == SLSI_SEC_MODE_EAP) {
			DPRINT("SLSI_API set_security - EAP - wrong parameter\n");
			result = SLSI_STATUS_PARAM_FAILED;
			goto errout;
		} else if (sec_config->secmode == (SLSI_SEC_MODE_WEP | SLSI_SEC_MODE_WEP_SHARED)) {
			DPRINT("SLSI_API set_security - " "WEP OPEN/SHARED cannot be set together - wrong parameter\n");
			result = SLSI_STATUS_PARAM_FAILED;
			goto errout;
		} else if (sec_config->secmode & (SLSI_SEC_MODE_WPA_MIXED | SLSI_SEC_MODE_WPA2_MIXED)) {
			// WPA or WPA2
			char *proto = NULL;
			char *pairwise = NULL;

			keymgmt = WPA_PARAM_KEY_MGMT_WPA_PSK;

			// setting proto
			if ((sec_config->secmode & SLSI_SEC_MODE_WPA_MIXED) && (sec_config->secmode & SLSI_SEC_MODE_WPA2_MIXED)) {
				proto = WPA_VALUE_PROTO_MIXED;
			} else if (sec_config->secmode & SLSI_SEC_MODE_WPA2_MIXED) {
				proto = WPA_VALUE_PROTO_RSN;
			} else {
				proto = WPA_VALUE_PROTO_WPA;
			}

			slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SET_NETWORK, (char *)network_id, WPA_PARAM_PROTO, proto, &result);
			if (result != SLSI_STATUS_SUCCESS) {
				goto errout;
			}
			// setting cipher
			if (sec_config->secmode == SLSI_SEC_MODE_WPA_MIXED || sec_config->secmode == SLSI_SEC_MODE_WPA2_MIXED || sec_config->secmode == (SLSI_SEC_MODE_WPA_MIXED | SLSI_SEC_MODE_WPA2_MIXED)) {
				pairwise = WPA_VALUE_CIPHER_MIXED;
			} else if (sec_config->secmode == SLSI_SEC_MODE_WPA_CCMP || sec_config->secmode == SLSI_SEC_MODE_WPA2_CCMP) {
				pairwise = WPA_VALUE_CIPHER_CCMP;
			} else {
				pairwise = WPA_VALUE_CIPHER_TKIP;
			}
			slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SET_NETWORK, (char *)network_id, WPA_PARAM_PAIRWISE, pairwise, &result);
			if (result != SLSI_STATUS_SUCCESS) {
				goto errout;
			}

			if (slsi_get_op_mode() == SLSI_WIFI_SOFT_AP_IF) {
				slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SET_NETWORK, (char *)network_id, WPA_PARAM_GROUP, pairwise, &result);
				if (result != SLSI_STATUS_SUCCESS) {
					goto errout;
				}
			}
		} else {
			DPRINT("SLSI_API set_security - wrong parameter\n");
			result = SLSI_STATUS_PARAM_FAILED;
			goto errout;
		}

		slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SET_NETWORK, (char *)network_id, keymgmt, NULL, &result);
		if (result != SLSI_STATUS_SUCCESS) {
			goto errout;
		}
		// We always set auth_alg
		if (sec_config->secmode == SLSI_SEC_MODE_WEP_SHARED) {
			authalg = WPA_VALUE_AUTH_ALG_SHARED;
		}
		//We always set auth_alg
		slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SET_NETWORK, (char *)network_id, WPA_PARAM_AUTH_ALG, authalg, &result);
		if (result != SLSI_STATUS_SUCCESS) {
			goto errout;
		}

		if (sec_config->secmode != SLSI_SEC_MODE_OPEN && sec_config->passphrase[0] != '\0') {
			char command[WPA_COMMAND_MAX_SIZE] = { 0 };
			size_t len = strlen(sec_config->passphrase);
			if (sec_config->secmode == SLSI_SEC_MODE_WEP || sec_config->secmode == SLSI_SEC_MODE_WEP_SHARED) {
				// wep_key
				VPRINT("SLSI_API set_security WEP key: %s\n", sec_config->passphrase);
				// Validate length ascii (5 or 13) or hex (10 or 26)
				if (sec_config->passphrase[0] == '"') {
					// ascii specified
					if (len >= SLSI_WIFI_WEP_ASCII_KEY_MIN + 2 && len <= SLSI_WIFI_WEP_ASCII_KEY_MAX + 2) {
						snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %s%s", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_WEPKEY, sec_config->passphrase);
					} else {
						DPRINT("SLSI_API set_security WEP - wrong ASCII key length\n");
						result = SLSI_STATUS_PARAM_FAILED;
						goto errout;
					}
				} else {
					// hex specified
					if (len >= SLSI_WIFI_WEP_HEX_KEY_MIN && len <= SLSI_WIFI_WEP_HEX_KEY_MAX) {
						snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %s%s", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_WEPKEY, sec_config->passphrase);
					} else {
						DPRINT("SLSI_API set_security WEP - wrong HEX key length\n");
						result = SLSI_STATUS_PARAM_FAILED;
						goto errout;
					}
				}
			} else {
				// wpa_key
				VPRINT("SLSI_API set_security WPA key: %s\n", sec_config->passphrase);
				// Validate length ascii (8 or 63)
				if (len >= SLSI_WIFI_WPA_ASCII_KEY_MIN && len <= SLSI_WIFI_WPA_ASCII_KEY_MAX) {
					snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %s\"%s\"", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_PSK, sec_config->passphrase);
				} else {
					DPRINT("SLSI_API set_security WPA - wrong key length\n");
					result = SLSI_STATUS_PARAM_FAILED;
					goto errout;
				}
			}
			char *pbuf = slsi_send_request(NULL, command, &result);
			if (pbuf) {
				free(pbuf);
				pbuf = NULL;
				if (result != SLSI_STATUS_SUCCESS) {
					goto errout;
				}
			}
		} else {
			DPRINT("SLSI_API set_security key missing (or set but not needed)\n");
			goto errout;
		}
	}
errout:
	return result;
}

#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
static slsi_ap_config_t *slsi_get_ap_config(void)
{

	// Clean up structure
	slsi_ap_config_t *p_ap_config = zalloc(sizeof(slsi_ap_config_t));

	if (p_ap_config == NULL) {
		EPRINT("Memory allocation failed \n");
	} else {
		slsi_ap_config_t *tmp_ap_config = &g_recovery_data.ap_config;
		memcpy(p_ap_config, tmp_ap_config, sizeof(slsi_ap_config_t));

		if (tmp_ap_config->security) {
			p_ap_config->security = zalloc(sizeof(slsi_security_config_t));
			if (p_ap_config->security == NULL) {
				EPRINT("Memory allocation failed \n");
			} else {
				memcpy(p_ap_config->security, tmp_ap_config->security, sizeof(slsi_security_config_t));
			}
		}
		if (tmp_ap_config->vsie) {
			p_ap_config->vsie = zalloc(sizeof(slsi_vendor_ie_t));
			if (p_ap_config->vsie == NULL) {
				EPRINT("Memory allocation failed \n");
			} else {
				memcpy(p_ap_config->vsie, tmp_ap_config->vsie, sizeof(slsi_vendor_ie_t));
			}
			p_ap_config->vsie->content = zalloc(tmp_ap_config->vsie->content_length);
			if (p_ap_config->vsie->content == NULL) {
				EPRINT("Memory allocation failed \n");
			} else {
				memcpy(p_ap_config->vsie->content, tmp_ap_config->vsie->content, tmp_ap_config->vsie->content_length);
			}
		}
	}
	return p_ap_config;
}

static void slsi_save_ap_config(slsi_ap_config_t *ap_config)
{

	// Clean up structure
	slsi_ap_config_t *t_ap_config = &g_recovery_data.ap_config;
	if (t_ap_config->security) {
		free(t_ap_config->security);
		t_ap_config->security = NULL;
	}
	if (t_ap_config->vsie) {
		if (t_ap_config->vsie->content) {
			free(t_ap_config->vsie->content);
			t_ap_config->vsie->content = NULL;
		}
		free(t_ap_config->vsie);
		t_ap_config->vsie = NULL;
	}
	memset(&g_recovery_data.ap_config, 0, sizeof(slsi_ap_config_t));

	// Save new config
	memcpy(&g_recovery_data.ap_config, ap_config, sizeof(slsi_ap_config_t));
	if (ap_config->security) {
		t_ap_config->security = zalloc(sizeof(slsi_security_config_t));
		if (t_ap_config->security == NULL) {
			EPRINT("Memory allocation failed \n");
		} else {
			memcpy(t_ap_config->security, ap_config->security, sizeof(slsi_security_config_t));
		}
	}
	if (ap_config->vsie) {
		t_ap_config->vsie = zalloc(sizeof(slsi_vendor_ie_t));
		if (t_ap_config->vsie == NULL) {
			EPRINT("Memory allocation failed \n");
		} else {
			memcpy(t_ap_config->vsie, ap_config->vsie, sizeof(slsi_vendor_ie_t));
		}

		if (ap_config->vsie->content) {
			t_ap_config->vsie->content = zalloc(ap_config->vsie->content_length);
			if (t_ap_config->vsie->content == NULL) {
				EPRINT("Memory allocation failed \n");
			} else {
				memcpy(t_ap_config->vsie->content, ap_config->vsie->content, ap_config->vsie->content_length);
			}
		}
	}
}

static void slsi_save_join(uint8_t *ssid, int ssid_len, uint8_t *bssid, const slsi_security_config_t *sec_config)
{
	// Clean up structure
	if (g_recovery_data.security) {
		free(g_recovery_data.security);
		g_recovery_data.security = NULL;
	}
	memset(&(g_recovery_data.ssid), 0, sizeof(g_recovery_data.ssid));
	memset(&(g_recovery_data.bssid), 0, sizeof(g_recovery_data.bssid));

	// Save new join
	if (ssid) {
		memcpy(&(g_recovery_data.ssid), ssid, sizeof(g_recovery_data.ssid));
		VPRINT("Saved ssid: %s\n", g_recovery_data.ssid);
	}
	g_recovery_data.ssid_len = ssid_len;
	if (bssid) {
		memcpy(&(g_recovery_data.bssid), bssid, sizeof(g_recovery_data.bssid));
		VPRINT("Saved bssid: %s\n", g_recovery_data.bssid);
	}

	if (sec_config) {
		// TODO: security not saved!!!
		g_recovery_data.security = zalloc(sizeof(slsi_security_config_t));
		if (g_recovery_data.security == NULL) {
			EPRINT("Memory allocation failed \n");
		} else {
			memcpy(g_recovery_data.security, sec_config, sizeof(slsi_security_config_t));
			VPRINT("Saved security settings: security mode %d\n", g_recovery_data.security->secmode);
		}
	}
}
#endif

static int8_t slsi_join_network(uint8_t *ssid, int ssid_len, uint8_t *bssid, const slsi_security_config_t *sec_config)
{
	int8_t result = SLSI_STATUS_ERROR;
	char *pbuf = NULL;
	char *network_id = NULL;

	VPRINT("SLSI_API join_network setup network with ssid %s\n", ssid);
	if (sec_config) {
		VPRINT("SLSI_API join_network setup network with security settings: " "security mode %d\n", sec_config->secmode);
	}
	// Find network or add new network
	result = slsi_get_network(ssid, ssid_len, &network_id);
	if ((result != SLSI_STATUS_SUCCESS) && (network_id == NULL) /* Attempt to make SVACE happy */) {
		// Add new network
		char ssid_formatted[WPA_MAX_SSID_LEN];
		char command[WPA_COMMAND_MAX_SIZE] = { 0 };
		snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", WPA_COMMAND_ADD_NETWORK);
		pbuf = slsi_send_request(NULL, command, NULL);
		if (pbuf) {
			pbuf[strcspn(pbuf, "\r\n")] = '\0';
			network_id = strdup(pbuf);
			free(pbuf);
			pbuf = NULL;
		} else {
			goto errout;
		}

		// Set network ssid
		printf_encode(ssid_formatted, WPA_MAX_SSID_LEN, ssid, ssid_len);
		memset(command, 0, WPA_COMMAND_MAX_SIZE);
		snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %sP\"%s\"", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_SSID_, ssid_formatted);
		pbuf = slsi_send_request(NULL, command, &result);
		if (pbuf) {
			free(pbuf);
			// removed to prevent lint warning pbuf = NULL;
			if (result != SLSI_STATUS_SUCCESS) {
				goto errout;
			}
		}
		// Set scan_ssid which scans for APs using hidden SSIDs.
		memset(command, 0, WPA_COMMAND_MAX_SIZE);
		snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %s%d", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_SCAN_SSID, 1);
		pbuf = slsi_send_request(NULL, command, &result);
		if (pbuf) {
			free(pbuf);
			// removed to prevent lint warning pbuf = NULL;
			if (result != SLSI_STATUS_SUCCESS) {
				goto errout;
			}
		}
		// Set network bssid if available
		if (bssid != NULL && bssid[0] != '\0') {
			slsi_send_command_str(NULL, &result, "%s%s %s%s", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_BSSID_, (char *)bssid);
			if (result != SLSI_STATUS_SUCCESS) {
				goto errout;
			}
		}
	}
	// Set security
	result = slsi_set_security(sec_config, network_id);
	if (result != SLSI_STATUS_SUCCESS) {
		// remove network
		slsi_send_command_str_upto_4(NULL, WPA_COMMAND_REMOVE_NETWORK, network_id, NULL, NULL, NULL);
	} else {
		// Select network (and disable other networks)
		g_state = SLSI_WIFIAPI_STATE_STA_CONNECTING;
		slsi_set_scan_interval(SLSI_SCAN_INTERVAL_CONNECT);	//set more agressive scan interval for connections
		slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SELECT_NETWORK, network_id, NULL, NULL, &result);
		if (result != SLSI_STATUS_SUCCESS) {
			g_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
			goto errout;
		}
		if (g_network_id) {
			free(g_network_id);
			g_network_id = NULL;
		}
		g_network_id = strdup(network_id);
	}

errout:
	if (network_id) {
		free(network_id);
		network_id = NULL;
	}

	return result;
}

static void slsi_set_bss_expiration(void)
{
	slsi_send_command_str_digit(NULL, WPA_COMMAND_BSS_EXPIRE_AGE, SLSI_BSS_EXPIRE_AGE, NULL);
}

static void slsi_set_scan_interval(uint8_t interval)
{
	if (interval < SLSI_SCAN_INTERVAL_MIN) {
		interval = SLSI_SCAN_INTERVAL_MIN;
	} else if (interval > SLSI_SCAN_INTERVAL_MAX) {
		interval = SLSI_SCAN_INTERVAL_MAX;
	}
	slsi_send_command_str_digit(NULL, WPA_COMMAND_SCAN_INTERVAL, interval, NULL);
}

static void slsi_set_autoconnect(uint8_t onoff)
{
	slsi_send_command_str_digit(NULL, WPA_COMMAND_AUTOCONNECT, onoff, NULL);
}

#ifdef CONFIG_SLSI_WIFI_P2P_LISTEN_CHANNEL
static void slsi_set_p2p_channel(uint8_t channel)
{
	if (channel == 1 || channel == 6 || channel == 11) {
		char buf[2];
		(void)slsi_p2p_set(WPA_PARAM_P2P_LISTEN_CHANNEL, itoa(channel, &buf[0], 10));
		DPRINT("Setting listening channel to %d\n", channel);
	} else if (channel == 0) {
		//ignore
	} else {
		DPRINT("P2P channel set to invalid value (%d) in menu config\n", channel);
	}
}
#endif

static void slsi_set_updateconfig(void)
{
	slsi_send_command_str_digit(NULL, WPA_COMMAND_UPDATE_CONFIG, SLSI_SAVE_CONFIG, NULL);
}

static int8_t slsi_disable_all_networks(void)
{
	int8_t result = SLSI_STATUS_SUCCESS;
	slsi_send_command_str_upto_4(NULL, WPA_COMMAND_DISABLE_NETWORK, NULL, NULL, NULL, NULL);
	return result;
}

static int8_t slsi_stop_ap(void)
{
	int8_t result = SLSI_STATUS_ERROR;
	VPRINT("requesting to stop ap mode\n");
	slsi_send_command_str_upto_4(NULL, WPA_COMMAND_STOP_AP, NULL, NULL, NULL, &result);
	if (result == SLSI_STATUS_SUCCESS) {
		VPRINT("stop ap mode awaiting AP_DISABLED event\n");
		int res = sem_wait(&g_sem_ap_mode);
		if (res) {
			EPRINT("SLSI_API sem_wait error %d\n", errno);
		}
		if (g_state != SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING) {
			EPRINT("stop ap mode - awaiting AP_DISABLED event - failed\n");
			result = SLSI_STATUS_ERROR;
		}
	}
	return result;
}

static void slsi_remove_network(char *network_id)
{
	// remove network
	slsi_send_command_str_upto_4(NULL, WPA_COMMAND_REMOVE_NETWORK, network_id, NULL, NULL, NULL);
}

// create a new network block for AP mode (mode=2)
static int8_t slsi_set_ap_network(slsi_ap_config_t *ap_config)
{
	int8_t result = SLSI_STATUS_ERROR;
	char *pbuf = NULL;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };
	char *network_id = NULL;
	char ssid_formated[WPA_MAX_SSID_LEN];
	printf_encode(ssid_formated, WPA_MAX_SSID_LEN - 1, ap_config->ssid, ap_config->ssid_len);
	g_state = SLSI_WIFIAPI_STATE_AP_ENABLING;

	VPRINT("SLSI_API setup network with ssid %s\n", ssid_formated);

	if ((slsi_get_network(ap_config->ssid, ap_config->ssid_len, &network_id) != SLSI_STATUS_SUCCESS)
		&& network_id == NULL) {
		//no network block exists with that SSID - create one!
		memset(command, 0, WPA_COMMAND_MAX_SIZE);
		snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", WPA_COMMAND_ADD_NETWORK);
		pbuf = slsi_send_request(NULL, command, NULL);
		if (pbuf) {
			pbuf[strcspn(pbuf, "\r\n")] = '\0';
			network_id = strdup(pbuf);
			if (pbuf) {
				free(pbuf);
				//removed to prevent lint warning pbuf = NULL;
			}
		}
	}

	if (network_id) {
		// Set mode (needs to be mode=2 for AP mode)
		slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_MODE_AP, NULL, &result);
		if (result != SLSI_STATUS_SUCCESS) {
			goto errout;
		}
		// Set network ssid
		if (ap_config->ssid_len != 0) {
			memset(command, 0, WPA_COMMAND_MAX_SIZE);
			snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %sP\"%s\"", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_SSID_, ssid_formated);
			pbuf = slsi_send_request(NULL, command, &result);
			if (pbuf) {
				free(pbuf);
				//removed to prevent lint warning pbuf = NULL;
				if (result != SLSI_STATUS_SUCCESS) {
					goto errout;
				}
			}
		}
		// Set network disabled=0
		slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_NETWORK_DISABLED, NULL, &result);
		if (result != SLSI_STATUS_SUCCESS) {
			goto errout;
		}
		// Set security
		result = slsi_set_security(ap_config->security, network_id);

		if (result == SLSI_STATUS_SUCCESS) {
			// Set freqency/channel
			if (ap_config->channel != 0) {
				int localFreq = 0;
				char ccode[3];
				memset(ccode, 0, 3);
				if (slsi_get_country_code(ccode) == SLSI_STATUS_SUCCESS) {
					int endchannel = 13;
					if (strncmp(ccode, "US", 2) == 0 || strncmp(ccode, "CA", 2) == 0) {
						endchannel = 11;
					}
					if (ap_config->channel >= 1 && ap_config->channel <= endchannel) {
						localFreq = 2407 + 5 * ap_config->channel;
					}
					if ((strncmp(ccode, "JP", 2) == 0) && ap_config->channel == 14) {
						localFreq = 2484;
					}
				}

				memset(command, 0, WPA_COMMAND_MAX_SIZE);
				if (localFreq) {
					// Frequency found the specify otherwise failed
					snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %s%d", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_FREQUENCY_, localFreq);
					pbuf = slsi_send_request(NULL, command, &result);
					if (pbuf) {
						free(pbuf);
						// removed to prevent lint error pbuf = NULL;
						if (result != SLSI_STATUS_SUCCESS) {
							goto errout;
						}
					}
				} else {
					EPRINT("SLSI_API ERROR: Failed to set channel - wrong channel \n");
					result = SLSI_STATUS_PARAM_FAILED;
					goto errout;
				}
			}
			// Set ap config beacon interval different from default 100
			if (ap_config->beacon_period != 100) {
				memset(command, 0, WPA_COMMAND_MAX_SIZE);
				snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %s %d", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_BEACON_INT_, ap_config->beacon_period);
				pbuf = slsi_send_request(NULL, command, &result);
				if (pbuf) {
					free(pbuf);
					// removed to prevent lint warning pbuf = NULL;
					if (result != SLSI_STATUS_SUCCESS) {
						goto errout;
					}
				}
			}
			// Set ap config DTIM interval different from default 2
			if (ap_config->DTIM != 2) {
				memset(command, 0, WPA_COMMAND_MAX_SIZE);
				snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %s %d", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_DTIM_PERIOD_, ap_config->DTIM);
				pbuf = slsi_send_request(NULL, command, &result);
				if (pbuf) {
					free(pbuf);
					// removed to prevent lint warning pbuf = NULL;
					if (result != SLSI_STATUS_SUCCESS) {
						goto errout;
					}
				}
			}
			// Set ap config Vendor specific IE
			if (ap_config->vsie != NULL) {
				DPRINT("OUI: %02X%02X%02X\n", ap_config->vsie->oui[0], ap_config->vsie->oui[1], ap_config->vsie->oui[2]);
				uint32_t cmdsize = ap_config->vsie->content_length * 3 + 30;
				char *tcommand = zalloc(cmdsize);
				size_t slen = 0;

				if (tcommand == NULL) {
					goto errout;
				}
				if (ap_config->vsie->content_length > 253) {
					free(tcommand);
					result = SLSI_STATUS_PARAM_FAILED;
					goto errout;
				}
				uint8_t *iehex = slsi_bytearray_2_hexstr(ap_config->vsie->content,
								 ap_config->vsie->content_length,
								 &slen);
				VPRINT("Content length: %d\nContent: %s\n", ap_config->vsie->content_length, iehex);
				snprintf(tcommand, cmdsize, "%s%s%02X%02X%02X %s", WPA_COMMAND_SET, WPA_PARAM_VSIE, ap_config->vsie->oui[0], ap_config->vsie->oui[1], ap_config->vsie->oui[2], iehex);

				pbuf = slsi_send_request(NULL, tcommand, &result);
				if (iehex != NULL) {
					free(iehex);
					iehex = NULL;
				}
				if (tcommand) {
					free(tcommand);
				}
				if (pbuf) {
					free(pbuf);
					// removed to prevent lint warning pbuf = NULL;
					if (result != SLSI_STATUS_SUCCESS) {
						goto errout;
					}
				}
			}
			// Set phy_mode if disabled, default is enabled
			if (ap_config->phy_mode == 0) {
#ifdef CONFIG_HT_OVERRIDES
				slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_DISABLE_HT, NULL, &result);
				if (result != SLSI_STATUS_SUCCESS) {
					goto errout;
				}
#else
				result = SLSI_STATUS_COMMAND_UNKNOWN;
				goto errout;
#endif
			} else {
				if ((ap_config->ht_mode.ht_capab_info & HT_CAP_INFO_GREEN_FIELD)
					&& (ap_config->ht_mode.ht_capab_info & HT_CAP_INFO_SHORT_GI20MHZ)) {
					// Cannot both be set
					DPRINT("Cannot set both Green field and Short Guard Interval \n");
					result = SLSI_STATUS_PARAM_FAILED;
					goto errout;
				}
				// Set green field
				// green_field and guard_interval are mutually exclusive
				if (ap_config->ht_mode.ht_capab_info & HT_CAP_INFO_GREEN_FIELD) {

#ifdef CONFIG_HT_OVERRIDES
					slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_HT_CAPAB, HT_CAP_INFO_GREEN_FIELD, &result);
					if (result != SLSI_STATUS_SUCCESS) {
						goto errout;
					}
#else
					result = SLSI_STATUS_COMMAND_UNKNOWN;
					goto errout;
#endif
				}
				// Set guard interval off
				// green_field and guard_interval are mutually exclusive
#ifdef CONFIG_HT_OVERRIDES
				if ((ap_config->ht_mode.ht_capab_info & HT_CAP_INFO_SHORT_GI20MHZ) == 0) {
					slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_DISABLE_SHORT_GI, NULL, &result);
					if (result != SLSI_STATUS_SUCCESS) {
						goto errout;
					}
				}
#endif
				// Set MCS index
				if (ap_config->ht_mode.mcs_index[0] != 0 || ap_config->ht_mode.mcs_index[1] != 0) {
#ifdef CONFIG_HT_OVERRIDES
					memset(command, 0, WPA_COMMAND_MAX_SIZE);
					snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %s%x", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_HT_MCS, ap_config->ht_mode.mcs_index);
					pbuf = slsi_send_request(NULL, command, &result);
					if (pbuf) {
						free(pbuf);
						pbuf = NULL;
						if (result != SLSI_STATUS_SUCCESS) {
							goto errout;
						}
					}
#else
					result = SLSI_STATUS_COMMAND_UNKNOWN;
					goto errout;
#endif
				}

			}

			// Select network
			slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SELECT_NETWORK, network_id, NULL, NULL, &result);
			if (result != SLSI_STATUS_SUCCESS) {
				goto errout;
			} else {
				VPRINT("setup ap mode awaiting AP_ENABLED event\n");
				int res = sem_wait(&g_sem_ap_mode);
				if (res) {
					EPRINT("SLSI_API sem_wait error %d\n", errno);
				}
				if (g_state != SLSI_WIFIAPI_STATE_AP_ENABLED) {
					EPRINT("stop ap mode - awaiting AP_DISABLED event - failed\n");
					result = SLSI_STATUS_ERROR;
					goto errout;
				}
				if (g_network_id) {
					free(g_network_id);
					g_network_id = NULL;
				}
				g_network_id = strdup(network_id);
			}
		}

errout:
		if (result != SLSI_STATUS_SUCCESS) {
			// remove network
			slsi_remove_network(network_id);
			g_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
		}
		if (network_id) {
			free(network_id);
			network_id = NULL;
		}
	}

	return result;
}

static int8_t slsi_start_scan(void)
{
	int8_t result = SLSI_STATUS_ERROR;
	slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SCAN, NULL, NULL, NULL, &result);
	if (result == SLSI_STATUS_SUCCESS) {
		g_scanning = 1;
	}
	return result;
}

static int8_t slsi_start_specific_scan(uint8_t *ssid, uint8_t ssid_len, const slsi_security_config_t *sec_config)
{
	int8_t result = SLSI_STATUS_ERROR;
	char *pbuf = NULL;
	char *network_id = NULL;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };

	VPRINT("SLSI_API slsi_start_specific_scan with ssid %s\n", ssid);
	if (sec_config) {
		VPRINT("SLSI_API slsi_start_specific_scan with security settings: " "security mode %d\n", sec_config->secmode);
	}
	// Find network or add new network
	result = slsi_get_network(ssid, ssid_len, &network_id);
	if ((result != SLSI_STATUS_SUCCESS) && (network_id == NULL) /* Attempt to make SVACE happy */) {
		// Add new network
		char ssid_formatted[WPA_MAX_SSID_LEN];
		snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", WPA_COMMAND_ADD_NETWORK);
		pbuf = slsi_send_request(NULL, command, NULL);
		if (pbuf) {
			pbuf[strcspn(pbuf, "\r\n")] = '\0';
			network_id = strdup(pbuf);
			free(pbuf);
		} else {
			goto errout;
		}
		if (g_scan_network_id) {
			free(g_scan_network_id);
			g_scan_network_id = NULL;
		}
		g_scan_network_id = strdup(network_id);
		// Set network ssid
		printf_encode(ssid_formatted, WPA_MAX_SSID_LEN - 1, ssid, ssid_len);
		memset(command, 0, WPA_COMMAND_MAX_SIZE);
		snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %sP\"%s\"", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_SSID_, ssid_formatted);
		pbuf = slsi_send_request(NULL, command, &result);
		if (pbuf) {
			free(pbuf);
			// removed to prevent lint warning pbuf = NULL;
			if (result != SLSI_STATUS_SUCCESS) {
				goto errout;
			}
		}
		// Set scan_ssid which scans for APs using hidden SSIDs.
		memset(command, 0, WPA_COMMAND_MAX_SIZE);
		snprintf(command, WPA_COMMAND_MAX_SIZE, "%s%s %s%d", WPA_COMMAND_SET_NETWORK, network_id, WPA_PARAM_SCAN_SSID, 1);
		pbuf = slsi_send_request(NULL, command, &result);
		if (pbuf) {
			free(pbuf);
			// removed to prevent lint warning pbuf = NULL;
			if (result != SLSI_STATUS_SUCCESS) {
				goto errout;
			}
		}
	}
	// Set security
	result = slsi_set_security(sec_config, network_id);
	if (result != SLSI_STATUS_SUCCESS) {
		// remove network
		slsi_send_command_str_upto_4(NULL, WPA_COMMAND_REMOVE_NETWORK, network_id, NULL, NULL, NULL);
	} else {
		memset(command, 0, WPA_COMMAND_MAX_SIZE);
		snprintf(command, WPA_COMMAND_MAX_SIZE, " %s%s", WPA_PARAM_SCAN_ID, network_id);
		slsi_send_command_str_upto_4(NULL, WPA_COMMAND_SCAN, command, NULL, NULL, &result);
		if (result == SLSI_STATUS_SUCCESS) {
			g_scanning = 1;
		}
	}

errout:
	if (network_id) {
		free(network_id);
		network_id = NULL;
	}

	return result;
}

slsi_scan_info_t *slsi_parse_scan_results(char *sr)
{
	slsi_scan_info_t *results = NULL;
	char *pos, *end;
	pos = strchr(sr, '\n');

	if (pos == NULL) {
		return results;
	}

	slsi_scan_info_t *local_scan_result = (slsi_scan_info_t *)zalloc((size_t) sizeof(slsi_scan_info_t));
	if (local_scan_result == NULL) {
		return results;
	}

	results = local_scan_result;
	/*
	 * Example of result to parse
	 *  bssid
	 *  c4:04:15:0d:5d:04
	 *  20:4e:7f:5a:d4:a0
	 *  00:11:21:76:d4:00
	 */
	*pos = '\0';
	pos = pos + 1;
	bool discarted = FALSE;
	slsi_scan_info_t *local_prev = NULL;

	while (*pos) {
		if (local_scan_result == NULL) {
			break;
		}
		memset(local_scan_result, 0, sizeof(slsi_scan_info_t));
		end = strchr(pos, '\n');
		if (end == NULL) {
			break;
		}

		*end = '\0';			// Replace '\n' with '\0' to make it a valid string
		memcpy(&(local_scan_result->bssid), pos, 18);
		/* The BSSID have been extracted, fill in the remaining information */

		if (slsi_get_bss_info(local_scan_result->bssid, local_scan_result)) {
			discarted = FALSE;
			pos = end + 1;
			if (*pos != '\0') {
				local_prev = local_scan_result;
				local_scan_result->next = zalloc(sizeof(slsi_scan_info_t));
				local_scan_result = local_scan_result->next;
			} else {
				local_scan_result->next = NULL;
			}
		} else {
			pos = end + 1;
			discarted = TRUE;
		}
	}

	if (discarted) {
		if (local_scan_result) {
			VPRINT("Allocated one to many - set previous next to NULL\n");
			free(local_scan_result);
			local_scan_result = NULL;
			if (local_prev) {
				local_prev->next = NULL;
			}
		}
	}

	return results;
}

static int8_t slsi_get_api_scan_results(slsi_scan_info_t **result_handler)
{
	int8_t result = SLSI_STATUS_ERROR;
	char *pbuf = NULL;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };
	snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", WPA_COMMAND_SCAN_RESULTS);
	pbuf = slsi_send_request(NULL, command, NULL);
	if (pbuf) {
		*result_handler = slsi_parse_scan_results(pbuf);
		free(pbuf);
		pbuf = NULL;
		result = SLSI_STATUS_SUCCESS;
	}
	return result;
}

static int8_t slsi_get_bssid(char **bssid)
{
	int8_t result = SLSI_STATUS_ERROR;
	char *pbuf = NULL;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };
	char *pos, *end;

	snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", WPA_COMMAND_STATUS);
	pbuf = slsi_send_request(NULL, command, NULL);
	if (pbuf) {
		pos = strstr(pbuf, WPA_PARAM_BSSID);
		if (pos == NULL) {
			VPRINT("SLSI_API get bssid FAILED");
			free(pbuf);
			pbuf = NULL;
			goto errout;
		}
		pos += 6;
		end = strchr(pos, '\n');
		if (end == NULL) {
			VPRINT("SLSI_API get bssid FAILED");
			free(pbuf);
			pbuf = NULL;
			goto errout;
		}
		*end = '\0';
		VPRINT("SLSI_API get_bssid bssid %s\n", pos);
		memcpy(*bssid, pos, 18);
		free(pbuf);
		pbuf = NULL;
		result = SLSI_STATUS_SUCCESS;
	}
errout:
	return result;
}

int8_t slsi_get_country_code(char *country_code)
{
	int8_t result = SLSI_STATUS_ERROR;
	slsi_init_nvram();
	if (up_wlan_read_config((void *)g_nvram, SLSI_WIFI_NV_DATA_SIZE)) {
		g_slsi_wifi_nv_data = (slsi_wifi_nv_data_t *)g_nvram;
		memcpy(country_code, g_slsi_wifi_nv_data->country_code, 3);
		country_code[2] = '\0';
		DPRINT("Country code found: %s\n", country_code);
		result = SLSI_STATUS_SUCCESS;
	}
	if (g_nvram) {
		free(g_nvram);
		g_nvram = NULL;
	}

	return result;
}

int8_t slsi_sent_force_panic(void)
{
	int8_t result = SLSI_STATUS_ERROR;
	slsi_send_command_str_upto_4(NULL, WPA_COMMAND_DRIVER, WPA_PARAM_FORCE_PANIC, NULL, NULL, &result);
	return result;
}

int8_t slsi_get_mac(uint8_t *mac)
{
	int8_t result = SLSI_STATUS_ERROR;
	char *pbuf = NULL;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };
	char *pos, *end;

	// Check status to get own address
	snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", WPA_COMMAND_STATUS);
	pbuf = slsi_send_request(NULL, command, NULL);
	if (pbuf) {
		uint8_t i;
		pos = strstr(pbuf, WPA_PARAM_IP_ADDRESS);
		if (pos != NULL) {
			pos += 11;
			pos = strstr(pos, WPA_PARAM_ADDRESS);
		} else {
			pos = strstr(pbuf, WPA_PARAM_ADDRESS);
		}
		if (pos == NULL) {
			VPRINT("SLSI_API get_mac parameter missing WPA_VALUE_ADDRESS\n");
			free(pbuf);
			pbuf = NULL;
			goto errout;
		}
		pos += 8;
		end = strchr(pos, '\n');
		if (end == NULL) {
			VPRINT("SLSI_API get_mac command 2 FAILED\n");
			free(pbuf);
			pbuf = NULL;
			goto errout;
		}
		*end = '\0';
		for (i = 0; i < 6; i++) {
			mac[i] = strtol(pos, &end, 16);
			VPRINT("SLSI_API get_mac select: %02x\n", mac[i]);
			pos += 3;
		}
		free(pbuf);
		pbuf = NULL;
		result = SLSI_STATUS_SUCCESS;
	}
errout:
	return result;
}

static int8_t slsi_set_country_code(const char *country_code, bool write_to_nvram, bool write_to_driver)
{
	int8_t result = SLSI_STATUS_ERROR;
	slsi_init_nvram();
	if (write_to_driver) {
		slsi_send_command_str_upto_4(NULL, WPA_COMMAND_DRIVER, WPA_PARAM_COUNTRY, (char *)country_code, NULL, &result);
		VPRINT("SLSI_API country code: %s\n", country_code);
	}
	if (write_to_nvram) {
		if (g_nvram == NULL) {
			g_nvram = malloc(SLSI_WIFI_NV_DATA_SIZE);	// flash block size of 4k
		}
		// set the country code in NV ram for next time
		//start by reading the existing values form NVRAM and Erase to be ready for writing
		if (up_wlan_read_config((void *)g_nvram, SLSI_WIFI_NV_DATA_SIZE)) {
			g_slsi_wifi_nv_data = (slsi_wifi_nv_data_t *)g_nvram;
			VPRINT("Read from NVRAM: %s\n", g_slsi_wifi_nv_data->country_code);
			up_wlan_erase_config();
			memcpy(g_slsi_wifi_nv_data->country_code, country_code, 3);
			g_slsi_wifi_nv_data->country_code[2] = '\0';
			//store the country code for later
			g_country_code[0] = country_code[0];
			g_country_code[1] = country_code[1];
			// write it back to nvram
			DPRINT("Writing countrycode %s to NVRAM\n", g_slsi_wifi_nv_data->country_code);
			if (!up_wlan_write_config((void *)g_nvram, SLSI_WIFI_NV_DATA_SIZE)) {
				EPRINT("NVRAM write failed\n");
			} else {
				result = SLSI_STATUS_SUCCESS;
			}
		} else {
			EPRINT("NVRAM read failed\n");
		}
	}
	if (g_state == SLSI_WIFIAPI_STATE_NOT_STARTED) {
		// we need to cleanup nvram as it will only get cleaned on wifistop
		if (g_nvram != NULL) {
			free(g_nvram);
			g_nvram = NULL;
		}
	}
	return result;
}

static int8_t slsi_get_tx_power(uint8_t *dbm)
{
	int8_t result = SLSI_STATUS_ERROR;
	char *pbuf = NULL;
	char *pos, *end;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };

	slsi_init_nvram();
	snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", WPA_COMMAND_GET_TX_POWER);
	pbuf = slsi_send_request(NULL, command, NULL);

	if (pbuf) {
		*dbm = strtol(pbuf, &end, 10);
		VPRINT("dbm = %d\n", *dbm);
		pos = end + 1;
		free(pbuf);
		pbuf = NULL;
		result = SLSI_STATUS_SUCCESS;
	} else {
		DPRINT("Could not get tx_power from supplicant, trying from NVRAM\n");
		if (g_nvram == NULL) {
			g_nvram = malloc(SLSI_WIFI_NV_DATA_SIZE);	// flash block size of 4k
		}
		if (up_wlan_read_config((void *)g_nvram, SLSI_WIFI_NV_DATA_SIZE)) {
			g_slsi_wifi_nv_data = (slsi_wifi_nv_data_t *)g_nvram;
			*dbm = g_slsi_wifi_nv_data->tx_power;
			DPRINT("Tx Power found in NVRAM: %d dbm\n", dbm);
			result = SLSI_STATUS_SUCCESS;
		}
	}
	if (g_nvram) {
		free(g_nvram);
		g_nvram = NULL;
	}

	return result;
}

static int8_t slsi_set_tx_power(uint8_t *dbm, bool write_to_nvram, bool write_to_sup)
{
	int8_t result = SLSI_STATUS_ERROR;

	if (*dbm > 30 || *dbm < 12) {
		return SLSI_STATUS_PARAM_FAILED;
	}
	if (write_to_sup) {
		slsi_send_command_str_digit(NULL, WPA_COMMAND_SET_TX_POWER, *dbm, NULL);
	}
	if (write_to_nvram) {
		slsi_init_nvram();
		// set the tx_power in NV ram for next time
		//start by reading the existing values form NVRAM and Erase to be ready for writing
		if (up_wlan_read_config((void *)g_nvram, SLSI_WIFI_NV_DATA_SIZE)) {
			g_slsi_wifi_nv_data = (slsi_wifi_nv_data_t *)g_nvram;
			VPRINT("Tx Power read from NVRAM: %d\n", g_slsi_wifi_nv_data->tx_power);
			up_wlan_erase_config();
			g_slsi_wifi_nv_data->tx_power = *dbm;
			// write it back to nvram
			DPRINT("Writing tx_power (dbm) %d to NVRAM\n", g_slsi_wifi_nv_data->tx_power);
			if (!up_wlan_write_config((void *)g_nvram, SLSI_WIFI_NV_DATA_SIZE)) {
				EPRINT("NVRAM write failed\n");
			} else {
				result = SLSI_STATUS_SUCCESS;
			}
		} else {
			EPRINT("NVRAM read failed\n");
		}
	}
	if (g_state == SLSI_WIFIAPI_STATE_NOT_STARTED) {
		// we need to cleanup nvram as it will only get cleaned on wifistop
		if (g_nvram != NULL) {
			free(g_nvram);
			g_nvram = NULL;
		}
	}
	return result;
}

static int8_t slsi_get_rssi(int8_t *rssi_value)
{
	int8_t result = SLSI_STATUS_ERROR;

	char *pbuf = NULL;
	char *pos, *end;

	char command[WPA_COMMAND_MAX_SIZE] = { 0 };

	snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", WPA_COMMAND_SIGNAL_POLL);

	pbuf = slsi_send_request(NULL, command, NULL);
	if (pbuf) {
		VPRINT("SLSI_API slsi_get_rssi buffer: %s\n", pbuf);
		// Extract rssi
		pos = strstr(pbuf, WPA_PARAM_RSSI);
		if (pos == NULL) {
			VPRINT("SLSI_API get_rssi cannot find: %s\n", WPA_PARAM_RSSI);
			free(pbuf);
			pbuf = NULL;
			goto errout;
		}
		pos += 5;
		end = strchr(pos, '\n');
		if (end == NULL) {
			VPRINT("SLSI_API get_rssi failed \n");
			free(pbuf);
			pbuf = NULL;
			goto errout;
		}
		*end = '\0';
		*rssi_value = strtol(pos, &end, 10);
		VPRINT("SLSI_API rssi: %d\n", *rssi_value);
		result = SLSI_STATUS_SUCCESS;
	}
errout:
	if (pbuf) {
		free(pbuf);
	}
	return result;
}

static int8_t slsi_get_channel(int8_t *channel)
{
	int8_t result = SLSI_STATUS_ERROR;
	char *pbuf = NULL;
	char command[WPA_COMMAND_MAX_SIZE] = { 0 };
	char *pos, *end;

	snprintf(command, WPA_COMMAND_MAX_SIZE, "%s", WPA_COMMAND_STATUS);
	pbuf = slsi_send_request(NULL, command, NULL);
	if (pbuf) {
		// Extract frequency
		pos = strstr(pbuf, WPA_PARAM_FREQ);
		if (pos == NULL) {
			VPRINT("SLSI_API get: %s FAILED \n", WPA_PARAM_FREQ);
			free(pbuf);
			pbuf = NULL;
			goto errout;
		}
		pos += 5;
		end = strchr(pos, '\n');
		if (end == NULL) {
			free(pbuf);
			pbuf = NULL;
			goto errout;
		}
		*end = '\0';
		ieee80211_freq_to_chan(strtol(pos, &end, 10), (uint8_t *)channel);
		VPRINT("SLSI_API channel: %d\n", *channel);
		result = SLSI_STATUS_SUCCESS;
		pos = end + 1;
		free(pbuf);
		pbuf = NULL;
	}
errout:
	return result;
}

static int8_t slsi_terminate_supplicant(void)
{
	int8_t result = SLSI_STATUS_ERROR;
	VPRINT("slsi_terminate_supplicant \n");
	slsi_send_command_str_upto_4(NULL, WPA_COMMAND_TERMINATE, NULL, NULL, NULL, &result);
	return result;
}

static uint8_t slsi_stop_supplicant(void)
{
	uint8_t result;
	int status = -1;
	UNUSED(status);
	pid_t r_task_id = -1;
	/* Send message to end callback thread */
	if (g_callback_running) {
		slsi_send_mqueue(SLSI_CALLBACK_SHUTDOWN, NULL);
	}
	/* Send TERMINATE to supplicant */
	g_state = SLSI_WIFIAPI_STATE_TERMINATING;
	VPRINT("SLSI_API terminate requested\n");
	result = slsi_terminate_supplicant();
	if (result == SLSI_STATUS_SUCCESS) {
		int res = sem_wait(&g_sem_terminate);
		if (res) {
			EPRINT("SLSI_API sem_wait error %d\n", errno);
		}
	} else {
		EPRINT("Termination of supplicant failed - but will continue disconnecting wpa_ctrl \n");
	}
	DPRINT("Wait for supplicant task to terminate task id=%d \n", g_task);
	r_task_id = waitpid(g_task, &status, NULL);
	if (r_task_id != g_task) {
		DPRINT("    result: %d, status: %d\n", r_task_id, status);
		EPRINT("Error waiting for task termination but nothing to do about it\n");
	}
	g_active_ifname = NULL;
	g_task = 0;
	/* If monitor still running - stop it now */
	if (g_running) {
		VPRINT("SLSI_API stop running\n");
		g_running = 0;
	}
	/* Ensure callback queues are closed and thread have terminated */
	if (g_callback_running) {
		VPRINT("Should already be closed\n");
		g_callback_running = 0;
	}
	if (g_send_cbmqfd) {
		if (mq_close(g_send_cbmqfd) < 0) {
			EPRINT("sender mq_close failed\n");
		}
	}
	if (g_recv_cbmqfd) {
		if (mq_close(g_recv_cbmqfd) < 0) {
			EPRINT("receiver mq_close failed\n");
		}
	}
	if (mq_unlink(SLSI_CALLBACK_MQUEUE) < 0) {
		EPRINT("mq_unlink failed \n");
	}

	/* Disconnect wpa_ctrl connection */
	result = slsi_wpa_close(TRUE);
	if (result != SLSI_STATUS_SUCCESS) {
		EPRINT("Error: still running after disconnect. \n");
	}

	return result;
}

static uint8_t slsi_create_callback_thread(void)
{
	uint8_t result = SLSI_STATUS_ERROR;
	if (pthread_create(&g_callback_thread, NULL, (void *)slsi_callback_thread_handler, NULL)) {
		EPRINT("SLSI_API Failed to create thread\n");
	} else {
		result = SLSI_STATUS_SUCCESS;
		pthread_setname_np(g_callback_thread, "Wi-Fi API callback");
		VPRINT("SLSI_API Callback thread created successfully\n");
	}
	return result;
}

static uint8_t slsi_start_supplicant(WiFi_InterFace_ID_t interface)
{
	uint8_t result;
#ifndef CONFIG_DEBUG_FEATURES
	char *logarg = NULL;
#endif
	char *confarg = NULL;
#ifdef CONFIG_SLSI_WIFI_P2P_API
	char *confarg2 = NULL;
#endif
	g_active_ifname = (interface == SLSI_WIFI_SOFT_AP_IF || interface == SLSI_WIFI_STATION_IF)
					  ? CTRL_IFNAME : CTRL_P2P_IFNAME;

#ifdef CONFIG_SLSI_WIFI_FILESYSTEM_SUPPORT
	if (slsi_init_filesystem(interface) == SLSI_STATUS_SUCCESS) {
		// TODO This is a hack!!!
		confarg = "-c" CONFIG_SLSI_WIFI_DIR "/" CONFIG_SLSI_WIFI_CONFIG_FILE_NAME;
#ifdef CONFIG_SLSI_WIFI_P2P_API
		confarg2 = "-c" CONFIG_SLSI_WIFI_DIR "/" CONFIG_SLSI_WIFI_P2P_CONFIG_FILE_NAME;
#endif

#ifndef CONFIG_DEBUG_FEATURES
		logarg = "-f" CONFIG_SLSI_WIFI_DIR "/" CONFIG_SLSI_WIFI_LOG_FILE_NAME;
#endif							// CONFIG_DEBUG
	}
#endif							// CONFIG_SLSI_WIFI_FILESYSTEM_SUPPORT

	// Start by creating task for wpa_supplicant
	int arg_count = 0;

#ifndef CONFIG_DEBUG_FEATURES
	sup_argv[arg_count++] = "-B";
#endif
	sup_argv[arg_count++] = "-t";
	sup_argv[arg_count++] = "-i" CTRL_IFNAME;
	if (confarg != NULL) {
		sup_argv[arg_count++] = confarg;
	} else {
		sup_argv[arg_count++] = "-Cudp";
	}
#ifdef CONFIG_SLSI_WIFI_P2P_API
	sup_argv[arg_count++] = "-N";
	sup_argv[arg_count++] = "-i" CTRL_P2P_IFNAME;
	if (confarg2 != NULL) {
		sup_argv[arg_count++] = confarg2;
	} else {
		sup_argv[arg_count++] = "-Cudp";
	}
#endif
#ifndef CONFIG_DEBUG_FEATURES
	if (logarg != NULL) {
		sup_argv[arg_count++] = logarg;
	}
#endif
	sup_argv[arg_count++] = NULL;

	DPRINT("SLSI_API call task_create, arg count = %d\n", arg_count);
	g_task = task_create("WPA Supplicant", CONFIG_WPA_SUPPLICANT_PRIORITY, CONFIG_WPA_SUPPLICANT_STACKSIZE, (main_t)CONFIG_WPA_SUPPLICANT_ENTRYPOINT, sup_argv);
	sleep(1);

	/* Start call back thread */
	slsi_create_callback_thread();

	if (g_task < 0) {
		EPRINT("SLSI_API ERROR: Failed to start supplicant \n");
		result = SLSI_STATUS_SUPPLICANT_START_FAILED;
	} else {
		DPRINT("SLSI_API task_create supplicant task pid: %d\n", g_task);
		/* initialize globals */
		g_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
		g_running = 0;
		g_wpa_attached = 0;
		VPRINT("register ifname %s\n", g_active_ifname);
		/* open channel to supplicant and create monitor thread */
		result = slsi_wpa_open(g_active_ifname);
	}
	return result;
}

static bool slsi_is_connected(void)
{
	bool ret = FALSE;
	if ((g_state == SLSI_WIFIAPI_STATE_STA_CONNECTED) || (g_state == SLSI_WIFIAPI_STATE_AP_CONNECTED)) {
		ret = TRUE;
	}
	return ret;
}

static void slsi_init_nvram(void)
{
	if (g_nvram == NULL) {
		g_nvram = zalloc(SLSI_WIFI_NV_DATA_SIZE);	// flash block size of 4k
		if (!g_nvram) {
			return;
		}
	}
	if (up_wlan_read_config((void *)g_nvram, SLSI_WIFI_NV_DATA_SIZE)) {
		g_slsi_wifi_nv_data = (slsi_wifi_nv_data_t *) g_nvram;
		//check if it this is not the first boot - it is already initialized
		if (memcmp("SLSI", g_slsi_wifi_nv_data->initialized, 4) != 0) {
			char *default_cc = SLSI_WIFI_NV_DEFAULT_COUNTRY_CODE;
			uint8_t tx_power = SLSI_WIFI_NV_DEFAULT_TX_POWER;

			//erase the NVRAM area so we are ready to write to it
			up_wlan_erase_config();
			// write it back to nvram
			strncpy(&g_slsi_wifi_nv_data->initialized[0], "SLSI", 4);
			g_slsi_wifi_nv_data->initialized[4] = '\0';	//needed for svace
			/* the read value is not one we can understand - lets store a sane one */
			g_slsi_wifi_nv_data->country_code[0] = default_cc[0];
			g_slsi_wifi_nv_data->country_code[1] = default_cc[1];
			g_slsi_wifi_nv_data->country_code[2] = '\0';
			g_slsi_wifi_nv_data->tx_power = tx_power;
			if (!up_wlan_write_config((void *)g_nvram, SLSI_WIFI_NV_DATA_SIZE)) {
				EPRINT("NVRAM write failed\n");
			} else {
				VPRINT("NVRAM now contains:\n\tcountry-code: %s\n\ttx_power: %d\n", g_slsi_wifi_nv_data->country_code, g_slsi_wifi_nv_data->tx_power);
			}
		} else {
			VPRINT("NVRAM already initialized\n");
			VPRINT("NVRAM contains:\n\tcountry-code: %s\n\ttx_power: %d\n", g_slsi_wifi_nv_data->country_code, g_slsi_wifi_nv_data->tx_power);
		}

	} else {
		EPRINT("Failed to read NVRAM\n");
	}
}

static int8_t slsi_init(WiFi_InterFace_ID_t interface_id, const slsi_ap_config_t *ap_config)
{
	int8_t result = SLSI_STATUS_ERROR;
	g_scan_result_handler = NULL;
	g_scanning = 0;
	sem_init(&g_sem_disconnect, 0, 0);
	sem_init(&g_sem_ap_mode, 0, 0);
	sem_init(&g_sem_api_block, 0, 0);
	sem_init(&g_sem_terminate, 0, 0);

	if (ap_config) {
		VPRINT("start ap_config available\n");
		if (g_ap_config != NULL) {
			if (g_ap_config->security) {
				free(g_ap_config->security);	// We might have a sec. config to free
				g_ap_config->security = NULL;
			}
			if (g_ap_config->vsie) {
				if (g_ap_config->vsie->content) {
					free(g_ap_config->vsie->content);
					g_ap_config->vsie->content = NULL;
				}
				free(g_ap_config->vsie);	// We might have a vsie config to free
				g_ap_config->vsie = NULL;
			}
			free(g_ap_config);	// Free app_config of needed
			g_ap_config = NULL;
		}
		g_ap_config = zalloc(sizeof(slsi_ap_config_t));
		if (g_ap_config == NULL) {
			goto errout;
		}

		*g_ap_config = *ap_config;
		if (ap_config->security) {
			g_ap_config->security = zalloc(sizeof(slsi_security_config_t));
			if (g_ap_config->security == NULL) {
				goto errout;
			}
			*(g_ap_config->security) = *(ap_config->security);
		} else {
			g_ap_config->security = NULL;
		}
		if (ap_config->vsie) {
			g_ap_config->vsie = zalloc(sizeof(slsi_vendor_ie_t));
			if (g_ap_config->vsie == NULL) {
				goto errout;
			}
			*(g_ap_config->vsie) = *(ap_config->vsie);
			g_ap_config->vsie->content = zalloc(ap_config->vsie->content_length);
			if (g_ap_config->vsie->content == NULL) {
				goto errout;
			}
			memcpy(g_ap_config->vsie->content, ap_config->vsie->content, ap_config->vsie->content_length);
		} else {
			g_ap_config->vsie = NULL;
		}
	}

	if (g_task == 0) {
		result = slsi_start_supplicant(interface_id);
		if (result == SLSI_STATUS_SUCCESS) {
#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
			// setup recovery thread
			if (!g_recovery_running) {
				if (pthread_create(&g_recovery_thread, NULL, (void *)slsi_recovery_thread_handler, NULL)) {
					EPRINT("Failed to recovery create thread\n");
				} else {
					result = SLSI_STATUS_SUCCESS;
					pthread_setname_np(g_recovery_thread, "Wi-Fi API recovery");
					VPRINT("Recovery thread created successfully\n");
				}
			}
#endif
			/* Initialize nvram value if it is the first time we run.
			 * Use default values from menuconfig defines */
			slsi_init_nvram();	//Always need to be called at startup prior to any set function
			slsi_set_country_code(g_slsi_wifi_nv_data->country_code, FALSE, TRUE);
			slsi_set_tx_power(&g_slsi_wifi_nv_data->tx_power, FALSE, TRUE);
			slsi_set_updateconfig();
			slsi_set_scan_interval(SLSI_SCAN_INTERVAL);
			slsi_set_bss_expiration();
		} else {
			goto errout;
		}
	} else {
		if (g_task && g_running && g_wpa_attached) {
			result = SLSI_STATUS_SUCCESS;
		} else {
			EPRINT("Supplicant started but socket connections need restart \n");
		}
	}

errout:
	return result;
}

static void slsi_deinit(void)
{
	g_scan_result_handler = NULL;
	g_link_up = NULL;
	g_link_down = NULL;
	g_wpa_attached = 0;
	g_callback_running = 0;
	g_running = 0;
	g_scanning = 0;
	g_state = SLSI_WIFIAPI_STATE_NOT_STARTED;
	g_task = 0;
	g_num_sta_connected = 0;
	sem_post(&g_sem_terminate);
	sem_destroy(&g_sem_terminate);
	sem_post(&g_sem_ap_mode);
	sem_destroy(&g_sem_ap_mode);
	sem_post(&g_sem_disconnect);
	sem_destroy(&g_sem_disconnect);
	sem_post(&g_sem_api_block);
	sem_destroy(&g_sem_api_block);

	if (g_nvram != NULL) {
		free(g_nvram);
		g_nvram = NULL;
	}
	if (g_ap_config != NULL) {
		if (g_ap_config->security) {
			free(g_ap_config->security);	// We might have a sec. config to free
			g_ap_config->security = NULL;
		}
		if (g_ap_config->vsie) {
			if (g_ap_config->vsie->content) {
				free(g_ap_config->vsie->content);	// We might have a vsie config to free
				g_ap_config->vsie->content = NULL;
			}
			free(g_ap_config->vsie);	// We might have a vsie config to free
			g_ap_config->vsie = NULL;
		}
		// Free app_config of needed
		free(g_ap_config);
		g_ap_config = NULL;
	}
	if (g_network_id) {
		free(g_network_id);
		g_network_id = NULL;
	}
}

static void slsi_init_sta(void)
{
#ifdef CONFIG_SLSI_WIFI_NO_AUTOCONNECT
	slsi_set_autoconnect(0);
#else
	slsi_set_autoconnect(1);
#endif
}

static void slsi_init_ap(void)
{
	slsi_set_autoconnect(0);
}

static void slsi_init_p2p(void)
{
	slsi_set_autoconnect(0);
#ifdef CONFIG_SLSI_WIFI_P2P_LISTEN_CHANNEL
	uint8_t channel = 0;
	channel = CONFIG_SLSI_WIFI_P2P_LISTEN_CHANNEL;
	slsi_set_p2p_channel(channel);
#endif
	//    slsi_send_command_str_upto_4(WPA_COMMAND_DISABLE_NETWORK, NULL, NULL, NULL, NULL);
	//    slsi_send_command_str_digit(WPA_COMMAND_UPDATE_CONFIG, SLSI_SAVE_CONFIG, NULL);
	/*
	 * TODO: add set commands for the following to get rid of config file
	 CTRL_IFACE SET 'config_methods'='push_button display keypad'
	 CTRL_IFACE SET 'device_type'='10-0050F204-5'
	 */
}

static int8_t slsi_ap_stop(void)
{
	int8_t result = SLSI_STATUS_ERROR;
	VPRINT("STA enabled - stop AP mode\n");
	g_state = SLSI_WIFIAPI_STATE_AP_DISABLING;
	result = slsi_stop_ap();
	if (result != SLSI_STATUS_SUCCESS) {
		EPRINT("stop ap mode - failed - still attempt to disable networks\n");
		uint8_t tmp_result = slsi_disable_all_networks();
		if (tmp_result != SLSI_STATUS_SUCCESS) {
			EPRINT("disable networks - failed\n");
		}
	} else {
		VPRINT("supplicant running so disabling all existing networks\n");
		result = slsi_disable_all_networks();
	}
	// remove network
	if (g_network_id) {
		slsi_remove_network(g_network_id);
		free(g_network_id);
		g_network_id = NULL;
	}
	return result;
}

static int8_t slsi_sta_stop(void)
{
	int8_t result = SLSI_STATUS_ERROR;
	/* Disconnect if in STA mode and connected */
	if (g_state == SLSI_WIFIAPI_STATE_STA_CONNECTED) {
		g_state = SLSI_WIFIAPI_STATE_STA_DISCONNECTING;
		VPRINT("supplicant already running - disconnect connected STA networks\n");
		result = slsi_leave_network(NULL);
		if (result == SLSI_STATUS_SUCCESS) {
			int res = sem_wait(&g_sem_disconnect);
			if (res) {
				EPRINT("g_sem_disconnect sem_wait error %d\n", errno);
			}
		} else {
			EPRINT("%s Error: disconnect. \n", __func__);
		}
	} else {
		/* Nothing to do */
		result = SLSI_STATUS_SUCCESS;
	}
	return result;
}

static void slsi_set_ifname(WiFi_InterFace_ID_t interface_id)
{
	VPRINT("Update ifname %s for new interface \n", g_active_ifname);
	//Update the Ifname alone as supplicant is already started to handle Mode Switch cases.
	g_active_ifname = (interface_id == SLSI_WIFI_SOFT_AP_IF || interface_id == SLSI_WIFI_STATION_IF)
					  ? CTRL_IFNAME : CTRL_P2P_IFNAME;
	wpa_update_ctrl_ifname(g_ctrl_conn, g_active_ifname);
}

static int8_t slsi_api_start(WiFi_InterFace_ID_t interface_id, const slsi_ap_config_t *ap_config)
{
	int8_t result = SLSI_STATUS_ERROR;
	DPRINT("Start %s\n", slsi_wifi_interface_id[interface_id]);
	result = slsi_init(interface_id, ap_config);
	if (result == SLSI_STATUS_SUCCESS) {
		/*Supplicant started */
		switch (interface_id) {
		case SLSI_WIFI_STATION_IF: {
			VPRINT("Start STA\n");
			/* Check old interface type - if AP then stop first */
			if (slsi_get_op_mode() == SLSI_WIFI_SOFT_AP_IF) {
				result = slsi_ap_stop();
			}
#ifdef CONFIG_SLSI_WIFI_P2P_API
			else if (slsi_get_op_mode() == SLSI_WIFI_P2P_IF) {
				/* P2P procedures would have been stopped through explicit stop procedure via
				 * artikwifi, otherwise it needs to be stopped here. */
				result = slsi_p2p_stop();
				g_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
			}
#endif
			slsi_set_ifname(interface_id);
			slsi_init_sta();
			break;
		}
		case SLSI_WIFI_SOFT_AP_IF: {
			VPRINT("Start AP\n");
			if (slsi_get_op_mode() == SLSI_WIFI_STATION_IF) {
				result = slsi_sta_stop();
			}
#ifdef CONFIG_SLSI_WIFI_P2P_API
			else if (slsi_get_op_mode() == SLSI_WIFI_P2P_IF) {
				/* P2P procedures would have been stopped through explicit stop procedure via
				 * artikwifi. otherwise it needs to be stopped here. */
				result = slsi_p2p_stop();
				g_state = SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING;
			}
#endif
			slsi_set_ifname(interface_id);
			slsi_init_ap();
			if (result == SLSI_STATUS_SUCCESS) {
				result = slsi_set_ap_network(g_ap_config);
#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
				if (!g_recovering) {
					/* Save start information needed in case of recovery handling */
					if (result == SLSI_STATUS_SUCCESS) {
						// TODO: can rely on g_ap_config as this is now our own???
						slsi_save_ap_config(g_ap_config);
					}
				}
#endif
			}
			break;
		}
#ifdef CONFIG_SLSI_WIFI_P2P_API
		case SLSI_WIFI_P2P_IF: {
			VPRINT("Start P2P\n");
			// TODO: add parameters from API call
			if (slsi_get_op_mode() == SLSI_WIFI_SOFT_AP_IF) {
				DPRINT("Stop AP mode first \n");
				result = slsi_ap_stop();
			} else if (slsi_get_op_mode() == SLSI_WIFI_STATION_IF) {
				DPRINT("Stop STA mode first \n");
				result = slsi_sta_stop();
			}
			slsi_set_ifname(interface_id);
			slsi_init_p2p();
			if (result == SLSI_STATUS_SUCCESS) {
				result = slsi_p2p_start();
				if (result == SLSI_STATUS_SUCCESS) {
					g_state = SLSI_WIFIAPI_STATE_P2P;
				}
			} else {
				EPRINT("Failed to stop running modes!\n");
			}
			break;
		}
#endif
		default:
			EPRINT("No interface specified \n");
			break;
		}

		/* Do not assign global until after used to check for running interfaces
		 * If i.e. start of AP mode fails then supplicant is still running and we
		 * need to make sure that the interface type is set in that situation */
#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
		if ((result == SLSI_STATUS_SUCCESS || (g_task != 0 && g_state == SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING)) && !g_recovering) {
			g_recovery_data.old_interface_type = interface_id;
		}
#endif
		if (result != SLSI_STATUS_SUCCESS && (slsi_get_op_mode() == SLSI_WIFI_SOFT_AP_IF)) {
			if (g_network_id) {
				free(g_network_id);
				g_network_id = NULL;
			}
		}
	} else {
		slsi_deinit();
	}
	return result;
}

static WiFi_InterFace_ID_t slsi_get_op_mode(void)
{
	if (g_state >= SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING && g_state <= SLSI_WIFIAPI_STATE_STA_DISCONNECTING) {
		return SLSI_WIFI_STATION_IF;
	} else if (g_state >= SLSI_WIFIAPI_STATE_AP_ENABLING && g_state <= SLSI_WIFIAPI_STATE_AP_DISABLING) {
		return SLSI_WIFI_SOFT_AP_IF;
	}
#ifdef CONFIG_SLSI_WIFI_P2P_API
	else if (g_state == SLSI_WIFIAPI_STATE_P2P) {
		return SLSI_WIFI_P2P_IF;
	}
#endif
	return SLSI_WIFI_NONE;
}

/**
 * Public interface
 */
int8_t WiFiStart(WiFi_InterFace_ID_t interface_id, const slsi_ap_config_t *ap_config)
{
	int8_t result = SLSI_STATUS_ERROR;
	if (interface_id == SLSI_WIFI_NONE || ((interface_id == SLSI_WIFI_SOFT_AP_IF) && (ap_config == NULL))) {
		/* Not a valid parameters */
		DPRINT("WiFiStart parameters not set\n");
		result = SLSI_STATUS_PARAM_FAILED;
		goto errout;
	}

	if (interface_id == slsi_get_op_mode()) {
		DPRINT("WiFiStart interface type already enabled\n");
		result = SLSI_STATUS_ALREADY_STARTED;
		goto errout;
	}
	ENTER_CRITICAL;

	result = slsi_api_start(interface_id, ap_config);

	LEAVE_CRITICAL;
errout:
	return result;
}

int8_t WiFiStop(void)
{
	int8_t result = SLSI_STATUS_ERROR;
	if (g_state == SLSI_WIFIAPI_STATE_NOT_STARTED) {
		DPRINT("SLSI_WiFiStop - no interface started\n");
		return SLSI_STATUS_SUCCESS;
	}
	DPRINT("SLSI_WiFiStop\n");
	ENTER_CRITICAL;

	if (g_state == SLSI_WIFIAPI_STATE_STA_CONNECTED) {
		// Disconnect if in STA mode and connected
		g_state = SLSI_WIFIAPI_STATE_STA_DISCONNECTING;
		result = slsi_leave_network(NULL);
		if (result == SLSI_STATUS_SUCCESS) {
			int res = sem_wait(&g_sem_disconnect);
			if (res) {
				EPRINT("SLSI_API sem_wait error %d\n", errno);
			}
		} else {
			EPRINT("%s Error: disconnect. \n", __func__);
		}
		// Still progress shutting down
	} else if (slsi_get_op_mode() == SLSI_WIFI_SOFT_AP_IF) {
		// Stop AP mode if in AP mode
		VPRINT("Stop AP mode\n");
		g_state = SLSI_WIFIAPI_STATE_AP_DISABLING;
		result = slsi_stop_ap();
		// remove network
		if (g_network_id) {
			slsi_remove_network(g_network_id);
			free(g_network_id);
			g_network_id = NULL;
		}
	}
#ifdef CONFIG_SLSI_WIFI_P2P_API
	// Stop P2P mode
	else if (g_state == SLSI_WIFIAPI_STATE_P2P) {
		VPRINT("Stop P2P mode\n");
		result = slsi_p2p_stop();
	}
#endif
	else {
		result = SLSI_STATUS_SUCCESS;
	}

	sem_post(&g_sem_disconnect);	//for svace
	LEAVE_CRITICAL;
	ENTER_CRITICAL;				// needed as some other thread might have locked the mutex by now

	// Stop supplicant
	if (result == SLSI_STATUS_SUCCESS) {
#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
		slsi_clean_recover();
#endif
		result = slsi_stop_supplicant();
		// CLean up local
		slsi_deinit();
	}

	VPRINT("Stopped\n");
	pthread_mutex_destroy(&mutex_state);
	g_mutex_initialized = FALSE;
//    LEAVE_CRITICAL; //Not actually doing anything as the mutex is already destroyed
	return result;
}

int8_t WiFiScanNetwork(void)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_STARTED;
	if (g_state != SLSI_WIFIAPI_STATE_NOT_STARTED) {
		result = slsi_start_scan();
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiScanSpecificNetwork(uint8_t *ssid, uint8_t ssid_len, const slsi_security_config_t *security_config)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_STARTED;
	if (g_state != SLSI_WIFIAPI_STATE_NOT_STARTED) {
		result = slsi_start_specific_scan(ssid, ssid_len, security_config);
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiGetScanResults(slsi_scan_info_t **scan_results)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_STARTED;
	if (g_state != SLSI_WIFIAPI_STATE_NOT_STARTED) {
		result = slsi_get_api_scan_results(scan_results);
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiFreeScanResults(slsi_scan_info_t **scan_results)
{
	ENTER_CRITICAL;
	slsi_scan_info_t *tmp, *tmp_bss_list = *scan_results;
	// This will handle passing a NULL pointer
	while (tmp_bss_list != NULL) {
		tmp = tmp_bss_list;
		tmp_bss_list = tmp_bss_list->next;
		free(tmp->sec_modes);
		tmp->sec_modes = NULL;
		slsi_vendor_ie_t *vsie, *vsielist = tmp->vsie;
		while (vsielist != NULL) {
			vsie = vsielist;
			vsielist = vsielist->next;
			free(vsie->content);
			vsie->content = NULL;
			free(vsie);
			vsie = NULL;
		}
		free(tmp);
	}
	tmp = NULL;
	*scan_results = NULL;
	LEAVE_CRITICAL;
	return SLSI_STATUS_SUCCESS;
}

int8_t WiFiRegisterLinkCallback(slsi_network_link_callback_t link_up, slsi_network_link_callback_t link_down)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_ERROR;
	slsi_reason_t reason;
	memset(&reason, 0, sizeof(slsi_reason_t));
	g_link_up = link_up;
	g_link_down = link_down;
#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
	g_recovery_data.link_up = link_up;
	g_recovery_data.link_down = link_down;
#endif
	if (g_running) {
		result = slsi_check_status(reason.ssid, &reason.ssid_len, reason.bssid);
		if (result == SLSI_STATUS_SUCCESS) {
			if (slsi_is_connected()) {	// Mind SSID can be empty for hidden networks
				if (g_link_up) {
					slsi_msg_callback_t msg;
					msg.reason = reason;
					slsi_send_mqueue(SLSI_CALLBACK_LINK_UP, &msg);
				}
			}
		}
	} else {
		result = SLSI_STATUS_SUCCESS;
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiRegisterScanCallback(slsi_scan_result_callback_t scan_result_handler)
{
	ENTER_CRITICAL;

	g_scan_result_handler = scan_result_handler;
#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
	g_recovery_data.scan_result_handler = scan_result_handler;
#endif
	LEAVE_CRITICAL;
	return SLSI_STATUS_SUCCESS;
}

int8_t WiFiNetworkJoin(uint8_t *ssid, uint8_t ssid_len, uint8_t *bssid, const slsi_security_config_t *security_config)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_STARTED;
	if (g_state == SLSI_WIFIAPI_STATE_STA_CONNECTED) {
		result = SLSI_STATUS_ALREADY_CONNECTED;
	} else if (g_state == SLSI_WIFIAPI_STATE_SUPPLICANT_RUNNING) {
		if ((security_config == NULL) || ((security_config != NULL) && (security_config->secmode != 0) && (security_config->passphrase[0] != '\0'))) {
			// Save network information is case of recovery handling
#ifdef CONFIG_SCSC_WLAN_AUTO_RECOVERY
			if (!g_recovering) {
				slsi_save_join(ssid, ssid_len, bssid, security_config);
			}
#endif
			//Call to join network
			result = slsi_join_network(ssid, ssid_len, bssid, security_config);
		} else {
			EPRINT("SLSI_API NetworkJoin - error on security config\n");
		}
	} else {
		EPRINT("SLSI_API Invalid Join request - current state: %s\n", slsi_state_strings[g_state]);
	}

	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiNetworkLeave(void)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_STARTED;
	if (g_state != SLSI_WIFIAPI_STATE_NOT_STARTED) {
		if (slsi_is_connected()) {
			g_state = SLSI_WIFIAPI_STATE_STA_DISCONNECTING;
			result = slsi_leave_network(NULL);
			if (result == SLSI_STATUS_SUCCESS) {
				VPRINT("Before wait sem count %d\n", g_sem_disconnect.semcount);
				int res = sem_wait(&g_sem_disconnect);
				if (res) {
					EPRINT("g_sem_disconnect sem_wait error %d\n", errno);
				}
			} else {
				EPRINT("%s Error: disconnect. \n", __func__);
			}
		} else {
			result = SLSI_STATUS_NOT_CONNECTED;
			DPRINT("SLSI_API NetworkLeave - not connected\n");
		}
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiGetTxPower(uint8_t *dbm)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_STARTED;
	if (g_state != SLSI_WIFIAPI_STATE_NOT_STARTED) {
		if (dbm != NULL) {
			result = slsi_get_tx_power(dbm);
		} else {
			if (slsi_is_connected()) {
				result = SLSI_STATUS_PARAM_FAILED;
			} else {
				result = SLSI_STATUS_NOT_CONNECTED;
			}
		}
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiSetTxPower(uint8_t *dbm)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_STARTED;
	if (dbm != NULL) {
		if (g_state != SLSI_WIFIAPI_STATE_NOT_STARTED) {
			if (slsi_is_connected()) {
				result = slsi_set_tx_power(dbm, TRUE, TRUE);
			} else {
				DPRINT("Supplicant not connected - setting in NVRAM only\n");
				result = slsi_set_tx_power(dbm, TRUE, FALSE);
			}
		} else {
			DPRINT("Supplicant not running - setting in NVRAM only\n");
			result = slsi_set_tx_power(dbm, TRUE, FALSE);
		}
	} else {
		result = SLSI_STATUS_PARAM_FAILED;
		DPRINT("No value provided for tx power\n");
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiGetMac(uint8_t *mac)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_STARTED;
	if (mac != NULL) {
		if (g_state != SLSI_WIFIAPI_STATE_NOT_STARTED) {
			result = slsi_get_mac(mac);
		}
	} else {
		result = SLSI_STATUS_PARAM_FAILED;
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiGetRssi(int8_t *rssi)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_STARTED;
	if (rssi != NULL) {
		if (g_state != SLSI_WIFIAPI_STATE_NOT_STARTED) {
			if (slsi_is_connected()) {
				result = slsi_get_rssi(rssi);
			} else {
				result = SLSI_STATUS_NOT_CONNECTED;
			}
		}
	} else {
		result = SLSI_STATUS_PARAM_FAILED;
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiGetChannel(int8_t *channel)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_STARTED;
	if (channel != NULL) {
		if (g_state != SLSI_WIFIAPI_STATE_NOT_STARTED) {
			if (slsi_is_connected()) {
				result = slsi_get_channel(channel);
			} else {
				result = SLSI_STATUS_NOT_CONNECTED;
			}
		}
	} else {
		result = SLSI_STATUS_PARAM_FAILED;
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiIsConnected(uint8_t *ret, slsi_reason_t *details)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_CONNECTED;
	uint8_t count = 0;
	if (details != NULL) {
		memset(details, 0, sizeof(slsi_reason_t));
	}

	if (g_state == SLSI_WIFIAPI_STATE_STA_CONNECTED) {
		count = 1;
		result = SLSI_STATUS_SUCCESS;
		if (details != NULL) {
			result = slsi_check_status(details->ssid, &details->ssid_len, details->bssid);
		}
	} else if (g_state == SLSI_WIFIAPI_STATE_AP_CONNECTED) {
		count = g_num_sta_connected;
		result = SLSI_STATUS_SUCCESS;
	} else if (g_state == SLSI_WIFIAPI_STATE_NOT_STARTED) {
		result = SLSI_STATUS_NOT_STARTED;
	}

	*ret = count;
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiGetOpMode(WiFi_InterFace_ID_t *ret)
{
	*ret = slsi_get_op_mode();
	return SLSI_STATUS_SUCCESS;
}

int8_t WiFiSetCountryCode(const char *country_code)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_ERROR;

	bool write_to_nvram = FALSE;
	bool write_to_driver = FALSE;

	if (country_code != NULL) {
		if (slsi_get_op_mode() == SLSI_WIFI_STATION_IF) {
			// we can send to supplicant in station mode at any point.
			// it will however not be used until next scan an will not impact
			// existing connections.
			write_to_nvram = TRUE;
			write_to_driver = TRUE;
		} else {
			write_to_nvram = TRUE;
			write_to_driver = FALSE;
		}
		result = slsi_set_country_code(country_code, write_to_nvram, write_to_driver);
	} else {
		DPRINT("SetCountryCode - Countrycode not provided\n");
		result = SLSI_STATUS_PARAM_FAILED;
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiGetCountryCode(char *country_code)
{
	ENTER_CRITICAL;
	int8_t result = SLSI_STATUS_NOT_STARTED;
	if (g_state != SLSI_WIFIAPI_STATE_NOT_STARTED) {
		result = slsi_get_country_code(country_code);
	} else {
		DPRINT("GetCountryCode - Interface not stared \n");
	}
	LEAVE_CRITICAL;
	return result;
}

int8_t WiFiSaveConfig(void)
{
	int8_t result = SLSI_STATUS_NOT_SUPPORTED;
#ifdef CONFIG_SLSI_WIFI_FILESYSTEM_SUPPORT
	result = SLSI_STATUS_NOT_STARTED;
	ENTER_CRITICAL;
	if (slsi_get_op_mode() == SLSI_WIFI_STATION_IF) {
		result = slsi_save_config();
	} else if (g_state == SLSI_WIFIAPI_STATE_NOT_STARTED) {
		DPRINT("WiFiSaveConfig - system not started\n");
	} else {
		DPRINT("WiFiSaveConfig - not allowed during AP mode\n");
		result = SLSI_STATUS_NOT_ALLOWED;
	}
	LEAVE_CRITICAL;
#endif
	return result;
}
