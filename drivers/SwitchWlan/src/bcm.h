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

/* 0.6.0: connect (stage 4c) */
enum { BCM_LINK_IDLE = 0, BCM_LINK_JOINING = 1, BCM_LINK_UP = 2, BCM_LINK_FAILED = 3, BCM_LINK_LOST = 4 };

typedef struct _BCM_JOIN {
    UCHAR Ssid[32];
    ULONG SsidLen;
    UCHAR Bssid[6];
    UCHAR Channel;
    ULONG Wsec;             /* 0 open, 2 TKIP, 4 AES (brcmf wsec bits) */
    ULONG WpaAuth;          /* 0 disabled, 0x04 WPA-PSK, 0x80 WPA2-PSK */
} BCM_JOIN;

typedef struct _BCM_KEY {
    ULONG Index;
    ULONG Len;              /* 0 = delete */
    UCHAR Data[32];
    BOOLEAN Pairwise;
    UCHAR Peer[6];
    BOOLEAN HaveRsc;
    UCHAR Rsc[6];           /* receive PN, little-endian */
} BCM_KEY;

NTSTATUS BcmJoin(const BCM_JOIN *Join);          /* security + join; the result comes from BcmPoll */
ULONG BcmLinkState(VOID);                       /* BCM_LINK_* */
VOID BcmLinkResult(PULONG Event, PULONG Status, PULONG Reason);
VOID BcmGetAssocIes(PUCHAR Req, ULONG ReqMax, PULONG ReqLen, PUCHAR Resp, ULONG RespMax, PULONG RespLen);
LONG BcmGetRssi(VOID);
VOID BcmLeave(VOID);                            /* disassociate, data path down */
NTSTATUS BcmSetMac(const UCHAR Mac[6]);
NTSTATUS BcmSetKey(const BCM_KEY *Key);         /* waits for queued EAPOL frames to leave first */

/* 0.6.0: data path (Ethernet frames) */
typedef VOID BCM_RX_HANDLER(PVOID Context, const UCHAR *Frame, ULONG Len);
VOID BcmSetRxHandler(BCM_RX_HANDLER *Handler, PVOID Context);   /* called from BcmPoll */
NTSTATUS BcmDataStart(const UCHAR Peer[6]);     /* create the tx flowring */
VOID BcmDataStop(VOID);
BOOLEAN BcmDataReady(VOID);
BOOLEAN BcmTxFrame(const UCHAR *Frame, ULONG Len, UCHAR Priority);  /* copies; FALSE = no room */
VOID BcmTxCommit(VOID);
VOID BcmSaveTrace(VOID);

#ifdef __cplusplus
}
#endif
