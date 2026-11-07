/*
 * bcm.h - BCM4356 (PCIe, msgbuf) chip layer of switchwlan.
 *
 * One device only. All calls except BcmSetRegistryPath must run at
 * PASSIVE_LEVEL from one thread at a time (PrepareHardware, then the
 * switchwlan worker thread, then ReleaseHardware).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define BCM_IE_MAX          768
#define BCM_SCAN_MAX        48

typedef struct _BCM_BSS {
    UCHAR Bssid[6];
    UCHAR SsidLen;
    UCHAR Ssid[32];
    UCHAR Channel;          /* control channel */
    UCHAR Band;             /* 1 = 2.4 GHz, 2 = 5 GHz (WDI_BAND_ID) */
    USHORT Chanspec;
    SHORT Rssi;             /* dBm */
    USHORT BeaconPeriod;
    USHORT Capability;
    USHORT IeLen;
    UCHAR Ie[BCM_IE_MAX];   /* information elements of the beacon/probe response */
} BCM_BSS;

VOID BcmSetRegistryPath(PCUNICODE_STRING RegistryPath);
NTSTATUS BcmPrepareHardware(WDFCMRESLIST Translated);
VOID BcmReleaseHardware(VOID);

BOOLEAN BcmIsReady(VOID);                       /* firmware up, ioctls work, radio up */
VOID BcmGetMac(UCHAR Mac[6]);
VOID BcmGetFirmwareVersion(CHAR *Text, ULONG Size);
VOID BcmTrace(PCWSTR Name, ULONG Value);        /* Parameters\<Name> = Value */

NTSTATUS BcmScanBegin(VOID);                    /* escan start, results collected by BcmPoll */
VOID BcmPoll(VOID);                             /* drain the control completion ring */
BOOLEAN BcmScanFinished(VOID);
ULONG BcmScanCount(VOID);
const BCM_BSS *BcmScanEntry(ULONG Index);
VOID BcmScanSaveList(VOID);                     /* Parameters\ScanList, for Wifi.ps1 */

#ifdef __cplusplus
}
#endif
