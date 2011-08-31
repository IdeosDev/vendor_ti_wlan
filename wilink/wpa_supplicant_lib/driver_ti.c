/*
 * driver_ti.c
 *
 * Copyright 2001-2010 Texas Instruments, Inc. - http://www.ti.com/
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "includes.h"
#include <sys/ioctl.h>
#include <net/if_arp.h>
#ifdef ANDROID
#include <cutils/properties.h>
#endif
#include "driver_ti.h"
#include "scanmerge.h"
#ifdef TI_WAPI
#include "ti_wapi.h"
#endif


/*-------------------------------------------------------------------*/
#define TI2WPA_STATUS(s)	(((s) != 0) ? -1 : 0)
#define TI_CHECK_DRIVER(f,r)	\
	if( !(f) ) { \
		wpa_printf(MSG_ERROR,"TI: Driver not initialized yet"); \
		return( r ); \
	}

static int scan_results_received;

/*-----------------------------------------------------------------------------
Routine Name: check_and_get_build_channels
Routine Description: get number of allowed channels according to a build var.
Arguments: None
Return Value: Number of channels
-----------------------------------------------------------------------------*/
static int check_and_get_build_channels( void )
{
#ifdef ANDROID
	char prop_status[PROPERTY_VALUE_MAX];
	char *prop_name = "ro.wifi.channels";
	int i, default_channels = NUMBER_SCAN_CHANNELS_ETSI;

	if ( property_get(prop_name, prop_status, NULL) )
	{
		i = atoi(prop_status);
		if ( i != 0 )
			default_channels = i;
	}
	return( default_channels );
#else
	return( NUMBER_SCAN_CHANNELS_FCC );
#endif
}

static int wpa_driver_tista_cipher2wext(int cipher)
{
	switch (cipher)
	{
	case CIPHER_NONE:
		return IW_AUTH_CIPHER_NONE;
	case CIPHER_WEP40:
		return IW_AUTH_CIPHER_WEP40;
	case CIPHER_TKIP:
		return IW_AUTH_CIPHER_TKIP;
	case CIPHER_CCMP:
		return IW_AUTH_CIPHER_CCMP;
	case CIPHER_WEP104:
		return IW_AUTH_CIPHER_WEP104;
#ifdef TI_WAPI
	case CIPHER_SMS4:
		return IW_AUTH_CIPHER_SMS4;
#endif
	default:
		return 0;
	}
}

static int wpa_driver_tista_keymgmt2wext(int keymgmt)
{
	switch (keymgmt)
	{
	case KEY_MGMT_802_1X:
	case KEY_MGMT_802_1X_NO_WPA:
		return IW_AUTH_KEY_MGMT_802_1X;
	case KEY_MGMT_PSK:
		return IW_AUTH_KEY_MGMT_PSK;
	default:
		return 0;
	}
}

static int wpa_driver_tista_get_bssid(void *priv, u8 *bssid)
{
	struct wpa_driver_ti_data *drv = priv;
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	return wpa_driver_wext_get_bssid(drv->wext, bssid);
}

static int wpa_driver_tista_get_ssid(void *priv, u8 *ssid)
{
	struct wpa_driver_ti_data *drv = priv;
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	return wpa_driver_wext_get_ssid(drv->wext, ssid);
}

static int wpa_driver_tista_private_send( void *priv, u32 ioctl_cmd, void *bufIn, u32 sizeIn, void *bufOut, u32 sizeOut )
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	ti_private_cmd_t private_cmd;
	struct iwreq iwr;
	s32 res;

	private_cmd.cmd = ioctl_cmd;
	if (bufOut == NULL)
		private_cmd.flags = PRIVATE_CMD_SET_FLAG;
	else
		private_cmd.flags = PRIVATE_CMD_GET_FLAG;

	private_cmd.in_buffer = bufIn;
	private_cmd.in_buffer_len = sizeIn;
	private_cmd.out_buffer = bufOut;
	private_cmd.out_buffer_len = sizeOut;

	os_memset(&iwr, 0, sizeof(iwr));
	os_strncpy(iwr.ifr_name, drv->ifname, IFNAMSIZ);

	iwr.u.data.pointer = &private_cmd;
	iwr.u.data.length = sizeof(ti_private_cmd_t);
	iwr.u.data.flags = 0;

	res = ioctl(drv->ioctl_sock, SIOCIWFIRSTPRIV, &iwr);
	if (0 != res)
	{
		wpa_printf(MSG_ERROR, "ERROR - wpa_driver_tista_private_send - error sending Wext private IOCTL to STA driver (ioctl_cmd = %x,  res = %d, errno = %d)", ioctl_cmd, res, errno);
		drv->errors++;
		if (drv->errors > MAX_NUMBER_SEQUENTIAL_ERRORS)
		{
			drv->errors = 0;
			wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "HANGED");
		}
		return -1;
	}
	drv->errors = 0;
	wpa_printf(MSG_DEBUG, "wpa_driver_tista_private_send ioctl_cmd = %x  res = %d", ioctl_cmd, res);

	return 0;
}

static int wpa_driver_tista_driver_start( void *priv )
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	u32 uDummyBuf;
	s32 res;

	res = wpa_driver_tista_private_send(priv, DRIVER_START_PARAM, &uDummyBuf, sizeof(uDummyBuf), NULL, 0);

	if (0 != res)
	{
		wpa_printf(MSG_ERROR, "ERROR - Failed to start driver!");
		wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "HANGED");
	}
	else
	{
		os_sleep(0, WPA_DRIVER_WEXT_WAIT_US); /* delay 400 ms */
		wpa_printf(MSG_DEBUG, "wpa_driver_tista_driver_start success");
	}
	return res;
}

static int wpa_driver_tista_driver_stop( void *priv )
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	u32 uDummyBuf;
	s32 res;

	res = wpa_driver_tista_private_send(priv, DRIVER_STOP_PARAM, &uDummyBuf, sizeof(uDummyBuf), NULL, 0);

	if (0 != res)
	{
		wpa_printf(MSG_ERROR, "ERROR - Failed to stop driver!");
		wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "HANGED");
	}
	else
		wpa_printf(MSG_DEBUG, "wpa_driver_tista_driver_stop success");

	return res;
}

int wpa_driver_tista_parse_custom(void *ctx, const void *custom)
{
	IPC_EV_DATA * pData = NULL;

	pData = (IPC_EV_DATA *)custom;
	wpa_printf(MSG_DEBUG, "uEventType %d", pData->EvParams.uEventType);
	switch (pData->EvParams.uEventType)
	{
	case	IPC_EVENT_LINK_SPEED:
		wpa_printf(MSG_DEBUG, "IPC_EVENT_LINK_SPEED");
		if (pData->uBufferSize == sizeof(u32))
		{
			wpa_printf(MSG_DEBUG, "update link_speed");
			/* Dm: pStaDrv->link_speed = *((u32 *)pData->uBuffer) / 2; */
		}

		/* Dm: wpa_printf(MSG_INFO,"wpa_supplicant - Link Speed = %u", pStaDrv->link_speed ); */
		break;
#ifdef CONFIG_WPS
	case	IPC_EVENT_WPS_SESSION_OVERLAP:
		wpa_printf(MSG_INFO, "IPC_EVENT_WPS_SESSION_OVERLAP");
		wpa_supplicant_event(ctx, EVENT_WSC_PBC_OVERLAP, NULL);
		break;
#endif /* CONFIG_WPS */
	default:
		wpa_printf(MSG_DEBUG, "Unknown event");
		break;
	}

	return 0;
}

static void ti_init_scan_params( scan_Params_t *pScanParams, int scanType,
                                 int noOfChan, int scan_probe_flag )
{
	u8 i,j;
	int maxDwellTime = 110000;

	/* init application scan default params */
	pScanParams->desiredSsid.len = 0;
	/* all scan, we will use active scan */
	pScanParams->scanType = scanType;
	if ((scanType == SCAN_TYPE_NORMAL_ACTIVE) && scan_probe_flag)
		maxDwellTime = 30000;

	pScanParams->band = RADIO_BAND_2_4_GHZ;
	pScanParams->probeReqNumber = 3;
	pScanParams->probeRequestRate = RATE_MASK_UNSPECIFIED; /* Let the FW select */;
	pScanParams->Tid = 0;
	pScanParams->numOfChannels = noOfChan;
	for ( i = 0; i < noOfChan; i++ )
	{
		for ( j = 0; j < 6; j++ )
		{
			pScanParams->channelEntry[ i ].normalChannelEntry.bssId[ j ] = 0xff;
		}
		pScanParams->channelEntry[ i ].normalChannelEntry.earlyTerminationEvent = SCAN_ET_COND_DISABLE;
		pScanParams->channelEntry[ i ].normalChannelEntry.ETMaxNumOfAPframes = 0;
		pScanParams->channelEntry[ i ].normalChannelEntry.maxChannelDwellTime = maxDwellTime;
		pScanParams->channelEntry[ i ].normalChannelEntry.minChannelDwellTime = maxDwellTime;
		pScanParams->channelEntry[ i ].normalChannelEntry.txPowerDbm = DEF_TX_POWER;
		pScanParams->channelEntry[ i ].normalChannelEntry.channel = i + 1;
	}
}

/*-----------------------------------------------------------------------------
Routine Name: wpa_driver_tista_scan
Routine Description: request scan from driver
Arguments:
   priv - pointer to private data structure
   ssid - ssid buffer
   ssid_len - length of ssid
Return Value: 0 on success, -1 on failure
-----------------------------------------------------------------------------*/
static int wpa_driver_tista_scan( void *priv, const u8 *ssid, size_t ssid_len )
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)(drv->ctx);
	struct wpa_ssid *issid;
	scan_Params_t scanParams;
	int scan_type, res, scan_probe_flag = 0;

	wpa_printf(MSG_DEBUG, "%s", __func__);
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );

#if 1
	os_memset(&scanParams, 0, sizeof(scan_Params_t));
	/* Initialize scan parameters */
	scan_type = drv->scan_type;
	if (wpa_s->prev_scan_ssid != BROADCAST_SSID_SCAN)
	{
		if (wpa_s->prev_scan_ssid->scan_ssid)
		{
			scan_type = SCAN_TYPE_NORMAL_ACTIVE;
			scan_probe_flag = 1;
		}
	}
	ti_init_scan_params(&scanParams, scan_type, drv->scan_channels,
	                    scan_probe_flag);

	drv->force_merge_flag = 0; /* Set merge flag */

	if ((scan_probe_flag && ssid) &&
	        (ssid_len > 0 && ssid_len <= sizeof(scanParams.desiredSsid.str)))
	{
		os_memcpy(scanParams.desiredSsid.str, ssid, ssid_len);
		if (ssid_len < sizeof(scanParams.desiredSsid.str))
			scanParams.desiredSsid.str[ssid_len] = '\0';
		scanParams.desiredSsid.len = ssid_len;
		drv->force_merge_flag = 1;
	}

	drv->last_scan = scan_type; /* Remember scan type for last scan */

	res = wpa_driver_tista_private_send(priv, TIWLN_802_11_START_APP_SCAN_SET, &scanParams, sizeof(scanParams), NULL, 0);

	if (0 != res)
		wpa_printf(MSG_ERROR, "ERROR - Failed to do tista scan!");
	else
	{
		eloop_cancel_timeout(wpa_driver_wext_scan_timeout, drv,
		                     drv->ctx);
		eloop_register_timeout(15, 0, wpa_driver_wext_scan_timeout,
		                       drv, drv->ctx);
		wpa_printf(MSG_DEBUG, "wpa_driver_tista_scan success");
	}

	return res;
#else
	return wpa_driver_wext_scan(drv->wext, ssid, ssid_len);
#endif
}

/*-----------------------------------------------------------------------------
Routine Name: wpa_driver_tista_get_mac_addr
Routine Description: return WLAN MAC address
Arguments:
   priv - pointer to private data structure
Return Value: pointer to BSSID
-----------------------------------------------------------------------------*/
const u8 *wpa_driver_tista_get_mac_addr( void *priv )
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	u8 mac[ETH_ALEN] = {0};

	TI_CHECK_DRIVER( drv->driver_is_loaded, NULL );
	if (0 != wpa_driver_tista_private_send(priv, CTRL_DATA_MAC_ADDRESS, NULL, 0,
	                                       mac, ETH_ALEN))
	{
		wpa_printf(MSG_ERROR, "ERROR - Failed to get mac address!");
		os_memset(drv->own_addr, 0, ETH_ALEN);
	}
	else
	{
		os_memcpy(drv->own_addr, mac, ETH_ALEN);
		wpa_printf(MSG_DEBUG, "Macaddr = " MACSTR, MAC2STR(drv->own_addr));
	}
	wpa_printf(MSG_DEBUG, "wpa_driver_tista_get_mac_addr success");

	return (const u8 *)&drv->own_addr;
}

static int wpa_driver_tista_get_rssi(void *priv, int *rssi_data, int *rssi_beacon)
{
	u8 bssid[ETH_ALEN];
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	TCuCommon_RoamingStatisticsTable buffer;

	os_memset(&buffer, 0, sizeof(TCuCommon_RoamingStatisticsTable));
	*rssi_data = 0;
	*rssi_beacon = 0;
	if (wpa_driver_tista_get_bssid(priv, bssid) == 0 &&
	        os_memcmp(bssid, "\x00\x00\x00\x00\x00\x00", ETH_ALEN) != 0)
	{
		if (0 != wpa_driver_tista_private_send(priv, TIWLN_802_11_RSSI, NULL, 0,
		                                       &buffer, sizeof(TCuCommon_RoamingStatisticsTable)))
		{
			wpa_printf(MSG_ERROR, "ERROR - Failed to get rssi level");
			return -1;
		}
		*rssi_data = (s8)buffer.rssi;
		*rssi_beacon = (s8)buffer.rssiBeacon;
		wpa_printf(MSG_DEBUG, "wpa_driver_tista_get_rssi data %d beacon %d success",
		           *rssi_data, *rssi_beacon);
	}
	else
	{
		wpa_printf(MSG_DEBUG, "no WiFi link.");
		return -1;
	}
	return 0;
}

static int wpa_driver_tista_config_power_management(void *priv, TPowerMgr_PowerMode *mode, u8 is_set)
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;

	if (is_set) /* set power mode */
	{
		if ((mode->PowerMode) < POWER_MODE_MAX)
		{
			if (0 != wpa_driver_tista_private_send(priv, TIWLN_802_11_POWER_MODE_SET,
			                                       mode, sizeof(TPowerMgr_PowerMode), NULL, 0))
			{
				wpa_printf(MSG_ERROR, "ERROR - Failed to set power mode");
				return -1;
			}
		}
		else
		{
			wpa_printf(MSG_ERROR, "ERROR - Invalid Power Mode");
			return -1;
		}
	}
	else /* get power mode */
	{
		if (0 != wpa_driver_tista_private_send(priv, TIWLN_802_11_POWER_MODE_GET, NULL, 0,
		                                       mode, sizeof(TPowerMgr_PowerMode)))
		{
			wpa_printf(MSG_ERROR, "ERROR - Failed to get power mode");
			return -1;
		}
	}
	wpa_printf(MSG_DEBUG, "wpa_driver_tista_config_power_management success");

	return 0;
}

static int wpa_driver_tista_enable_bt_coe(void *priv, u32 mode)
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	u32 mode_set = mode;

	/* Mapping the mode between UI enum and driver enum */
	switch (mode_set)
	{
	case BLUETOOTH_COEXISTENCE_MODE_ENABLED:
		mode_set = SG_OPPORTUNISTIC;
		break;
	case BLUETOOTH_COEXISTENCE_MODE_SENSE:
		mode_set = SG_PROTECTIVE;
		break;
	case BLUETOOTH_COEXISTENCE_MODE_DISABLED:
		mode_set = SG_DISABLE;
		break;
	default:
		wpa_printf(MSG_DEBUG, "wpa_driver_tista_enable_bt_coe - Unknown Mode");
		return -1;
		break;
	}

	if (0 != wpa_driver_tista_private_send(priv, SOFT_GEMINI_SET_ENABLE,
	                                       &mode_set, sizeof(u32), NULL, 0))
	{
		wpa_printf(MSG_ERROR, "ERROR - Failed to enable BtCoe");
		return -1;
	}
	wpa_printf(MSG_DEBUG, "wpa_driver_tista_enable_bt_coe success");

	return 0;
}

static int wpa_driver_tista_get_bt_coe_status(void *priv, u32 *mode)
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	u32 mode_get = 0;

	if (0 != wpa_driver_tista_private_send(priv, SOFT_GEMINI_GET_CONFIG, NULL, 0,
	                                       &mode_get, sizeof(u32)))
	{
		wpa_printf(MSG_ERROR, "ERROR - Failed to get bt coe status");
		return -1;
	}
	*mode = mode_get;
	wpa_printf(MSG_DEBUG, "wpa_driver_tista_get_bt_coe_status mode %d success", *mode);

	return 0;
}

/*-----------------------------------------------------------------------------
Routine Name: prepare_filter_struct
Routine Description: fills rx data filter structure according to parameter type
Arguments:
   priv - pointer to private data structure
   type - type of mac address
   dfreq_ptr - pointer to TRxDataFilterRequest structure
Return Value: 0 - success, -1 - error
-----------------------------------------------------------------------------*/
static int prepare_filter_struct( void *priv, int type,
                                  TRxDataFilterRequest *dfreq_ptr )
{
	const u8 *macaddr = NULL;
	size_t len = 0;
	u8 mask;
	int ret = -1;

	wpa_printf(MSG_DEBUG, "filter type=%d", type);
	switch (type)
	{
	case RX_SELF_FILTER:
		macaddr = wpa_driver_tista_get_mac_addr(priv);
		len = MAC_ADDR_LEN;
		mask = 0x3F; /* 6 bytes */
		break;
	case RX_BROADCAST_FILTER:
		macaddr = (const u8 *)"\xFF\xFF\xFF\xFF\xFF\xFF";
		len = MAC_ADDR_LEN;
		mask = 0x3F; /* 6 bytes */
		break;
	case RX_IPV4_MULTICAST_FILTER:
		macaddr = (const u8 *)"\x01\x00\x5E";
		len = 3;
		mask = 0x7; /* 3 bytes */
		break;
	case RX_IPV6_MULTICAST_FILTER:
		macaddr = (const u8 *)"\x33\x33";
		len = 2;
		mask = 0x3; /* 2 bytes */
		break;
	}

	if (macaddr != NULL)
	{
		dfreq_ptr->offset = 0;
		dfreq_ptr->maskLength = 1;
		dfreq_ptr->mask[0] = mask;
		dfreq_ptr->patternLength = len;
		os_memcpy( dfreq_ptr->pattern, macaddr, MAC_ADDR_LEN );
		ret = 0;
	}
	return ret;
}

static int wpa_driver_tista_driver_rx_data_filter( void *priv, TRxDataFilterRequest *dfreq_ptr, u8 is_add )
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	int cmd, res;

	if (is_add)   /* add rx data filter */
	{
		cmd = TIWLN_ADD_RX_DATA_FILTER;
		wpa_printf(MSG_DEBUG, "Add RX data filter");
	}
	else   /* remove rx data filter */
	{
		cmd = TIWLN_REMOVE_RX_DATA_FILTER;
		wpa_printf(MSG_DEBUG, "Remove RX data filter");
	}

	res = wpa_driver_tista_private_send(priv, cmd, dfreq_ptr, sizeof(TRxDataFilterRequest), NULL, 0);
	if (0 != res)
		wpa_printf(MSG_ERROR, "ERROR - Failed to handle rx data filter command!");
	else
		wpa_printf(MSG_DEBUG, "%s success", __func__);
	return res;
}

static int wpa_driver_tista_driver_enable_rx_data_filter( void *priv )
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	u32 val = TRUE;
	int res;

	res = wpa_driver_tista_private_send(priv, TIWLN_ENABLE_DISABLE_RX_DATA_FILTERS,
	                                    &val, sizeof(u32), NULL, 0);
	if (0 != res)
		wpa_printf(MSG_ERROR, "ERROR - Failed to enable RX data filter!");
	else
		wpa_printf(MSG_DEBUG, "%s success", __func__);
	return res;
}

static int wpa_driver_tista_driver_disable_rx_data_filter( void *priv )
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	u32 val = FALSE;
	int res;

	res = wpa_driver_tista_private_send(priv, TIWLN_ENABLE_DISABLE_RX_DATA_FILTERS,
	                                    &val, sizeof(u32), NULL, 0);
	if (0 != res)
		wpa_printf(MSG_ERROR, "ERROR - Failed to disable RX data filter!");
	else
		wpa_printf(MSG_DEBUG, "%s success", __func__);
	return res;
}

static int wpa_driver_tista_driver_rx_data_filter_statistics( void *priv,
        TCuCommon_RxDataFilteringStatistics *stats )
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	int res;

	res = wpa_driver_tista_private_send(priv, TIWLN_GET_RX_DATA_FILTERS_STATISTICS,
	                                    NULL, 0, stats, sizeof(TCuCommon_RxDataFilteringStatistics));
	if (0 != res)
		wpa_printf(MSG_ERROR, "ERROR - Failed to get RX data filter statistics!");
	else
		wpa_printf(MSG_DEBUG, "%s success", __func__);
	return res;
}

/*-----------------------------------------------------------------------------
Routine Name: wpa_driver_tista_driver_cmd
Routine Description: executes driver-specific commands
Arguments:
   priv - pointer to private data structure
   cmd - command
   buf - return buffer
   buf_len - buffer length
Return Value: actual buffer length - success, -1 - failure
-----------------------------------------------------------------------------*/
static int wpa_driver_tista_driver_cmd( void *priv, char *cmd, char *buf, size_t buf_len )
{
	struct wpa_driver_ti_data *drv = (struct wpa_driver_ti_data *)priv;
	int ret = -1, prev_events;

	wpa_printf(MSG_DEBUG, "%s %s", __func__, cmd);

	if ( os_strcasecmp(cmd, "start") == 0 )
	{
		wpa_printf(MSG_DEBUG,"Start command");
		ret = wpa_driver_tista_driver_start(priv);
		if ( ret == 0 )
		{
			drv->driver_is_loaded = TRUE;
			wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "STARTED");
		}
		return( TI2WPA_STATUS(ret) );
	}

	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );

	if ( os_strcasecmp(cmd, "stop") == 0 )
	{
		wpa_printf(MSG_DEBUG,"Stop command");
		ret = wpa_driver_tista_driver_stop(priv);
		if ( ret == 0 )
		{
			drv->driver_is_loaded = FALSE;
			wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "STOPPED");
		}
	}
	if ( os_strcasecmp(cmd, "reload") == 0 )
	{
		wpa_printf(MSG_DEBUG,"Reload command");
		ret = 0;
		wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "HANGED");
	}
	else if ( os_strcasecmp(cmd, "macaddr") == 0 )
	{
		wpa_driver_tista_get_mac_addr(priv);
		wpa_printf(MSG_DEBUG, "Macaddr command");
		ret = sprintf(buf, "Macaddr = " MACSTR "\n", MAC2STR(drv->own_addr));
		wpa_printf(MSG_DEBUG, "buf %s", buf);
	}
	else if ( os_strcasecmp(cmd, "scan-passive") == 0 )
	{
		wpa_printf(MSG_DEBUG,"Scan Passive command");
		drv->scan_type =  SCAN_TYPE_NORMAL_PASSIVE;
		ret = 0;
	}
	else if ( os_strcasecmp(cmd, "scan-active") == 0 )
	{
		wpa_printf(MSG_DEBUG,"Scan Active command");
		drv->scan_type =  SCAN_TYPE_NORMAL_ACTIVE;
		ret = 0;
	}
	else if ( os_strcasecmp(cmd, "scan-mode") == 0 )
	{
		wpa_printf(MSG_DEBUG,"Scan Mode command");
		ret = snprintf(buf, buf_len, "ScanMode = %u\n", drv->scan_type);
		if (ret < (int)buf_len)
		{
			return( ret );
		}
	}
	else if ( os_strcasecmp(cmd, "linkspeed") == 0 )
	{
		struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)(drv->ctx);

		wpa_printf(MSG_DEBUG,"Link Speed command");
		drv->link_speed = wpa_s->link_speed / 1000000;
		ret = sprintf(buf,"LinkSpeed %u\n", drv->link_speed);
		wpa_printf(MSG_DEBUG, "buf %s", buf);
	}
	else if ( os_strncasecmp(cmd, "scan-channels", 13) == 0 )
	{
		int noOfChan;

		noOfChan = atoi(cmd + 13);
		wpa_printf(MSG_DEBUG,"Scan Channels command = %d", noOfChan);
		if ( (noOfChan > 0) && (noOfChan <= MAX_NUMBER_OF_CHANNELS_PER_SCAN) )
			drv->scan_channels = noOfChan;
		ret = sprintf(buf,"Scan-Channels = %d\n", drv->scan_channels);
		wpa_printf(MSG_DEBUG, "buf %s", buf);
	}
	else if( os_strcasecmp(cmd, "rssi") == 0 || os_strcasecmp(cmd, "rssi-approx") == 0 )
	{
		u8 ssid[MAX_SSID_LEN];
		int rssi_data, rssi_beacon, len;

		wpa_printf(MSG_DEBUG,"rssi command");

		ret = wpa_driver_tista_get_rssi(priv, &rssi_data, &rssi_beacon);
		if ( ret == 0 )
		{
			len = wpa_driver_tista_get_ssid( priv, (u8 *)ssid );
			wpa_printf(MSG_DEBUG,"rssi_data %d rssi_beacon %d", rssi_data, rssi_beacon);
			if ( (len > 0) && (len <= MAX_SSID_LEN) )
			{
				os_memcpy( (void *)buf, (void *)ssid, len );
				ret = len;
				ret += sprintf(&buf[ret], " rssi %d\n", rssi_beacon);
				wpa_printf(MSG_DEBUG, "buf %s", buf);
			}
			else
			{
				wpa_printf(MSG_DEBUG, "Fail to get ssid when reporting rssi");
				ret = -1;
			}
		}
	}
	else if ( os_strncasecmp(cmd, "powermode", 9) == 0 )
	{
		u32 mode;
		TPowerMgr_PowerMode tMode;

		mode = (u32)atoi(cmd + 9);
		wpa_printf(MSG_DEBUG,"Power Mode command = %u", mode);
		if ( mode < POWER_MODE_MAX )
		{
			tMode.PowerMode = (PowerMgr_PowerMode_e)mode;
			tMode.PowerMngPriority = POWER_MANAGER_USER_PRIORITY;
			ret = wpa_driver_tista_config_power_management( priv, &tMode, 1 );
		}
	}
	else if (os_strncasecmp(cmd, "getpower", 8) == 0 )
	{
		u32 mode;
		TPowerMgr_PowerMode tMode;
		os_memset(&tMode, 0, sizeof(TPowerMgr_PowerMode));

		ret = wpa_driver_tista_config_power_management( priv, &tMode, 0 );
		if ( ret == 0 )
		{
			ret = sprintf(buf, "powermode = %u\n", tMode.PowerMode);
			wpa_printf(MSG_DEBUG, "buf %s", buf);
		}
	}
	else if ( os_strncasecmp(cmd, "btcoexmode", 10) == 0 )
	{
		u32 mode;

		mode = (u32)atoi(cmd + 10);
		wpa_printf(MSG_DEBUG,"BtCoex Mode command = %u", mode);
		ret = wpa_driver_tista_enable_bt_coe( priv, mode );
		if ( ret == 0 )
		{
			drv->btcoex_mode = mode;
		}
	}
	else if ( os_strcasecmp(cmd, "btcoexstat") == 0 )
	{
		u32 status = drv->btcoex_mode;

		wpa_printf(MSG_DEBUG,"BtCoex Status");
		ret = wpa_driver_tista_get_bt_coe_status( priv, &status );
		if ( ret == 0 )
		{
			ret = sprintf(buf, "btcoexstatus = 0x%x\n", status);
			wpa_printf(MSG_DEBUG, "buf %s", buf);
		}
	}
	else if ( os_strcasecmp(cmd, "rxfilter-start") == 0 )
	{
		wpa_printf(MSG_DEBUG,"Rx Data Filter Start command");
		ret = wpa_driver_tista_driver_enable_rx_data_filter( priv );
	}
	else if ( os_strcasecmp(cmd, "rxfilter-stop") == 0 )
	{
		wpa_printf(MSG_DEBUG,"Rx Data Filter Stop command");
		ret = wpa_driver_tista_driver_disable_rx_data_filter( priv );
	}
	else if ( os_strcasecmp(cmd, "rxfilter-statistics") == 0 )
	{
		TCuCommon_RxDataFilteringStatistics stats;
		int len, i;

		os_memset(&stats, 0, sizeof(TCuCommon_RxDataFilteringStatistics));
		wpa_printf(MSG_DEBUG,"Rx Data Filter Statistics command");
		ret = wpa_driver_tista_driver_rx_data_filter_statistics( priv, &stats );
		if ( ret == 0 )
		{
			ret = snprintf(buf, buf_len, "RxFilterStat: %u", (u32)stats.unmatchedPacketsCount);
			for (i=0;( i < MAX_DATA_FILTERS );i++)
			{
				ret += snprintf(&buf[ret], buf_len-ret, " %u", (u32)stats.matchedPacketsCount[i]);
			}
			ret += snprintf(&buf[ret], buf_len-ret, "\n");
			if (ret >= (int)buf_len)
			{
				ret = -1;
			}
		}
	}
	else if ( os_strncasecmp(cmd, "rxfilter-add", 12) == 0 )
	{
		TRxDataFilterRequest dfreq;
		char *cp = cmd + 12;
		char *endp;
		int type;

		if (*cp != '\0')
		{
			type = (int)strtol(cp, &endp, 0);
			if (endp != cp)
			{
				wpa_printf(MSG_DEBUG,"Rx Data Filter Add [%d] command", type);
				ret = prepare_filter_struct( priv, type, &dfreq );
				if ( ret == 0 )
				{
					ret = wpa_driver_tista_driver_rx_data_filter( priv, &dfreq, 1 );
				}
			}
		}
	}
	else if ( os_strncasecmp(cmd, "rxfilter-remove",15) == 0 )
	{
		TRxDataFilterRequest dfreq;
		char *cp = cmd + 15;
		char *endp;
		int type;

		if (*cp != '\0')
		{
			type = (int)strtol(cp, &endp, 0);
			if (endp != cp)
			{
				wpa_printf(MSG_DEBUG,"Rx Data Filter remove [%d] command", type);
				ret = prepare_filter_struct( priv, type, &dfreq );
				if ( ret == 0 )
				{
					ret = wpa_driver_tista_driver_rx_data_filter( priv, &dfreq, 0 );
				}
			}
		}
	}
	else
	{
		wpa_printf(MSG_DEBUG,"Unsupported command");
	}
	return ret;
}

/**
 * wpa_driver_tista_init - Initialize WE driver interface
 * @ctx: context to be used when calling wpa_supplicant functions,
 * e.g., wpa_supplicant_event()
 * @ifname: interface name, e.g., wlan0
 * Returns: Pointer to private data, %NULL on failure
 */
void * wpa_driver_tista_init(void *ctx, const char *ifname)
{
	struct wpa_driver_ti_data *drv;

	drv = os_zalloc(sizeof(*drv));
	if (drv == NULL)
		return NULL;
	drv->wext = wpa_driver_wext_init(ctx, ifname);
	if (drv->wext == NULL)
	{
		os_free(drv);
		return NULL;
	}

	scan_results_received = 0;
	drv->ctx = ctx;
	os_strncpy(drv->ifname, ifname, sizeof(drv->ifname));
	drv->ioctl_sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (drv->ioctl_sock < 0)
	{
		perror("socket");
		wpa_driver_wext_deinit(drv->wext);
		os_free(drv);
		return NULL;
	}

	/* Signal that driver is not stopped */
	drv->driver_is_loaded = TRUE;

	/* Set default scan type */
	drv->scan_type = SCAN_TYPE_NORMAL_ACTIVE;
	drv->force_merge_flag = 0;
	scan_init(drv);

	/* Set default amount of channels */
	drv->scan_channels = check_and_get_build_channels();

	/* Link Speed will be set by the message from the driver */
	drv->link_speed = 0;

	/* BtCoex mode is read from tiwlan.ini file */
	drv->btcoex_mode = 0; /* SG_DISABLE */

	/* Number of sequential errors */
	drv->errors = 0;
	return drv;
}

/**
 * wpa_driver_tista_deinit - Deinitialize WE driver interface
 * @priv: Pointer to private wext data from wpa_driver_tista_init()
 *
 * Shut down driver interface and processing of driver events. Free
 * private data buffer if one was allocated in wpa_driver_tista_init().
 */
void wpa_driver_tista_deinit(void *priv)
{
	struct wpa_driver_ti_data *drv = priv;

	eloop_cancel_timeout(wpa_driver_wext_scan_timeout, drv, drv->ctx);
	wpa_driver_wext_deinit(drv->wext);
	close(drv->ioctl_sock);
	scan_exit(drv);
	os_free(drv);
}

static int wpa_driver_tista_set_auth_param(struct wpa_driver_ti_data *drv,
        int idx, u32 value)
{
	struct iwreq iwr;
	int ret = 0;

	os_memset(&iwr, 0, sizeof(iwr));
	os_strncpy(iwr.ifr_name, drv->ifname, IFNAMSIZ);
	iwr.u.param.flags = idx & IW_AUTH_INDEX;
	iwr.u.param.value = value;

	if (ioctl(drv->ioctl_sock, SIOCSIWAUTH, &iwr) < 0)
	{
		perror("ioctl[SIOCSIWAUTH]");
		fprintf(stderr, "WEXT auth param %d value 0x%x - ",
		        idx, value);
		ret = errno == EOPNOTSUPP ? -2 : -1;
	}

	return ret;
}

static int wpa_driver_tista_set_wpa(void *priv, int enabled)
{
	struct wpa_driver_ti_data *drv = priv;
	int ret;

	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	ret = wpa_driver_tista_set_auth_param(drv, IW_AUTH_WPA_ENABLED,
	                                      enabled);
	return ret;
}

static int wpa_driver_tista_set_auth_alg(void *priv, int auth_alg)
{
	struct wpa_driver_ti_data *drv = priv;
	int algs = 0, res;

	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	if (auth_alg & AUTH_ALG_OPEN_SYSTEM)
		algs |= IW_AUTH_ALG_OPEN_SYSTEM;
	if (auth_alg & AUTH_ALG_SHARED_KEY)
		algs |= IW_AUTH_ALG_SHARED_KEY;
	if (auth_alg & AUTH_ALG_LEAP)
		algs |= IW_AUTH_ALG_LEAP;
	if (algs == 0)
	{
		/* at least one algorithm should be set */
		algs = IW_AUTH_ALG_OPEN_SYSTEM;
	}

	res = wpa_driver_tista_set_auth_param(drv, IW_AUTH_80211_AUTH_ALG,
	                                      algs);

	return res;
}

static int wpa_driver_tista_set_countermeasures(void *priv, int enabled)
{
	struct wpa_driver_ti_data *drv = priv;
	int ret;

	wpa_printf(MSG_DEBUG, "%s", __FUNCTION__);
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	ret = wpa_driver_tista_set_auth_param(drv,
	                                      IW_AUTH_TKIP_COUNTERMEASURES,
	                                      enabled);
	return ret;
}

static int wpa_driver_tista_set_drop_unencrypted(void *priv,
        int enabled)
{
	struct wpa_driver_ti_data *drv = priv;
	int ret;

	wpa_printf(MSG_DEBUG, "%s", __FUNCTION__);
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	/* Dm: drv->use_crypt = enabled; */
	ret = wpa_driver_tista_set_auth_param(drv, IW_AUTH_DROP_UNENCRYPTED,
	                                      enabled);
	return ret;
}

static int wpa_driver_tista_pmksa(struct wpa_driver_ti_data *drv,
                                  u32 cmd, const u8 *bssid, const u8 *pmkid)
{
	struct iwreq iwr;
	struct iw_pmksa pmksa;
	int ret = 0;

	os_memset(&iwr, 0, sizeof(iwr));
	os_strncpy(iwr.ifr_name, drv->ifname, IFNAMSIZ);
	os_memset(&pmksa, 0, sizeof(pmksa));
	pmksa.cmd = cmd;
	pmksa.bssid.sa_family = ARPHRD_ETHER;
	if (bssid)
		os_memcpy(pmksa.bssid.sa_data, bssid, ETH_ALEN);
	if (pmkid)
	{
		os_memcpy(pmksa.pmkid, pmkid, IW_PMKID_LEN);
		wpa_printf(MSG_DEBUG, "pmkid %s", pmkid);
	}
	iwr.u.data.pointer = (caddr_t)&pmksa;
	iwr.u.data.length = sizeof(pmksa);

	if (ioctl(drv->ioctl_sock, SIOCSIWPMKSA, &iwr) < 0)
	{
		if (errno != EOPNOTSUPP)
			perror("ioctl[SIOCSIWPMKSA]");
		ret = -1;
	}
	return ret;
}

static int wpa_driver_tista_add_pmkid(void *priv, const u8 *bssid,
                                      const u8 *pmkid)
{
	struct wpa_driver_ti_data *drv = priv;
	int ret;

	wpa_printf(MSG_DEBUG, "%s", __FUNCTION__);
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	ret = wpa_driver_tista_pmksa(drv, IW_PMKSA_ADD, bssid, pmkid);
	return ret;
}

static int wpa_driver_tista_remove_pmkid(void *priv, const u8 *bssid,
        const u8 *pmkid)
{
	struct wpa_driver_ti_data *drv = priv;
	int ret;

	wpa_printf(MSG_DEBUG, "%s", __FUNCTION__);
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	ret = wpa_driver_tista_pmksa(drv, IW_PMKSA_REMOVE, bssid, pmkid);
	return ret;
}

static int wpa_driver_tista_flush_pmkid(void *priv)
{
	struct wpa_driver_ti_data *drv = priv;
	int ret;

	wpa_printf(MSG_DEBUG, "%s", __FUNCTION__);
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	ret = wpa_driver_tista_pmksa(drv, IW_PMKSA_FLUSH, NULL, NULL);
	return ret;
}

static int wpa_driver_tista_mlme(struct wpa_driver_ti_data *drv,
                                 const u8 *addr, int cmd, int reason_code)
{
	struct iwreq iwr;
	struct iw_mlme mlme;
	int ret = 0;

	os_memset(&iwr, 0, sizeof(iwr));
	os_strncpy(iwr.ifr_name, drv->ifname, IFNAMSIZ);
	os_memset(&mlme, 0, sizeof(mlme));
	mlme.cmd = cmd;
	mlme.reason_code = reason_code;
	mlme.addr.sa_family = ARPHRD_ETHER;
	os_memcpy(mlme.addr.sa_data, addr, ETH_ALEN);
	iwr.u.data.pointer = (caddr_t) &mlme;
	iwr.u.data.length = sizeof(mlme);

	if (ioctl(drv->ioctl_sock, SIOCSIWMLME, &iwr) < 0)
	{
		perror("ioctl[SIOCSIWMLME]");
		ret = -1;
	}

	return ret;
}

static int wpa_driver_tista_deauthenticate(void *priv, const u8 *addr,
        int reason_code)
{
	struct wpa_driver_ti_data *drv = priv;
	int ret;

	wpa_printf(MSG_DEBUG, "%s", __FUNCTION__);
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	ret = wpa_driver_tista_mlme(drv, addr, IW_MLME_DEAUTH, reason_code);
	return ret;
}


static int wpa_driver_tista_disassociate(void *priv, const u8 *addr,
        int reason_code)
{
	struct wpa_driver_ti_data *drv = priv;
	int ret;

	wpa_printf(MSG_DEBUG, "%s", __FUNCTION__);
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	ret = wpa_driver_tista_mlme(drv, addr, IW_MLME_DISASSOC, reason_code);
	saved_ap_remove(drv, addr);
	return ret;
}

static int wpa_driver_tista_set_key(void *priv, wpa_alg alg,
                                    const u8 *addr, int key_idx,
                                    int set_tx, const u8 *seq, size_t seq_len,
                                    const u8 *key, size_t key_len)
{
	struct wpa_driver_ti_data *drv = priv;
	int ret;

	wpa_printf(MSG_DEBUG, "%s", __func__);
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	ret = wpa_driver_wext_set_key(drv->wext, alg, addr, key_idx, set_tx,
	                              seq, seq_len, key, key_len);
	return ret;
}

/*-----------------------------------------------------------------------------
Compare function for sorting scan results. Return >0 if @b is considered better.
-----------------------------------------------------------------------------*/
static int wpa_driver_tista_scan_result_compare(const void *a, const void *b)
{
	const struct wpa_scan_result *wa = a;
	const struct wpa_scan_result *wb = b;

	return( wb->level - wa->level );
}

static int wpa_driver_tista_get_scan_results(void *priv,
        struct wpa_scan_result *results,
        size_t max_size)
{
	struct wpa_driver_ti_data *drv = priv;
	int ap_num = 0;

	if (2 > scan_results_received)
	{
		scan_results_received++;
	}
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	ap_num = wpa_driver_wext_get_scan_results(drv->wext, results, max_size);
	wpa_printf(MSG_DEBUG, "Actual APs number %d", ap_num);

	if (ap_num < 0)
		return -1;

	/* Merge new results with previous */
	ap_num = scan_merge( drv, results, drv->force_merge_flag, ap_num, max_size );
	wpa_printf(MSG_DEBUG, "After merge, APs number %d", ap_num);
	qsort( results, ap_num, sizeof(struct wpa_scan_result),
	       wpa_driver_tista_scan_result_compare );
	return ap_num;
}

static int wpa_driver_tista_associate(void *priv,
                                      struct wpa_driver_associate_params *params)
{
	struct wpa_driver_ti_data *drv = priv;
	int allow_unencrypted_eapol;
	int value, flags;
	int ret;

	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	if (wpa_driver_wext_get_ifflags(drv->wext, &flags) == 0)
	{
		if (!(flags & IFF_UP))
		{
			wpa_driver_wext_set_ifflags(drv->wext, flags | IFF_UP);
		}
	}
	/* Set driver network mode (Adhoc/Infrastructure) according to supplied parameters */
	wpa_driver_wext_set_mode(drv->wext, params->mode);

#ifdef TI_WAPI
	if (params->wpa_ie_len)
	{
		if (wpa_driver_wext_set_gen_ie(drv->wext, params->wpa_ie, params->wpa_ie_len) < 0)
			return -1;
	}
#endif

	if (params->wpa_ie == NULL || params->wpa_ie_len == 0)
		value = IW_AUTH_WPA_VERSION_DISABLED;
	else if (params->wpa_ie[0] == RSN_INFO_ELEM)
		value = IW_AUTH_WPA_VERSION_WPA2;
	else
		value = IW_AUTH_WPA_VERSION_WPA;
	wpa_driver_tista_set_auth_param(drv, IW_AUTH_WPA_VERSION, value);
	value = wpa_driver_tista_cipher2wext(params->pairwise_suite);
	wpa_driver_tista_set_auth_param(drv, IW_AUTH_CIPHER_PAIRWISE, value);
	value = wpa_driver_tista_cipher2wext(params->group_suite);
	wpa_driver_tista_set_auth_param(drv, IW_AUTH_CIPHER_GROUP, value);
	value = wpa_driver_tista_keymgmt2wext(params->key_mgmt_suite);
	wpa_driver_tista_set_auth_param(drv, IW_AUTH_KEY_MGMT, value);
	value = params->key_mgmt_suite != KEY_MGMT_NONE ||
	        params->pairwise_suite != CIPHER_NONE ||
	        params->group_suite != CIPHER_NONE ||
	        params->wpa_ie_len;
	wpa_driver_tista_set_auth_param(drv, IW_AUTH_PRIVACY_INVOKED, value);

	/* Allow unencrypted EAPOL messages even if pairwise keys are set when
	 * not using WPA. IEEE 802.1X specifies that these frames are not
	 * encrypted, but WPA encrypts them when pairwise keys are in use. */
	if (params->key_mgmt_suite == KEY_MGMT_802_1X ||
	        params->key_mgmt_suite == KEY_MGMT_PSK)
		allow_unencrypted_eapol = 0;
	else
		allow_unencrypted_eapol = 1;

	wpa_driver_tista_set_auth_param(drv,
	                                IW_AUTH_RX_UNENCRYPTED_EAPOL,
	                                allow_unencrypted_eapol);

	if ( params->bssid )
	{
		wpa_printf(MSG_DEBUG, "wpa_driver_tista_associate: BSSID=" MACSTR,
		           MAC2STR(params->bssid));
		/* if there is bssid -> set it */
		if ( os_memcmp( params->bssid, "\x00\x00\x00\x00\x00\x00", ETH_ALEN ) != 0 )
		{
			wpa_driver_wext_set_bssid( drv->wext, params->bssid );
		}
	}

	/* This is a work-around for a race-condition between the wpa_supplicant
	 * and the framework; both entities will try to do scan and the second
	 * one will actually trigger an associate request (code in
	 * wpa_supplicant.c). If the associate request is received by the driver
	 * before the AP is actually found with scan, there will be a timeout in
	 * the wpa_supplicant after 60 s. This workaround will detect when the
	 * first real EVENT_SCAN_RESULTS is received and otherwise return -1,
	 * resulting in a timeout of only 5 s before it can try to associate
	 * again.
	 * We must compare with 2 since the first EVENT_SCAN_RESULTS is a fake
	 * one generated from the wpa_supplicant at start-up.
	 */
	if (2 > scan_results_received)
		ret = -1;
	else
		ret = wpa_driver_wext_set_ssid(drv->wext, params->ssid, params->ssid_len);
	return ret;
}

static int wpa_driver_tista_set_operstate(void *priv, int state)
{
	struct wpa_driver_ti_data *drv = priv;

	wpa_printf(MSG_DEBUG, "%s: operstate %d (%s)",
	           __func__, /*drv->operstate,*/ state, state ? "UP" : "DORMANT");
	TI_CHECK_DRIVER( drv->driver_is_loaded, -1 );
	/* Dm: drv->operstate = state; */
	return wpa_driver_wext_set_operstate(drv->wext, state);
}

#ifdef TI_WAPI
static int wpa_driver_tista_set_port_state(void *priv, const int port_state)
{
	u32 state = port_state;
	return wpa_driver_tista_private_send(
	           priv, RSN_PORT_STATUS_PARAM, &state, sizeof(state),
	           NULL, 0);
}

static int wpa_driver_tista_set_generic_ethertype(void *priv, const int ethertype)
{
	u16 generic_ethertye = ethertype;
	return wpa_driver_tista_private_send(
	           priv, TX_CTRL_GENERIC_ETHERTYPE, &generic_ethertye,
	           sizeof(generic_ethertye), NULL, 0);
}

static int wpa_driver_tista_set_external_mode(void *priv, const int mode)
{
	u32 ext_mode = mode;
	return wpa_driver_tista_private_send(
	           priv, RSN_EXTERNAL_MODE_PARAM, &ext_mode, sizeof(ext_mode),
	           NULL, 0);
}
#endif /* TI_WAPI */

#ifdef CONFIG_WPS
static int wpa_driver_tista_set_wsc_mode(void *priv, const u32 WscMode, const void* probeReqBuf, int probeReqBufLen)
{
	struct wpa_driver_ti_data *drv = priv;
	struct iwreq iwr;
	ti_private_cmd_t private_cmd;
	TWscMode WcsModeStruct;
	int ret = 0;

	WcsModeStruct.WSCMode = WscMode;
	WcsModeStruct.uWscIeSize = probeReqBufLen;
	memset(WcsModeStruct.probeReqWSCIE, 0, DOT11_WSC_PROBE_REQ_MAX_LENGTH);
	memcpy(WcsModeStruct.probeReqWSCIE, probeReqBuf, probeReqBufLen);

	private_cmd.cmd = SITE_MGR_SIMPLE_CONFIG_MODE;
	private_cmd.flags = PRIVATE_CMD_SET_FLAG;
	private_cmd.in_buffer = &WcsModeStruct;
	private_cmd.in_buffer_len = sizeof(TWscMode);
	private_cmd.out_buffer = NULL;
	private_cmd.out_buffer_len = 0;

	os_memset(&iwr, 0, sizeof(iwr));
	os_strncpy(iwr.ifr_name, drv->ifname, IFNAMSIZ);

	iwr.u.data.pointer = &private_cmd;
	iwr.u.data.length = sizeof(ti_private_cmd_t);
	iwr.u.data.flags = 0;

	if (ioctl(drv->ioctl_sock, SIOCIWFIRSTPRIV, &iwr) < 0)
	{
		perror("ioctl[SIOCIWFIRSTPRIV]");
		ret = -1;
	}

	return ret;
}
#endif /* CONFIG_WPS */

const struct wpa_driver_ops wpa_driver_custom_ops =
{
	.name = TIWLAN_DRV_NAME,
	.desc = "TI Station Driver (1271)",
	.get_bssid = wpa_driver_tista_get_bssid,
	.get_ssid = wpa_driver_tista_get_ssid,
	.set_wpa = wpa_driver_tista_set_wpa,
	.set_key = wpa_driver_tista_set_key,
	.set_countermeasures = wpa_driver_tista_set_countermeasures,
	.set_drop_unencrypted = wpa_driver_tista_set_drop_unencrypted,
	.scan = wpa_driver_tista_scan,
	.get_scan_results = wpa_driver_tista_get_scan_results,
	.deauthenticate = wpa_driver_tista_deauthenticate,
	.disassociate = wpa_driver_tista_disassociate,
	.associate = wpa_driver_tista_associate,
	.set_auth_alg = wpa_driver_tista_set_auth_alg,
	.get_mac_addr = wpa_driver_tista_get_mac_addr,
	.init = wpa_driver_tista_init,
	.deinit = wpa_driver_tista_deinit,
	.add_pmkid = wpa_driver_tista_add_pmkid,
	.remove_pmkid = wpa_driver_tista_remove_pmkid,
	.flush_pmkid = wpa_driver_tista_flush_pmkid,
	.set_operstate = wpa_driver_tista_set_operstate,
#ifdef TI_WAPI
	.set_port_state = wpa_driver_tista_set_port_state,
	.set_generic_ethertype = wpa_driver_tista_set_generic_ethertype,
	.set_external_mode = wpa_driver_tista_set_external_mode,
#endif
#ifdef CONFIG_WPS
	.set_wsc_mode = wpa_driver_tista_set_wsc_mode,
#endif /* CONFIG_WPS */
	.driver_cmd = wpa_driver_tista_driver_cmd
};
