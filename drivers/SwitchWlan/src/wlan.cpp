/*
 * switchwlan: Nintendo Switch Wi-Fi (BCM4356 on Tegra210 PCIe) as a WiFiCx
 * client driver (Windows 11 22H2+: WiFiCx 1.0, NetAdapterCx 2.2, KMDF 1.33).
 *
 * Stage 4d: Windows' own Wi-Fi list, open-network association, and bounded
 * copy-based Ethernet path. The control path follows the WDK WiFiCx
 * sample (network/wlan/wificx): capabilities are pushed with
 * WifiDeviceSet*Capabilities in EvtDevicePrepareHardware, commands arrive as
 * WDI messages in EvtWifiDeviceSendCommand, tasks are completed twice (M3:
 * WifiRequestComplete, M4: a *_COMPLETE indication with the request's
 * TransactionId), and scan results go up as BSS_ENTRY_LIST indications.
 *
 * The chip work (bcm.c) sleeps, so it runs on one worker thread; the command
 * callback only records what to do. TLVs are written by hand (layout as the
 * sample's test data: UINT16 type, UINT16 length, little-endian, no padding).
 *
 * WPA2-PSK/CCMP key add/delete commands are parsed; handshake, data path, and
 * stability remain to be validated on the Switch.
 */
#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <net/virtualaddress.h>
#include <wificx.h>
#include "bcm.h"
#include <ntstrsafe.h>

#define WLAN_VERSION        0x00060C00u     /* 0.6.12 */
#define WLAN_TAG            'nlWS'
#define WLAN_FRAME_MAX      2048u
#define WLAN_FRAME_QUEUE    256u    /* 0.6.9: 32 frames per poll capped Rx at ~12 Mbit/s */

enum { WLAN_OP_NONE = 0, WLAN_OP_CONNECT = 1, WLAN_OP_DISCONNECT = 2, WLAN_OP_RESET = 3, WLAN_OP_CONNECTING = 4 };
#define WLAN_KEY_MAX 4u

typedef struct _WLAN_FRAME {
    ULONG Length;
    UCHAR Bytes[WLAN_FRAME_MAX];
} WLAN_FRAME;

typedef struct _WLAN_QUEUE {
    struct _WLAN_DEVICE *Device;
    NET_EXTENSION VirtualAddress;
    BOOLEAN IsRx;
} WLAN_QUEUE, *PWLAN_QUEUE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WLAN_QUEUE, WlanGetQueue)

#define WLAN_LOG_MAX 32

typedef struct _WLAN_DEVICE {
    void *WdfTriageInfoPtr;                 /* must be first (NetAdapterCx) */
    WDFDEVICE Device;
    NETADAPTER Adapter;
    PKTHREAD Thread;
    KEVENT Wake;
    volatile LONG Stop;
    KSPIN_LOCK Lock;
    BOOLEAN ScanPending;
    WDI_MESSAGE_HEADER ScanHeader;          /* PortId, TransactionId, IhvSpecificId of the scan task */
    ULONG PendingOp;
    WDI_MESSAGE_HEADER OpHeader;
    BCM_JOIN Join;
    ULONG OpElapsedMs;
    WLAN_FRAME TxFrames[WLAN_FRAME_QUEUE];
    ULONG TxHead, TxTail, TxCount;
    NETPACKETQUEUE TxQueue;
    BOOLEAN TxNotifyEnabled;
    WLAN_FRAME RxFrames[WLAN_FRAME_QUEUE];
    ULONG RxHead, RxTail, RxCount;
    NETPACKETQUEUE RxQueue;
    BOOLEAN RxNotifyEnabled;
    BOOLEAN KeyPending;
    WIFIREQUEST KeyRequest;
    ULONG PendingKeyCount;
    BCM_KEY PendingKeys[WLAN_KEY_MAX];
    ULONG Scans, Commands, Unsupported, LastUnsupported;
    /* 0.6.4 diagnostics: last WDI commands (id) and indications (0x8000 | id), written every second */
    volatile LONG LogN;
    USHORT Log[WLAN_LOG_MAX];
    ULONG Ticks;
    ULONGLONG TraceAt;
    /* 0.6.6 diagnostics: frames between the chip and Windows */
    ULONG RxQueued, RxNoQueue, RxFull, RxToOs, RxEapolToOs, RxAdvance;
    ULONG TxFromOs, TxEapolFromOs, TxDropNotReady, TxAdvance;
    ULONG ConnLinkMs, ConnDoneMs;
    ULONG RxDstMine, RxDstGroup, RxDstOther;
    ULONG TxL2Last, TxConverted, TxNot80211;
    /* 0.6.12: hold received frames from the join until CONNECT_COMPLETE is out,
     * so the AP's first EAPOL (M1) is not dropped by Windows; no scan meanwhile */
    volatile BOOLEAN RxHold;
    ULONG RxEapolHeld, ScanDeferred;
    UCHAR RxOtherOui[3];
} WLAN_DEVICE, *PWLAN_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WLAN_DEVICE, WlanGetDevice)

static VOID WlanLog(PWLAN_DEVICE Ctx, USHORT Entry)
{
    LONG N = InterlockedIncrement(&Ctx->LogN) - 1;
    Ctx->Log[(ULONG)N % WLAN_LOG_MAX] = Entry;
}

/* 0.6.4: written from the worker every second, so Status shows a live picture. */
static VOID WlanSaveTrace(PWLAN_DEVICE Ctx)
{
    static WCHAR Text[WLAN_LOG_MAX * 8];
    LONG N = Ctx->LogN;
    LONG First = (N > WLAN_LOG_MAX) ? N - WLAN_LOG_MAX : 0;
    LONG I;
    size_t Left = sizeof(Text);
    PWSTR Out = Text;
    Text[0] = L'\0';
    for (I = First; I < N; I++) {
        USHORT E = Ctx->Log[(ULONG)I % WLAN_LOG_MAX];
        if (E & 0x8000) (VOID)RtlStringCbPrintfExW(Out, Left, &Out, &Left, 0, L"i%u ", (ULONG)(E & 0x7FFF));
        else (VOID)RtlStringCbPrintfExW(Out, Left, &Out, &Left, 0, L"%u ", (ULONG)E);
    }
    BcmTraceText(L"WlanCmdLog", Text);
    BcmTrace(L"WlanCmdLogN", (ULONG)N);
    BcmTrace(L"WlanCommands", Ctx->Commands);
    BcmTrace(L"WlanUnsupported", Ctx->Unsupported);
    BcmTrace(L"WlanLastUnsupported", Ctx->LastUnsupported);
    BcmTrace(L"WlanPendingOp", Ctx->PendingOp);
    BcmTrace(L"WlanTicks", Ctx->Ticks);
    {
        static WCHAR Os[160];
        (VOID)RtlStringCbPrintfW(Os, sizeof(Os), L"rx queued=%u noQueue=%u full=%u toOs=%u eapolToOs=%u adv=%u | tx fromOs=%u eapolFromOs=%u dropNotReady=%u adv=%u | link %u ms, done %u ms",
            Ctx->RxQueued, Ctx->RxNoQueue, Ctx->RxFull, Ctx->RxToOs, Ctx->RxEapolToOs, Ctx->RxAdvance,
            Ctx->TxFromOs, Ctx->TxEapolFromOs, Ctx->TxDropNotReady, Ctx->TxAdvance, Ctx->ConnLinkMs, Ctx->ConnDoneMs);
        BcmTraceText(L"WlanOsPath", Os);
        (VOID)RtlStringCbPrintfW(Os, sizeof(Os), L"mine=%u group=%u other=%u (other OUI %02X:%02X:%02X)",
            Ctx->RxDstMine, Ctx->RxDstGroup, Ctx->RxDstOther, Ctx->RxOtherOui[0], Ctx->RxOtherOui[1], Ctx->RxOtherOui[2]);
        BcmTraceText(L"WlanRxDst", Os);
        (VOID)RtlStringCbPrintfW(Os, sizeof(Os), L"rx as 802.11 | tx l2type=%u converted=%u asIs=%u | eapolHeld=%u scanDeferred=%u",
            Ctx->TxL2Last, Ctx->TxConverted, Ctx->TxNot80211, Ctx->RxEapolHeld, Ctx->ScanDeferred);
        BcmTraceText(L"WlanFormat", Os);
    }
    BcmSaveTrace();
}

typedef struct _WLAN_ADAPTER {
    WDFDEVICE Device;
} WLAN_ADAPTER, *PWLAN_ADAPTER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WLAN_ADAPTER, WlanGetAdapter)

extern "C" DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
static EVT_WIFI_DEVICE_SEND_COMMAND EvtSendCommand;
static EVT_WIFI_DEVICE_CREATE_ADAPTER EvtCreateAdapter;
static EVT_WIFI_DEVICE_CREATE_WIFIDIRECTDEVICE EvtCreateWifiDirect;
static EVT_NET_ADAPTER_CREATE_TXQUEUE EvtCreateTxQueue;
static EVT_NET_ADAPTER_CREATE_RXQUEUE EvtCreateRxQueue;
static EVT_PACKET_QUEUE_ADVANCE EvtTxAdvance;
static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtTxNotify;
static EVT_PACKET_QUEUE_CANCEL EvtTxCancel;
static EVT_PACKET_QUEUE_ADVANCE EvtRxAdvance;
static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtRxNotify;
static EVT_PACKET_QUEUE_CANCEL EvtRxCancel;

static VOID WlanReceiveFrame(PVOID Context, const UCHAR *Frame, ULONG Length);
static VOID WlanDrainTx(PWLAN_DEVICE Ctx);
static VOID WlanNotifyTx(PWLAN_DEVICE Ctx);
static ULONG PutTlv(PUCHAR Out, ULONG Off, USHORT Type, const VOID *Data, ULONG Len);

/* ------------------------------------------------------------- indications */

static VOID WlanIndicate(PWLAN_DEVICE Ctx, UINT16 MessageId, const WDI_MESSAGE_HEADER *Request, UINT32 TransactionId,
                         NDIS_STATUS Status, const UCHAR *Tlv, ULONG TlvLen)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFMEMORY Memory;
    PUCHAR Buf;
    PWDI_MESSAGE_HEADER Header;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Ctx->Device;
    if (!NT_SUCCESS(WdfMemoryCreate(&Attributes, NonPagedPoolNx, WLAN_TAG, sizeof(WDI_MESSAGE_HEADER) + TlvLen, &Memory, (PVOID *)&Buf))) {
        return;
    }
    RtlZeroMemory(Buf, sizeof(WDI_MESSAGE_HEADER) + TlvLen);
    Header = (PWDI_MESSAGE_HEADER)Buf;
    Header->PortId = Request->PortId;
    Header->Status = Status;
    Header->TransactionId = TransactionId;
    Header->IhvSpecificId = Request->IhvSpecificId;
    if (TlvLen) RtlCopyMemory(Buf + sizeof(WDI_MESSAGE_HEADER), Tlv, TlvLen);
    WlanLog(Ctx, (USHORT)(0x8000 | (MessageId & 0x7FFF)));
    WifiDeviceReceiveIndication(Ctx->Device, MessageId, Memory);
    WdfObjectDelete(Memory);
}

/* M4: the task's completion indication, same TransactionId as the request. */
static VOID WlanTaskComplete(PWLAN_DEVICE Ctx, UINT16 Indication, const WDI_MESSAGE_HEADER *Request, NDIS_STATUS Status)
{
    WlanIndicate(Ctx, Indication, Request, Request->TransactionId, Status, NULL, 0);
}

/* WDI requires a successful association result before CONNECT_COMPLETE.
 * BcmGetAssocIes returns the firmware's fixed association fields plus IEs,
 * without the 802.11 MAC header, as required by the WDI frame TLVs. Keep the
 * parameter layout in sync with the WDI 2.0.3 result container. */
static VOID WlanIndicateAssociationResult(PWLAN_DEVICE Ctx, const WDI_MESSAGE_HEADER *Request, const BCM_JOIN *Join)
{
    UCHAR Tlv[1200];
    UCHAR Params[48];
    UCHAR LinkTlv[67];
    UCHAR LinkParams[15];
    UCHAR LinkInfo[40];
    UCHAR LocalMac[6];
    UCHAR ReqIes[512], RespIes[512];
    ULONG ReqLen = 0, RespLen = 0, Off = 4;
    ULONG Value;
    ULONG Band;
    LONG Rssi;
    ULONG Quality;
    ULONG LinkSpeed;
    ULONG Channel;

    RtlZeroMemory(Tlv, sizeof(Tlv));
    RtlZeroMemory(Params, sizeof(Params));
    RtlZeroMemory(ReqIes, sizeof(ReqIes));
    RtlZeroMemory(RespIes, sizeof(RespIes));

    /* Fixed 48-byte WDI v1.1.11+ result parameter layout (little-endian). */
    Value = WDI_ASSOC_STATUS_SUCCESS;
    RtlCopyMemory(Params, &Value, sizeof(Value));
    Value = 0; /* peer status code */
    RtlCopyMemory(Params + 4, &Value, sizeof(Value));
    Params[8] = 0; /* not a reassociation */
    Value = (Join->WpaAuth == 0x80) ? WDI_AUTH_ALGO_RSNA_PSK : WDI_AUTH_ALGO_80211_OPEN;
    RtlCopyMemory(Params + 9, &Value, sizeof(Value));
    Value = (Join->Wsec & 4) ? WDI_CIPHER_ALGO_CCMP : WDI_CIPHER_ALGO_NONE;
    RtlCopyMemory(Params + 13, &Value, sizeof(Value));
    RtlCopyMemory(Params + 17, &Value, sizeof(Value));
    /* Multicast management cipher remains NONE (offset 21). */
    Params[25] = 0; /* four-address mode */
    Params[26] = 0; /* WPA2 port is not authorized until key exchange */
    Params[27] = 0; /* WMM not reported by firmware */
    Value = WDI_DS_UNKNOWN;
    RtlCopyMemory(Params + 28, &Value, sizeof(Value));
    Value = 0; /* association comeback time */
    RtlCopyMemory(Params + 32, &Value, sizeof(Value));
    Band = (Join->Channel <= 14) ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
    RtlCopyMemory(Params + 36, &Band, sizeof(Band));
    /* IHV status and DisableDataPathOffloadsScenario are zero. */

    Off = PutTlv(Tlv, Off, WDI_TLV_BSSID, Join->Bssid, sizeof(Join->Bssid));
    Off = PutTlv(Tlv, Off, WDI_TLV_ASSOCIATION_RESULT_PARAMETERS, Params, sizeof(Params));
    BcmGetAssocIes(ReqIes, sizeof(ReqIes), &ReqLen, RespIes, sizeof(RespIes), &RespLen);
    if (ReqLen) Off = PutTlv(Tlv, Off, WDI_TLV_ASSOCIATION_REQUEST_FRAME, ReqIes, ReqLen);
    if (RespLen) Off = PutTlv(Tlv, Off, WDI_TLV_ASSOCIATION_RESPONSE_FRAME, RespIes, RespLen);

    /* WDI v1.1.4+ requires the active PHY list. 0.6.11: from the AP's
     * association response IEs (VHT capabilities 191, HT capabilities 45). */
    {
        BOOLEAN Ht = FALSE, Vht = FALSE;
        ULONG I = 6;                            /* capability, status, AID */
        while (RespLen >= 6 && I + 2 <= RespLen && I + 2 + RespIes[I + 1] <= RespLen) {
            if (RespIes[I] == 45) Ht = TRUE;
            if (RespIes[I] == 191) Vht = TRUE;
            I += 2 + RespIes[I + 1];
        }
        if (Join->Channel > 14) Value = Vht ? WDI_PHY_TYPE_VHT : Ht ? WDI_PHY_TYPE_HT : WDI_PHY_TYPE_OFDM;
        else Value = Ht ? WDI_PHY_TYPE_HT : WDI_PHY_TYPE_ERP;
        BcmTrace(L"WlanPhy", Value);
    }
    Off = PutTlv(Tlv, Off, WDI_TLV_PHY_TYPE_LIST, &Value, sizeof(Value));
    (VOID)PutTlv(Tlv, 0, WDI_TLV_ASSOCIATION_RESULT, NULL, Off - 4);

    Rssi = BcmGetRssi();
    Quality = (Rssi == 0) ? 0 : (Rssi >= -50) ? 100 : (Rssi <= -100) ? 0 : (ULONG)(2 * (Rssi + 100));
    BcmTrace(L"WlanAssocReqIes", ReqLen);
    BcmTrace(L"WlanAssocRespIes", RespLen);
    BcmTrace(L"WlanAssocRssi", (ULONG)Rssi);
    BcmTrace(L"WlanAssocQuality", Quality);
    WlanIndicate(Ctx, WDI_INDICATION_ASSOCIATION_RESULT, Request, 0, NDIS_STATUS_SUCCESS, Tlv, Off);

    /* Match the WDI sample sequence: association result, link state, then
     * CONNECT_COMPLETE. Report the firmware's current legacy-rate estimate. */
    RtlZeroMemory(LinkTlv, sizeof(LinkTlv));
    RtlZeroMemory(LinkParams, sizeof(LinkParams));
    RtlZeroMemory(LinkInfo, sizeof(LinkInfo));
    LinkSpeed = BcmGetLinkSpeed();
    BcmGetMac(LocalMac);
    Channel = Join->Channel;
    Band = (Channel <= 14) ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
    RtlCopyMemory(LinkParams, Join->Bssid, sizeof(Join->Bssid));
    RtlCopyMemory(LinkParams + 6, &LinkSpeed, sizeof(LinkSpeed));
    RtlCopyMemory(LinkParams + 10, &LinkSpeed, sizeof(LinkSpeed));
    LinkParams[14] = (UCHAR)Quality;
    Off = PutTlv(LinkTlv, 0, WDI_TLV_LINK_STATE_CHANGE_PARAMETERS, LinkParams, sizeof(LinkParams));
    /* WDI 2.0.3 requires one LinkInfo item; fields are serialized without C padding. */
    RtlCopyMemory(LinkInfo + 8, LocalMac, sizeof(LocalMac));
    RtlCopyMemory(LinkInfo + 14, Join->Bssid, sizeof(Join->Bssid));
    RtlCopyMemory(LinkInfo + 20, &Channel, sizeof(Channel));
    RtlCopyMemory(LinkInfo + 24, &Band, sizeof(Band));
    RtlCopyMemory(LinkInfo + 28, &Rssi, sizeof(Rssi));
    Value = BcmGetBandwidth();              /* 0.6.11: was a fixed 20 MHz */
    BcmTrace(L"WlanBandwidth", Value);
    RtlCopyMemory(LinkInfo + 32, &Value, sizeof(Value));
    Off = PutTlv(LinkTlv, Off, WDI_TLV_LINK_INFO, LinkInfo, sizeof(LinkInfo));
    BcmTrace(L"WlanLinkSpeed", LinkSpeed);
    WlanIndicate(Ctx, WDI_INDICATION_LINK_STATE_CHANGE, Request, 0, NDIS_STATUS_SUCCESS, LinkTlv, Off);
}

static ULONG PutTlv(PUCHAR Out, ULONG Off, USHORT Type, const VOID *Data, ULONG Len)
{
    RtlCopyMemory(Out + Off, &Type, 2);
    USHORT L16 = (USHORT)Len;
    RtlCopyMemory(Out + Off + 2, &L16, 2);
    if (Len && Data) RtlCopyMemory(Out + Off + 4, Data, Len);
    return Off + 4 + Len;
}

/* WDI uses nested (type,length,value) TLVs. Validate every boundary before
 * examining a request because these buffers originate in the OS/firmware path. */
static BOOLEAN WlanFindTlv(const UCHAR *Buf, ULONG Len, USHORT Type, const UCHAR **Value, ULONG *ValueLen)
{
    ULONG Off = 0;
    while (Off <= Len && Len - Off >= 4) {
        USHORT T, L;
        RtlCopyMemory(&T, Buf + Off, 2);
        RtlCopyMemory(&L, Buf + Off + 2, 2);
        Off += 4;
        if ((ULONG)L > Len - Off) return FALSE;
        if (T == Type) { *Value = Buf + Off; *ValueLen = L; return TRUE; }
        Off += L;
    }
    return FALSE;
}

static BOOLEAN WlanParseConnect(const UCHAR *Buf, ULONG Len, BCM_JOIN *Join)
{
    const UCHAR *Top, *V, *List;
    ULONG TopLen, VLen, ListLen;
    ULONG Off;
    BOOLEAN HaveSsid = FALSE, HaveBssid = FALSE;

    RtlZeroMemory(Join, sizeof(*Join));
    /* WPA2-PSK is enabled only for CCMP and the key/data path below. */
    Join->Wsec = 0;
    Join->WpaAuth = 0;
    if (!WlanFindTlv(Buf, Len, WDI_TLV_CONNECT_PARAMETERS, &Top, &TopLen)) return FALSE;
    if (WlanFindTlv(Top, TopLen, WDI_TLV_SSID, &V, &VLen) && VLen <= sizeof(Join->Ssid)) {
        Join->SsidLen = VLen;
        RtlCopyMemory(Join->Ssid, V, VLen);
        HaveSsid = VLen != 0;
    }
    if (!WlanFindTlv(Top, TopLen, WDI_TLV_AUTH_ALGO_LIST, &List, &ListLen) || ListLen < sizeof(UINT32)) return FALSE;
    {
        UINT32 Auth;
        RtlCopyMemory(&Auth, List, sizeof(Auth));
        if (Auth == WDI_AUTH_ALGO_80211_OPEN) {
            Join->Wsec = 0;
            Join->WpaAuth = 0;
        } else if (Auth == WDI_AUTH_ALGO_RSNA_PSK) {
            Join->Wsec = 4;                 /* BCM wsec AES */
            Join->WpaAuth = 0x80;            /* BCM WPA2-PSK */
        } else return FALSE;
    }
    /* Read the first preferred BSS entry (BSSID and channel); Windows already
     * selected it from the scan list. WDI_CHANNEL_NUMBER and WDI_BAND_ID are
     * both 32-bit values in the channel-info payload. */
    Off = 0;
    while (Off <= Len && Len - Off >= 4) {
        USHORT T, L;
        RtlCopyMemory(&T, Buf + Off, 2);
        RtlCopyMemory(&L, Buf + Off + 2, 2);
        Off += 4;
        if ((ULONG)L > Len - Off) return FALSE;
        if (T == WDI_TLV_CONNECT_BSS_ENTRY) {
            if (WlanFindTlv(Buf + Off, L, WDI_TLV_BSSID, &V, &VLen) && VLen == 6) {
                RtlCopyMemory(Join->Bssid, V, 6);
                HaveBssid = TRUE;
            }
            if (WlanFindTlv(Buf + Off, L, WDI_TLV_BSS_ENTRY_CHANNEL_INFO, &V, &VLen) && VLen >= 8) {
                UINT32 Channel;
                RtlCopyMemory(&Channel, V, sizeof(Channel));
                if (Channel > MAXUCHAR) return FALSE;
                Join->Channel = (UCHAR)Channel;
            }
            break;
        }
        Off += L;
    }
    return HaveSsid && HaveBssid && Join->Channel >= 1 && Join->Channel <= 196;
}

/* Parse only the RSN-PSK CCMP subset implemented by the BCM layer. WDI
 * aggregates are nested TLVs; CipherKeyTypeInfo is its fixed 13-byte struct. */
static BOOLEAN WlanParseCcmpKeys(const UCHAR *Buf, ULONG Len, BOOLEAN Delete, BCM_KEY Keys[WLAN_KEY_MAX], ULONG *Count)
{
    ULONG Off = 0, N = 0;
    while (Off <= Len && Len - Off >= 4) {
        USHORT T, L;
        const UCHAR *Peer = NULL, *Type = NULL, *Id = NULL, *Rsc = NULL, *Key = NULL;
        ULONG PeerLen = 0, TypeLen = 0, IdLen = 0, RscLen = 0, KeyLen = 0;
        RtlCopyMemory(&T, Buf + Off, 2);
        RtlCopyMemory(&L, Buf + Off + 2, 2);
        Off += 4;
        if ((ULONG)L > Len - Off) return FALSE;
        if (T == (Delete ? WDI_TLV_DELETE_CIPHER_KEY_INFO : WDI_TLV_SET_CIPHER_KEY_INFO)) {
            const UCHAR *Item = Buf + Off;
            ULONG ItemLen = L;
            if (N == WLAN_KEY_MAX ||
                !WlanFindTlv(Item, ItemLen, WDI_TLV_CIPHER_KEY_TYPE_INFO, &Type, &TypeLen) ||
                (TypeLen != 13 && TypeLen != 16))
                return FALSE;
            (VOID)WlanFindTlv(Item, ItemLen, WDI_TLV_PEER_MAC_ADDRESS, &Peer, &PeerLen);
            (VOID)WlanFindTlv(Item, ItemLen, WDI_TLV_CIPHER_KEY_ID, &Id, &IdLen);
            (VOID)WlanFindTlv(Item, ItemLen, WDI_TLV_CIPHER_KEY_RECEIVE_SEQUENCE_COUNT, &Rsc, &RscLen);
            (VOID)WlanFindTlv(Item, ItemLen, WDI_TLV_CIPHER_KEY_CCMP_KEY, &Key, &KeyLen);
            ULONG Algo, Direction, KeyType;
            RtlCopyMemory(&Algo, Type, 4);
            RtlCopyMemory(&Direction, Type + 4, 4);
            RtlCopyMemory(&KeyType, Type + ((TypeLen == 16) ? 12 : 9), 4);
            if (Algo != WDI_CIPHER_ALGO_CCMP || Direction != WDI_CIPHER_KEY_DIRECTION_BOTH ||
                (!Delete && (Key == NULL || KeyLen != 16)) || (Delete && Key != NULL) ||
                (Id != NULL && IdLen != 4) || (Rsc != NULL && RscLen != 6))
                return FALSE;
            RtlZeroMemory(&Keys[N], sizeof(Keys[N]));
            if (KeyType == WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY) {
                if (Peer == NULL || PeerLen != 6) return FALSE;
                Keys[N].Pairwise = TRUE;
                RtlCopyMemory(Keys[N].Peer, Peer, 6);
            } else if (KeyType == WDI_CIPHER_KEY_TYPE_GROUP_KEY) {
                if (Id == NULL) return FALSE;
                Keys[N].Pairwise = FALSE;
            } else return FALSE;
            if (Id != NULL) RtlCopyMemory(&Keys[N].Index, Id, sizeof(UINT32));
            if (Rsc != NULL) { Keys[N].HaveRsc = TRUE; RtlCopyMemory(Keys[N].Rsc, Rsc, 6); }
            Keys[N].Len = Delete ? 0 : KeyLen;
            if (!Delete) RtlCopyMemory(Keys[N].Data, Key, KeyLen);
            N++;
        }
        Off += L;
    }
    *Count = N;
    return N != 0;
}

/* One WDI_TLV_BSS_ENTRY: BSSID, beacon frame body (timestamp, interval,
 * capability, IEs), signal info (RSSI, link quality), channel info. */
static ULONG WlanPutBss(PUCHAR Out, ULONG Off, const BCM_BSS *B)
{
    ULONG Start = Off, Body = 12 + B->IeLen;
    UCHAR Frame[12 + BCM_IE_MAX];
    LONG Rssi = B->Rssi;
    ULONG Quality, Signal[2], Channel[2];

    Off += 4;                                           /* container header, filled below */
    Off = PutTlv(Out, Off, WDI_TLV_BSSID, B->Bssid, 6);
    RtlZeroMemory(Frame, 8);                            /* timestamp */
    RtlCopyMemory(Frame + 8, &B->BeaconPeriod, 2);
    RtlCopyMemory(Frame + 10, &B->Capability, 2);
    RtlCopyMemory(Frame + 12, B->Ie, B->IeLen);
    Off = PutTlv(Out, Off, WDI_TLV_BEACON_FRAME, Frame, Body);
    Quality = (Rssi >= -50) ? 100 : (Rssi <= -100) ? 0 : (ULONG)(2 * (Rssi + 100));
    Signal[0] = (ULONG)Rssi;
    Signal[1] = Quality;
    Off = PutTlv(Out, Off, WDI_TLV_BSS_ENTRY_SIGNAL_INFO, Signal, 8);
    Channel[0] = B->Channel;
    Channel[1] = B->Band;
    Off = PutTlv(Out, Off, WDI_TLV_BSS_ENTRY_CHANNEL_INFO, Channel, 8);
    (VOID)PutTlv(Out, Start, WDI_TLV_BSS_ENTRY, NULL, Off - Start - 4);
    return Off;
}

/* ------------------------------------------------------------- worker */

static VOID WlanRunScan(PWLAN_DEVICE Ctx, const WDI_MESSAGE_HEADER *Request)
{
    static UCHAR Buf[8 * (64 + BCM_IE_MAX)];
    NTSTATUS Status = BcmScanBegin();
    ULONG Ms = 0, I, Off = 0, InBuf = 0, Sent = 0;

    Ctx->Scans++;
    BcmTrace(L"WlanScans", Ctx->Scans);
    BcmTrace(L"WlanScanBeginStatus", (ULONG)Status);
    if (NT_SUCCESS(Status)) {
        while (!BcmScanFinished() && Ms < 8000 && !Ctx->Stop) {
            LARGE_INTEGER Delay;
            Delay.QuadPart = -20 * 10000LL;
            KeDelayExecutionThread(KernelMode, FALSE, &Delay);
            BcmPoll();
            Ms += 20;
        }
        BcmTrace(L"WlanScanMs", Ms);
        for (I = 0; I < BcmScanCount(); I++) {
            const BCM_BSS *B = BcmScanEntry(I);
            if (B == NULL || B->SsidLen == 0) continue;     /* hidden: no SSID to show */
            Off = WlanPutBss(Buf, Off, B);
            if (++InBuf == 8) {
                WlanIndicate(Ctx, WDI_INDICATION_BSS_ENTRY_LIST, Request, 0, NDIS_STATUS_SUCCESS, Buf, Off);
                Sent += InBuf;
                Off = 0;
                InBuf = 0;
            }
        }
        if (InBuf) {
            WlanIndicate(Ctx, WDI_INDICATION_BSS_ENTRY_LIST, Request, 0, NDIS_STATUS_SUCCESS, Buf, Off);
            Sent += InBuf;
        }
        BcmScanSaveList();
    }
    BcmTrace(L"WlanScanReported", Sent);
    WlanTaskComplete(Ctx, WDI_INDICATION_SCAN_COMPLETE, Request, NT_SUCCESS(Status) ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE);
}

/* 0.6.8: WiFiCx carries 802.11 data frames (the host does the 802.3 <-> 802.11
 * translation; cf. WDI_TLV_ETHERTYPE_ENCAP_TABLE), while the firmware's msgbuf
 * path uses 802.3. Rx: Ethernet -> 802.11 data (FromDS, A1=DA, A2=BSSID, A3=SA)
 * + RFC 1042 LLC/SNAP. Tx: 802.11 data (+LLC/SNAP) -> Ethernet. */
#define WLAN_80211_HDR  24u
static ULONG WlanEthTo80211(const UCHAR *Eth, ULONG Len, const UCHAR Bssid[6], PUCHAR Out, ULONG OutMax)
{
    ULONG Payload = Len - 14;
    if (Len < 14 || WLAN_80211_HDR + 8 + Payload > OutMax) return 0;
    RtlZeroMemory(Out, WLAN_80211_HDR);
    Out[0] = 0x08;                          /* data */
    Out[1] = 0x02;                          /* FromDS */
    RtlCopyMemory(Out + 4, Eth, 6);         /* A1 = DA */
    RtlCopyMemory(Out + 10, Bssid, 6);      /* A2 = BSSID */
    RtlCopyMemory(Out + 16, Eth + 6, 6);    /* A3 = SA */
    Out[24] = 0xAA; Out[25] = 0xAA; Out[26] = 0x03;
    Out[27] = 0x00; Out[28] = 0x00; Out[29] = 0x00;
    Out[30] = Eth[12]; Out[31] = Eth[13];
    RtlCopyMemory(Out + 32, Eth + 14, Payload);
    return 32 + Payload;
}

/* In place; returns the Ethernet length or 0 when the frame is not 802.11 data + SNAP. */
static ULONG Wlan80211ToEth(PUCHAR F, ULONG Len)
{
    UCHAR Da[6], Sa[6];
    ULONG H = WLAN_80211_HDR, Payload;
    if (Len < H + 8 || ((F[0] >> 2) & 3) != 2) return 0;
    if (F[0] & 0x80) H += 2;                /* QoS data */
    if ((F[1] & 3) == 3) H += 6;            /* 4 addresses */
    if ((F[0] & 0x80) && (F[1] & 0x80)) H += 4;     /* HT control */
    if (Len < H + 8 || F[H] != 0xAA || F[H + 1] != 0xAA || F[H + 2] != 0x03) return 0;
    switch (F[1] & 3) {
    case 1:  RtlCopyMemory(Da, F + 16, 6); RtlCopyMemory(Sa, F + 10, 6); break;  /* ToDS: A1 BSSID, A2 SA, A3 DA */
    case 2:  RtlCopyMemory(Da, F + 4, 6);  RtlCopyMemory(Sa, F + 16, 6); break;
    case 3:  RtlCopyMemory(Da, F + 16, 6); RtlCopyMemory(Sa, F + 24, 6); break;
    default: RtlCopyMemory(Da, F + 4, 6);  RtlCopyMemory(Sa, F + 10, 6); break;
    }
    Payload = Len - H - 8;
    RtlCopyMemory(F + 12, F + H + 6, 2);    /* ethertype (before the payload moves) */
    RtlMoveMemory(F + 14, F + H + 8, Payload);
    RtlCopyMemory(F, Da, 6);
    RtlCopyMemory(F + 6, Sa, 6);
    return 14 + Payload;
}

/* BCM callbacks run from the passive worker. Queue ownership of the frame is
 * transferred by copying it into a bounded software ring. */
static VOID WlanReceiveFrame(PVOID Context, const UCHAR *Frame, ULONG Length)
{
    PWLAN_DEVICE Ctx = (PWLAN_DEVICE)Context;
    KIRQL Irql;
    NETPACKETQUEUE Queue = NULL;

    if (Length < 14 || Length > WLAN_FRAME_MAX) return;
    {
        /* 0.6.7: is the frame addressed to the MAC Windows knows? */
        UCHAR Mine[6];
        BcmGetMac(Mine);
        if (Frame[0] & 1) Ctx->RxDstGroup++;
        else if (RtlCompareMemory(Frame, Mine, 6) == 6) Ctx->RxDstMine++;
        else { Ctx->RxDstOther++; RtlCopyMemory(Ctx->RxOtherOui, Frame, 3); }
    }
    KeAcquireSpinLock(&Ctx->Lock, &Irql);
    if (Ctx->RxCount < WLAN_FRAME_QUEUE && Length + 18 <= WLAN_FRAME_MAX) {
        WLAN_FRAME *Slot = &Ctx->RxFrames[Ctx->RxTail];
        Slot->Length = WlanEthTo80211(Frame, Length, Ctx->Join.Bssid, Slot->Bytes, WLAN_FRAME_MAX);
        Ctx->RxTail = (Ctx->RxTail + 1) % WLAN_FRAME_QUEUE;
        Ctx->RxCount++;
        Ctx->RxQueued++;
        if (Ctx->RxQueue == NULL) Ctx->RxNoQueue++;
        if (Ctx->RxHold && Length >= 14 && Frame[12] == 0x88 && Frame[13] == 0x8E) Ctx->RxEapolHeld++;
        Queue = (Ctx->RxNotifyEnabled && !Ctx->RxHold) ? Ctx->RxQueue : NULL;
        if (Queue != NULL) {
            Ctx->RxNotifyEnabled = FALSE;       /* 0.6.10: one notification per arm */
            WdfObjectReference(Queue);
        }
    } else {
        Ctx->RxFull++;
    }
    KeReleaseSpinLock(&Ctx->Lock, Irql);
    if (Queue != NULL) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(Queue);
        WdfObjectDereference(Queue);
    }
}

static VOID WlanNotifyTx(PWLAN_DEVICE Ctx)
{
    KIRQL Irql;
    NETPACKETQUEUE Queue = NULL;
    KeAcquireSpinLock(&Ctx->Lock, &Irql);
    if (Ctx->TxNotifyEnabled && Ctx->TxQueue != NULL) {
        Queue = Ctx->TxQueue;
        WdfObjectReference(Queue);
    }
    KeReleaseSpinLock(&Ctx->Lock, Irql);
    if (Queue != NULL) {
        NetTxQueueNotifyMoreCompletedPacketsAvailable(Queue);
        WdfObjectDereference(Queue);
    }
}

/* TX packets are copied at the NetAdapterCx callback's IRQL, then posted by
 * the passive worker because the BCM msgbuf layer polls and may wait. */
static VOID WlanDrainTx(PWLAN_DEVICE Ctx)
{
    KIRQL Irql;
    BOOLEAN Committed = FALSE;
    WLAN_FRAME Frame;

    if (!BcmDataReady()) {
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        Ctx->TxDropNotReady += Ctx->TxCount;
        Ctx->TxHead = Ctx->TxTail = Ctx->TxCount = 0;
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        WlanNotifyTx(Ctx);
        return;
    }

    for (;;) {
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        if (Ctx->TxCount == 0 || Ctx->TxFrames[Ctx->TxHead].Length == 0) {
            KeReleaseSpinLock(&Ctx->Lock, Irql);
            break;
        }
        Frame = Ctx->TxFrames[Ctx->TxHead];
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        if (!BcmTxFrame(Frame.Bytes, Frame.Length, 0)) break;
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        Ctx->TxHead = (Ctx->TxHead + 1) % WLAN_FRAME_QUEUE;
        Ctx->TxCount--;
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        Committed = TRUE;
        WlanNotifyTx(Ctx);
    }
    if (Committed) BcmTxCommit();
}

/* 0.6.12: Windows now knows the association; hand over what arrived meanwhile. */
static VOID WlanReleaseRx(PWLAN_DEVICE Ctx)
{
    KIRQL Irql;
    NETPACKETQUEUE Queue = NULL;
    KeAcquireSpinLock(&Ctx->Lock, &Irql);
    Ctx->RxHold = FALSE;
    if (Ctx->RxCount != 0 && Ctx->RxNotifyEnabled && Ctx->RxQueue != NULL) {
        Queue = Ctx->RxQueue;
        Ctx->RxNotifyEnabled = FALSE;
        WdfObjectReference(Queue);
    }
    KeReleaseSpinLock(&Ctx->Lock, Irql);
    if (Queue != NULL) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(Queue);
        WdfObjectDereference(Queue);
    }
}

static VOID WlanThread(PVOID Context)
{
    PWLAN_DEVICE Ctx = (PWLAN_DEVICE)Context;
    /* 0.6.9: no interrupt yet, so the rings are polled. Poll every 1 ms while
     * frames are moving (1 ms system timer while the worker runs), 20 ms idle. */
    ULONG Idle = 1000, LastFrames = 0;
    ULONGLONG LastTick = KeQueryInterruptTime();
    ULONG DeltaMs = 0;
    (VOID)ExSetTimerResolution(10000, TRUE);

    while (!Ctx->Stop) {
        LARGE_INTEGER Timeout;
        KIRQL Irql;
        BOOLEAN Scan;
        ULONG Op;
        WDI_MESSAGE_HEADER Header;
        BCM_JOIN Join;
        BCM_KEY Keys[WLAN_KEY_MAX];
        ULONG KeyCount = 0, K;
        WIFIREQUEST KeyRequest = NULL;

        Timeout.QuadPart = (Idle < 200) ? -1 * 10000LL : -20 * 10000LL;
        (VOID)KeWaitForSingleObject(&Ctx->Wake, Executive, KernelMode, FALSE, &Timeout);
        {
            ULONG Frames = Ctx->RxQueued + Ctx->TxFromOs;
            ULONGLONG Now = KeQueryInterruptTime();
            if (Frames != LastFrames) { LastFrames = Frames; Idle = 0; } else if (Idle < 1000) Idle++;
            DeltaMs = (ULONG)((Now - LastTick) / 10000ULL);
            LastTick = Now;
        }
        if (Ctx->Stop) break;
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        Op = Ctx->PendingOp;
        Scan = Ctx->ScanPending;
        if (Scan && (Op == WLAN_OP_CONNECT || Op == WLAN_OP_CONNECTING)) {
            Scan = FALSE;                       /* 0.6.12: run it after the connect */
            Ctx->ScanDeferred++;
        } else {
            Ctx->ScanPending = FALSE;
        }
        Header = Ctx->ScanHeader;
        /* 0.6.3: a scan keeps its own header; a pending op waits for the next turn. */
        if (!Scan && Op != WLAN_OP_NONE) Header = Ctx->OpHeader;
        /* 0.6.3: DISCONNECT / DOT11_RESET are done once (0.6.2 never cleared them). */
        if (!Scan && (Op == WLAN_OP_DISCONNECT || Op == WLAN_OP_RESET)) Ctx->PendingOp = WLAN_OP_NONE;
        Join = Ctx->Join;
        if (Ctx->KeyPending) {
            KeyCount = Ctx->PendingKeyCount;
            RtlCopyMemory(Keys, Ctx->PendingKeys, sizeof(Keys));
            KeyRequest = Ctx->KeyRequest;
            Ctx->KeyPending = FALSE;
            Ctx->PendingKeyCount = 0;
            Ctx->KeyRequest = NULL;
            RtlSecureZeroMemory(Ctx->PendingKeys, sizeof(Ctx->PendingKeys));
        }
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        if (Scan) {
            WlanRunScan(Ctx, &Header);
        } else if (Op == WLAN_OP_CONNECT) {
            NTSTATUS St;
            Ctx->RxHold = TRUE;
            St = BcmJoin(&Join);
            if (!NT_SUCCESS(St)) {
                Ctx->RxHold = FALSE;
                KeAcquireSpinLock(&Ctx->Lock, &Irql);
                Ctx->PendingOp = WLAN_OP_NONE;
                KeReleaseSpinLock(&Ctx->Lock, Irql);
                WlanTaskComplete(Ctx, WDI_INDICATION_CONNECT_COMPLETE, &Header, NDIS_STATUS_FAILURE);
            } else {
                KeAcquireSpinLock(&Ctx->Lock, &Irql);
                Ctx->OpHeader = Header;
                Ctx->PendingOp = WLAN_OP_CONNECTING;
                Ctx->OpElapsedMs = 0;
                KeReleaseSpinLock(&Ctx->Lock, Irql);
            }
        } else if (Op == WLAN_OP_DISCONNECT || Op == WLAN_OP_RESET) {
            BcmDataStop();
            BcmLeave();
            WlanReleaseRx(Ctx);
            WlanTaskComplete(Ctx, (Op == WLAN_OP_RESET) ? WDI_INDICATION_DOT11_RESET_COMPLETE : WDI_INDICATION_DISCONNECT_COMPLETE,
                             &Header, NDIS_STATUS_SUCCESS);
        } else {
            BcmPoll();                      /* events between scans */
            KeAcquireSpinLock(&Ctx->Lock, &Irql);
            if (Ctx->PendingOp == WLAN_OP_CONNECTING) {
                ULONG Link = BcmLinkState();
                Ctx->OpElapsedMs += DeltaMs;
                if (Link == BCM_LINK_UP || Link == BCM_LINK_FAILED || Link == BCM_LINK_LOST || Ctx->OpElapsedMs >= 20000) {
                    Header = Ctx->OpHeader;
                    Ctx->PendingOp = WLAN_OP_NONE;
                    Op = (Link == BCM_LINK_UP) ? WLAN_OP_CONNECT : WLAN_OP_DISCONNECT;
                } else Op = WLAN_OP_NONE;
            } else Op = WLAN_OP_NONE;
            KeReleaseSpinLock(&Ctx->Lock, Irql);
            if (Op == WLAN_OP_CONNECT) {
                ULONGLONG T0 = KeQueryInterruptTime();
                NTSTATUS St;
                Ctx->ConnLinkMs = Ctx->OpElapsedMs;
                St = BcmDataStart(Join.Bssid);
                BcmTrace(L"WlanDataStart", (ULONG)St);
                if (NT_SUCCESS(St)) {
                    /* Give the WDI supplicant the negotiated security and
                     * association IE context before completing CONNECT. */
                    WlanIndicateAssociationResult(Ctx, &Header, &Join);
                } else {
                    BcmDataStop();
                    BcmLeave();
                }
                WlanTaskComplete(Ctx, WDI_INDICATION_CONNECT_COMPLETE, &Header,
                                 NT_SUCCESS(St) ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE);
                Ctx->ConnDoneMs = (ULONG)((KeQueryInterruptTime() - T0) / 10000ULL);
                WlanReleaseRx(Ctx);
            } else if (Op == WLAN_OP_DISCONNECT) {
                if (Header.TransactionId != 0) WlanTaskComplete(Ctx, WDI_INDICATION_CONNECT_COMPLETE, &Header, NDIS_STATUS_FAILURE);
                WlanReleaseRx(Ctx);
            }
        }
        if (KeyRequest != NULL) {
            NTSTATUS KeyStatus = STATUS_SUCCESS;
            for (K = 0; K < KeyCount; K++) {
                KeyStatus = BcmSetKey(&Keys[K]);
                if (!NT_SUCCESS(KeyStatus)) break;
            }
            RtlSecureZeroMemory(Keys, sizeof(Keys));
            WifiRequestComplete(KeyRequest, KeyStatus, sizeof(WDI_MESSAGE_HEADER));
        }
        WlanDrainTx(Ctx);
        Ctx->Ticks++;
        {
            ULONGLONG Now = KeQueryInterruptTime();
            if (Now - Ctx->TraceAt >= 10000000ULL) { Ctx->TraceAt = Now; WlanSaveTrace(Ctx); }
        }
    }
    (VOID)ExSetTimerResolution(0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ------------------------------------------------------------- commands */

static VOID EvtSendCommand(WDFDEVICE Device, WIFIREQUEST Request)
{
    PWLAN_DEVICE Ctx = WlanGetDevice(Device);
    UINT InLen = 0, OutLen = 0;
    PWDI_MESSAGE_HEADER Header = (PWDI_MESSAGE_HEADER)WifiRequestGetInOutBuffer(Request, &InLen, &OutLen);
    UINT16 Id = WifiRequestGetMessageId(Request);
    WDI_MESSAGE_HEADER Copy;
    KIRQL Irql;

    Ctx->Commands++;
    WlanLog(Ctx, Id);
    if (Header == NULL || InLen < sizeof(WDI_MESSAGE_HEADER)) {
        WifiRequestComplete(Request, STATUS_INVALID_PARAMETER, 0);
        return;
    }
    Copy = *Header;

    switch (Id) {
    case WDI_TASK_SET_RADIO_STATE:
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));
        WlanTaskComplete(Ctx, WDI_INDICATION_SET_RADIO_STATE_COMPLETE, &Copy, NDIS_STATUS_SUCCESS);
        break;

    case WDI_TASK_DOT11_RESET:
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        if (Ctx->PendingOp != WLAN_OP_NONE) {
            KeReleaseSpinLock(&Ctx->Lock, Irql);
            WifiRequestComplete(Request, STATUS_DEVICE_BUSY, sizeof(WDI_MESSAGE_HEADER));
            break;
        }
        Ctx->PendingOp = WLAN_OP_RESET;
        Ctx->OpHeader = Copy;
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));
        KeSetEvent(&Ctx->Wake, IO_NO_INCREMENT, FALSE);
        break;

    case WDI_TASK_DISCONNECT:
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        if (Ctx->PendingOp != WLAN_OP_NONE) {
            KeReleaseSpinLock(&Ctx->Lock, Irql);
            WifiRequestComplete(Request, STATUS_DEVICE_BUSY, sizeof(WDI_MESSAGE_HEADER));
            break;
        }
        Ctx->OpHeader = Copy;
        Ctx->PendingOp = WLAN_OP_DISCONNECT;
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));
        KeSetEvent(&Ctx->Wake, IO_NO_INCREMENT, FALSE);
        break;

    case WDI_TASK_CONNECT:
        {
        BCM_JOIN Join;
        if (!BcmIsReady() || !WlanParseConnect((const UCHAR *)Header + sizeof(*Header), InLen - sizeof(*Header), &Join)) {
            WifiRequestComplete(Request, STATUS_INVALID_PARAMETER, sizeof(WDI_MESSAGE_HEADER));
            break;
        }
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        if (Ctx->PendingOp != WLAN_OP_NONE) {
            KeReleaseSpinLock(&Ctx->Lock, Irql);
            WifiRequestComplete(Request, STATUS_DEVICE_BUSY, sizeof(WDI_MESSAGE_HEADER));
            break;
        }
        Ctx->Join = Join;
        Ctx->OpHeader = Copy;
        Ctx->OpElapsedMs = 0;
        Ctx->PendingOp = WLAN_OP_CONNECT;
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));
        KeSetEvent(&Ctx->Wake, IO_NO_INCREMENT, FALSE);
        break;
        }

    case WDI_TASK_SCAN:
        if (!BcmIsReady()) {
            WifiRequestComplete(Request, STATUS_DEVICE_NOT_READY, sizeof(WDI_MESSAGE_HEADER));
            break;
        }
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        Ctx->ScanHeader = Copy;
        Ctx->ScanPending = TRUE;
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        KeSetEvent(&Ctx->Wake, IO_NO_INCREMENT, FALSE);
        break;

    case WDI_SET_ADD_CIPHER_KEYS:
    case WDI_SET_DELETE_CIPHER_KEYS:
        {
        BCM_KEY Keys[WLAN_KEY_MAX];
        ULONG Count = 0;
        BOOLEAN Delete = (Id == WDI_SET_DELETE_CIPHER_KEYS);
        RtlZeroMemory(Keys, sizeof(Keys));
        if (!BcmIsReady() || !WlanParseCcmpKeys((const UCHAR *)Header + sizeof(*Header), InLen - sizeof(*Header), Delete, Keys, &Count)) {
            WifiRequestComplete(Request, STATUS_NOT_SUPPORTED, sizeof(WDI_MESSAGE_HEADER));
            RtlSecureZeroMemory(Keys, sizeof(Keys));
            break;
        }
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        if (Ctx->KeyPending) {
            KeReleaseSpinLock(&Ctx->Lock, Irql);
            RtlSecureZeroMemory(Keys, sizeof(Keys));
            WifiRequestComplete(Request, STATUS_DEVICE_BUSY, sizeof(WDI_MESSAGE_HEADER));
            break;
        }
        RtlCopyMemory(Ctx->PendingKeys, Keys, sizeof(Keys));
        Ctx->PendingKeyCount = Count;
        Ctx->KeyRequest = Request;
        Ctx->KeyPending = TRUE;
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        RtlSecureZeroMemory(Keys, sizeof(Keys));
        KeSetEvent(&Ctx->Wake, IO_NO_INCREMENT, FALSE);
        break;
        }

    case WDI_SET_ADAPTER_CONFIGURATION:
    case WDI_ABORT_TASK:
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));
        break;

    default:
        Ctx->Unsupported++;
        Ctx->LastUnsupported = Id;
        WifiRequestComplete(Request, STATUS_NOT_SUPPORTED, sizeof(WDI_MESSAGE_HEADER));
        break;
    }
}

/* ------------------------------------------------------------- capabilities */

static NTSTATUS WlanSetCapabilities(WDFDEVICE Device)
{
    NTSTATUS Status;
    WIFI_DEVICE_CAPABILITIES Dev;
    WIFI_STATION_CAPABILITIES Sta;
    WIFI_BAND_CAPABILITIES Bands;
    WIFI_PHY_CAPABILITIES Phys;
    CHAR Ver[128];

    WIFI_DEVICE_CAPABILITIES_INIT(&Dev);
    Dev.HardwareRadioState = TRUE;
    Dev.SoftwareRadioState = TRUE;
    BcmGetFirmwareVersion(Ver, sizeof(Ver));
    if (Ver[0] == 0) RtlCopyMemory(Ver, "BCM4356", 8);
    RtlCopyMemory(Dev.FirmwareVersion, Ver, (strlen(Ver) + 1 < MAX_FIRMWARE_VERSION_LENGTH) ? strlen(Ver) + 1 : MAX_FIRMWARE_VERSION_LENGTH - 1);
    Dev.ActionFramesSupported = FALSE;
    Dev.NumRxStreams = 2;
    Dev.NumTxStreams = 2;
    Dev.BluetoothCoexistenceSupport = WDI_BLUETOOTH_COEXISTENCE_PERFORMANCE_MAINTAINED;
    Status = WifiDeviceSetDeviceCapabilities(Device, &Dev);
    BcmTrace(L"WlanCapsDevice", (ULONG)Status);
    if (!NT_SUCCESS(Status)) return Status;

    static DOT11_AUTH_CIPHER_PAIR Unicast[] = {
        { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE },
        { DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP },
    };
    static DOT11_AUTH_CIPHER_PAIR MulticastMgmt[] = {
        { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE },
    };
    WIFI_STATION_CAPABILITIES_INIT(&Sta);
    Sta.ScanSSIDListSize = 4;
    Sta.DesiredSSIDListSize = 1;
    Sta.PrivacyExemptionListSize = 1;
    Sta.KeyMappingTableSize = 32;
    Sta.DefaultKeyTableSize = 4;
    Sta.WEPKeyValueMaxLength = 0x20;
    Sta.MaxNumPerSTA = 4;
    Sta.NumSupportedUnicastAlgorithms = ARRAYSIZE(Unicast);
    Sta.UnicastAlgorithmsList = Unicast;
    Sta.NumSupportedMulticastDataAlgorithms = ARRAYSIZE(Unicast);
    Sta.MulticastDataAlgorithmsList = Unicast;
    Sta.NumSupportedMulticastMgmtAlgorithms = ARRAYSIZE(MulticastMgmt);
    Sta.MulticastMgmtAlgorithmsList = MulticastMgmt;
    Status = WifiDeviceSetStationCapabilities(Device, &Sta);
    BcmTrace(L"WlanCapsStation", (ULONG)Status);
    if (!NT_SUCCESS(Status)) return Status;

    static WDI_PHY_TYPE Phy24[] = { WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_HT };
    static WDI_PHY_TYPE Phy5[] = { WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT, WDI_PHY_TYPE_VHT };
    static WDI_CHANNEL_MAPPING_ENTRY Ch24[13];
    static WDI_CHANNEL_MAPPING_ENTRY Ch5[25];
    static const UCHAR Ch5List[25] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165 };
    static UINT32 Width24[] = { 20 };
    static UINT32 Width5[] = { 20, 40, 80 };
    static WIFI_BAND_INFO BandInfo[2];
    ULONG I;
    for (I = 0; I < 13; I++) { Ch24[I].ChannelNumber = I + 1; Ch24[I].ChannelCenterFrequency = 2412 + 5 * I; }
    for (I = 0; I < 25; I++) { Ch5[I].ChannelNumber = Ch5List[I]; Ch5[I].ChannelCenterFrequency = 5000 + 5 * Ch5List[I]; }
    RtlZeroMemory(BandInfo, sizeof(BandInfo));
    BandInfo[0].BandID = WDI_BAND_ID_2400;
    BandInfo[0].BandState = TRUE;
    BandInfo[0].NumValidPhyTypes = ARRAYSIZE(Phy24);
    BandInfo[0].ValidPhyTypeList = Phy24;
    BandInfo[0].NumValidChannelTypes = ARRAYSIZE(Ch24);
    BandInfo[0].ValidChannelTypes = Ch24;
    BandInfo[0].NumChannelWidths = ARRAYSIZE(Width24);
    BandInfo[0].ChannelWidthList = Width24;
    BandInfo[1].BandID = WDI_BAND_ID_5000;
    BandInfo[1].BandState = TRUE;
    BandInfo[1].NumValidPhyTypes = ARRAYSIZE(Phy5);
    BandInfo[1].ValidPhyTypeList = Phy5;
    BandInfo[1].NumValidChannelTypes = ARRAYSIZE(Ch5);
    BandInfo[1].ValidChannelTypes = Ch5;
    BandInfo[1].NumChannelWidths = ARRAYSIZE(Width5);
    BandInfo[1].ChannelWidthList = Width5;
    RtlZeroMemory(&Bands, sizeof(Bands));
    Bands.Size = sizeof(Bands);         /* WiFiCx 1.0 has no _INIT for this */
    Bands.NumBands = 2;
    Bands.BandInfoList = BandInfo;
    Status = WifiDeviceSetBandCapabilities(Device, &Bands);
    BcmTrace(L"WlanCapsBand", (ULONG)Status);
    if (!NT_SUCCESS(Status)) return Status;

    /* Rates in 500 kbps units. */
    static WIFI_PHY_INFO PhyInfo[4];
    static const USHORT ErpRates[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
    static const USHORT OfdmRates[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
    static const WDI_PHY_TYPE PhyTypes[4] = { WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT, WDI_PHY_TYPE_VHT };
    ULONG P;
    RtlZeroMemory(PhyInfo, sizeof(PhyInfo));
    for (P = 0; P < 4; P++) {
        const USHORT *Rates = (P == 0) ? ErpRates : OfdmRates;
        ULONG N = (P == 0) ? ARRAYSIZE(ErpRates) : ARRAYSIZE(OfdmRates);
        PhyInfo[P].PhyType = PhyTypes[P];
        PhyInfo[P].NumberDataRateEntries = N;
        for (I = 0; I < N; I++) {
            PhyInfo[P].DataRateList[I].DataRateFlag = WDI_DATA_RATE_RX_RATE | WDI_DATA_RATE_TX_RATE;
            PhyInfo[P].DataRateList[I].DataRateValue = Rates[I];
        }
    }
    RtlZeroMemory(&Phys, sizeof(Phys));
    Phys.Size = sizeof(Phys);
    Phys.NumPhyTypes = 4;
    Phys.PhyInfoList = PhyInfo;
    Status = WifiDeviceSetPhyCapabilities(Device, &Phys);
    BcmTrace(L"WlanCapsPhy", (ULONG)Status);
    return Status;
}

/* --------------------------- copy-based NetAdapterCx Ethernet data path
 * BCM msgbuf copies submitted frames, so the NET_PACKET buffers can be
 * returned once copied into the bounded software queue. BCM calls remain on
 * the passive worker; Rx frames are copied there then drained into OS buffers.
 */
static VOID EvtTxAdvance(NETPACKETQUEUE Queue)
{
    PWLAN_QUEUE Q = WlanGetQueue(Queue);
    NET_RING_COLLECTION const *Rings = NetTxQueueGetRingCollection(Queue);
    NET_RING *Packets = NetRingCollectionGetPacketRing(Rings);
    NET_RING *Fragments = NetRingCollectionGetFragmentRing(Rings);
    UINT32 P = Packets->NextIndex, F = Fragments->NextIndex;
    PWLAN_DEVICE Ctx = Q->Device;

    Ctx->TxAdvance++;
    while (P != Packets->EndIndex) {
        NET_PACKET *Packet = NetRingGetPacketAtIndex(Packets, P);
        UINT32 I, End;
        WLAN_FRAME Local;
        KIRQL Irql;
        BOOLEAN HaveSlot;

        if (Packet->Ignore) {             /* Do not inspect any other packet field. */
            P = NetRingIncrementIndex(Packets, P);
            continue;
        }
        if (Packet->FragmentCount == 0 || Packet->FragmentCount > 16) break;
        End = NetRingAdvanceIndex(Fragments, Packet->FragmentIndex, Packet->FragmentCount);
        I = Packet->FragmentIndex;
        Local.Length = 0;
        while (I != End) {
            NET_FRAGMENT *Fragment = NetRingGetFragmentAtIndex(Fragments, I);
            ULONG N = (ULONG)Fragment->ValidLength;
            if (N > WLAN_FRAME_MAX - Local.Length || N == 0) { Local.Length = 0; break; }
            NET_FRAGMENT_VIRTUAL_ADDRESS *Va = NetExtensionGetFragmentVirtualAddress(&Q->VirtualAddress, I);
            if (Va->VirtualAddress == NULL || (ULONG)Fragment->Offset + N > Fragment->Capacity) {
                Local.Length = 0;
                break;
            }
            RtlCopyMemory(Local.Bytes + Local.Length, (UCHAR *)Va->VirtualAddress + Fragment->Offset, N);
            Local.Length += N;
            I = NetRingIncrementIndex(Fragments, I);
        }
        if (Local.Length >= 14) {
            ULONG EthLen;
            Ctx->TxL2Last = Packet->Layout.Layer2Type;
            EthLen = (Packet->Layout.Layer2Type == NetPacketLayer2TypeEthernet) ? 0 : Wlan80211ToEth(Local.Bytes, Local.Length);
            if (EthLen) { Local.Length = EthLen; Ctx->TxConverted++; }
            else Ctx->TxNot80211++;
        }
        if (Local.Length < 14) {
            F = End;
            P = NetRingIncrementIndex(Packets, P);
            continue;
        }
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        HaveSlot = Ctx->TxCount < WLAN_FRAME_QUEUE;
        if (HaveSlot) {
            WLAN_FRAME *Slot = &Ctx->TxFrames[Ctx->TxTail];
            Slot->Length = 0;
            RtlCopyMemory(Slot->Bytes, Local.Bytes, Local.Length);
            Slot->Length = Local.Length;
            Ctx->TxTail = (Ctx->TxTail + 1) % WLAN_FRAME_QUEUE;
            Ctx->TxCount++;
            Ctx->TxFromOs++;
            if (Local.Bytes[12] == 0x88 && Local.Bytes[13] == 0x8E) Ctx->TxEapolFromOs++;
        }
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        if (!HaveSlot) break;
        F = End;
        P = NetRingIncrementIndex(Packets, P);
    }
    Fragments->NextIndex = F;
    Packets->NextIndex = P;
    /* Copies are owned by our bounded queue now; BCM completion is independent. */
    Fragments->BeginIndex = F;
    Packets->BeginIndex = P;
    KeSetEvent(&Ctx->Wake, IO_NO_INCREMENT, FALSE);
}

static VOID EvtTxNotify(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    PWLAN_QUEUE Q = WlanGetQueue(Queue);
    KIRQL Irql;
    KeAcquireSpinLock(&Q->Device->Lock, &Irql);
    Q->Device->TxNotifyEnabled = Enabled;
    KeReleaseSpinLock(&Q->Device->Lock, Irql);
    if (Enabled) WlanNotifyTx(Q->Device);
}

/* 0.6.4: on cancel every packet and fragment goes back to NetAdapterCx (0.5.0 did
 * this; 0.6.0-0.6.3 kept the rx buffers, so the data path could never stop: device
 * removal timed out in query-remove and wlansvc hung). */
static VOID WlanReturnAll(NETPACKETQUEUE Queue, BOOLEAN IsRx)
{
    NET_RING_COLLECTION const *Rings = IsRx ? NetRxQueueGetRingCollection(Queue) : NetTxQueueGetRingCollection(Queue);
    NET_RING *Packets = NetRingCollectionGetPacketRing(Rings);
    NET_RING *Fragments = NetRingCollectionGetFragmentRing(Rings);
    UINT32 I;
    if (IsRx) {
        for (I = Packets->BeginIndex; I != Packets->EndIndex; I = NetRingIncrementIndex(Packets, I)) {
            NetRingGetPacketAtIndex(Packets, I)->Ignore = 1;
        }
    }
    Packets->BeginIndex = Packets->NextIndex = Packets->EndIndex;
    Fragments->BeginIndex = Fragments->NextIndex = Fragments->EndIndex;
}

static VOID EvtTxCancel(NETPACKETQUEUE Queue)
{
    PWLAN_QUEUE Q = WlanGetQueue(Queue);
    KIRQL Irql;
    KeAcquireSpinLock(&Q->Device->Lock, &Irql);
    Q->Device->TxNotifyEnabled = FALSE;
    Q->Device->TxQueue = NULL;
    KeReleaseSpinLock(&Q->Device->Lock, Irql);
    WlanReturnAll(Queue, FALSE);
}

static VOID EvtRxAdvance(NETPACKETQUEUE Queue)
{
    PWLAN_QUEUE Q = WlanGetQueue(Queue);
    PWLAN_DEVICE Ctx = Q->Device;
    NET_RING_COLLECTION const *Rings = NetRxQueueGetRingCollection(Queue);
    NET_RING *Packets = NetRingCollectionGetPacketRing(Rings);
    NET_RING *Fragments = NetRingCollectionGetFragmentRing(Rings);
    UINT32 P = Packets->BeginIndex, F = Fragments->BeginIndex;
    KIRQL Irql;

    Ctx->RxAdvance++;
    if (Ctx->RxHold) {                          /* 0.6.12: keep them until CONNECT_COMPLETE */
        Fragments->NextIndex = Fragments->EndIndex;
        Packets->NextIndex = Packets->BeginIndex;
        return;
    }
    /* Framework-owned fragments become available to this copy-based receiver. */
    Fragments->NextIndex = Fragments->EndIndex;
    {
        /* 0.6.10: one lock to see how many frames wait, copy straight from the
         * slots (single consumer; the producer only writes free slots), one lock
         * to give them back. 0.6.9 copied each 2 KB slot to the stack first. */
        ULONG Avail, Head, Done = 0;
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        Avail = Ctx->RxCount;
        Head = Ctx->RxHead;
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        while (Done < Avail && P != Packets->EndIndex && F != Fragments->NextIndex) {
            const WLAN_FRAME *Frame = &Ctx->RxFrames[(Head + Done) % WLAN_FRAME_QUEUE];
            NET_FRAGMENT *Fragment = NetRingGetFragmentAtIndex(Fragments, F);
            NET_PACKET *Packet;
            NET_FRAGMENT_VIRTUAL_ADDRESS *Va;
            if (Frame->Length >= Fragment->Capacity - Fragment->Offset) break;
            Va = NetExtensionGetFragmentVirtualAddress(&Q->VirtualAddress, F);
            if (Va->VirtualAddress == NULL) break;
            RtlCopyMemory((UCHAR *)Va->VirtualAddress + Fragment->Offset, Frame->Bytes, Frame->Length);
            Fragment->ValidLength = Frame->Length;
            Packet = NetRingGetPacketAtIndex(Packets, P);
            Packet->FragmentIndex = F;
            Packet->FragmentCount = 1;
            RtlZeroMemory(&Packet->Layout, sizeof(Packet->Layout));
            Packet->Layout.Layer2Type = NetPacketLayer2TypeIeee80211;
            Packet->Layout.Layer2HeaderLength = WLAN_80211_HDR;
            Packet->Ignore = 0;
            Ctx->RxToOs++;
            if (Frame->Length >= 32 && Frame->Bytes[30] == 0x88 && Frame->Bytes[31] == 0x8E) Ctx->RxEapolToOs++;
            Done++;
            F = NetRingIncrementIndex(Fragments, F);
            P = NetRingIncrementIndex(Packets, P);
        }
        if (Done) {
            KeAcquireSpinLock(&Ctx->Lock, &Irql);
            Ctx->RxHead = (Ctx->RxHead + Done) % WLAN_FRAME_QUEUE;
            Ctx->RxCount -= Done;
            KeReleaseSpinLock(&Ctx->Lock, Irql);
        }
    }
    Fragments->BeginIndex = F;
    Packets->BeginIndex = P;
    Packets->NextIndex = P;
}

static VOID EvtRxNotify(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    PWLAN_QUEUE Q = WlanGetQueue(Queue);
    PWLAN_DEVICE Ctx = Q->Device;
    KIRQL Irql;
    BOOLEAN Ready;
    KeAcquireSpinLock(&Ctx->Lock, &Irql);
    Ctx->RxNotifyEnabled = Enabled;
    Ready = Enabled && Ctx->RxCount != 0;
    KeReleaseSpinLock(&Ctx->Lock, Irql);
    if (Ready) NetRxQueueNotifyMoreReceivedPacketsAvailable(Queue);
}

static VOID EvtRxCancel(NETPACKETQUEUE Queue)
{
    PWLAN_QUEUE Q = WlanGetQueue(Queue);
    KIRQL Irql;
    KeAcquireSpinLock(&Q->Device->Lock, &Irql);
    Q->Device->RxNotifyEnabled = FALSE;
    Q->Device->RxQueue = NULL;
    KeReleaseSpinLock(&Q->Device->Lock, Irql);
    WlanReturnAll(Queue, TRUE);
}

static NTSTATUS EvtCreateTxQueue(NETADAPTER Adapter, NETTXQUEUE_INIT *Init)
{
    NET_PACKET_QUEUE_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    NETPACKETQUEUE Queue;
    NET_EXTENSION_QUERY Query;
    PWLAN_DEVICE Ctx = WlanGetDevice(WlanGetAdapter(Adapter)->Device);
    NET_PACKET_QUEUE_CONFIG_INIT(&Config, EvtTxAdvance, EvtTxNotify, EvtTxCancel);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WLAN_QUEUE);
    NTSTATUS Status = NetTxQueueCreate(Init, &Attributes, &Config, &Queue);
    if (!NT_SUCCESS(Status)) return Status;
    PWLAN_QUEUE Q = WlanGetQueue(Queue);
    Q->Device = Ctx;
    Q->IsRx = FALSE;
    NET_EXTENSION_QUERY_INIT(&Query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
        NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetTxQueueGetExtension(Queue, &Query, &Q->VirtualAddress);
    KIRQL Irql;
    KeAcquireSpinLock(&Ctx->Lock, &Irql);
    Ctx->TxQueue = Queue;
    KeReleaseSpinLock(&Ctx->Lock, Irql);
    return STATUS_SUCCESS;
}

static NTSTATUS EvtCreateRxQueue(NETADAPTER Adapter, NETRXQUEUE_INIT *Init)
{
    NET_PACKET_QUEUE_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    NETPACKETQUEUE Queue;
    NET_EXTENSION_QUERY Query;
    PWLAN_DEVICE Ctx = WlanGetDevice(WlanGetAdapter(Adapter)->Device);
    NET_PACKET_QUEUE_CONFIG_INIT(&Config, EvtRxAdvance, EvtRxNotify, EvtRxCancel);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WLAN_QUEUE);
    NTSTATUS Status = NetRxQueueCreate(Init, &Attributes, &Config, &Queue);
    if (!NT_SUCCESS(Status)) return Status;
    PWLAN_QUEUE Q = WlanGetQueue(Queue);
    Q->Device = Ctx;
    Q->IsRx = TRUE;
    NET_EXTENSION_QUERY_INIT(&Query, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
        NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
    NetRxQueueGetExtension(Queue, &Query, &Q->VirtualAddress);
    KIRQL Irql;
    KeAcquireSpinLock(&Ctx->Lock, &Irql);
    Ctx->RxQueue = Queue;
    KeReleaseSpinLock(&Ctx->Lock, Irql);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------- adapter */

static NTSTATUS EvtCreateAdapter(WDFDEVICE Device, NETADAPTER_INIT *AdapterInit)
{
    PWLAN_DEVICE Ctx = WlanGetDevice(Device);
    NET_ADAPTER_DATAPATH_CALLBACKS Datapath;
    NETADAPTER Adapter;
    WDF_OBJECT_ATTRIBUTES AdapterAttributes;
    NTSTATUS Status;
    UCHAR Mac[6];

    BcmTrace(L"WlanAdapterType", (ULONG)WifiAdapterInitGetType(AdapterInit));
    if (WifiAdapterInitGetType(AdapterInit) != WIFI_ADAPTER_EXTENSIBLE_STATION) {
        return STATUS_NOT_SUPPORTED;
    }
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&Datapath, EvtCreateTxQueue, EvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(AdapterInit, &Datapath);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&AdapterAttributes, WLAN_ADAPTER);
    Status = NetAdapterCreate(AdapterInit, &AdapterAttributes, &Adapter);
    BcmTrace(L"WlanAdapterCreate", (ULONG)Status);
    if (!NT_SUCCESS(Status)) return Status;
    WlanGetAdapter(Adapter)->Device = Device;
    Status = WifiAdapterInitialize(Adapter);
    BcmTrace(L"WlanAdapterInit", (ULONG)Status);
    if (!NT_SUCCESS(Status)) return Status;

    NET_ADAPTER_TX_CAPABILITIES Tx;
    NET_ADAPTER_RX_CAPABILITIES Rx;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES Link;
    NET_ADAPTER_LINK_LAYER_ADDRESS Address;
    NET_ADAPTER_TX_CAPABILITIES_INIT(&Tx, 1);
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&Rx, 2048, 1);
    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&Link, 866700000ULL, 866700000ULL);
    NetAdapterSetLinkLayerCapabilities(Adapter, &Link);
    NetAdapterSetLinkLayerMtuSize(Adapter, 1500);
    NetAdapterSetDataPathCapabilities(Adapter, &Tx, &Rx);
    BcmGetMac(Mac);
    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&Address, 6, Mac);
    NetAdapterSetPermanentLinkLayerAddress(Adapter, &Address);
    NetAdapterSetCurrentLinkLayerAddress(Adapter, &Address);
    Status = NetAdapterStart(Adapter);
    BcmTrace(L"WlanAdapterStart", (ULONG)Status);
    if (NT_SUCCESS(Status)) Ctx->Adapter = Adapter;
    return Status;
}

static NTSTATUS EvtCreateWifiDirect(WDFDEVICE Device, WIFIDIRECT_DEVICE_INIT *Init)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Init);
    return STATUS_SUCCESS;          /* as the WDK sample: no Wi-Fi Direct */
}

/* ------------------------------------------------------------- PnP */

static NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PWLAN_DEVICE Ctx = WlanGetDevice(Device);
    HANDLE Thread;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Raw);

    KeInitializeSpinLock(&Ctx->Lock);
    BcmTrace(L"WlanVersion", WLAN_VERSION);
    Status = BcmPrepareHardware(Translated);
    BcmTrace(L"WlanPrepareStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) return Status;
    BcmTrace(L"WlanChipReady", BcmIsReady());
    Status = WlanSetCapabilities(Device);
    if (!NT_SUCCESS(Status)) return Status;

    Ctx->Stop = 0;
    KeInitializeEvent(&Ctx->Wake, SynchronizationEvent, FALSE);
    BcmSetRxHandler(WlanReceiveFrame, Ctx);
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, WlanThread, Ctx);
    if (!NT_SUCCESS(Status)) return Status;
    Status = ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&Ctx->Thread, NULL);
    ZwClose(Thread);
    return Status;
}

static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PWLAN_DEVICE Ctx = WlanGetDevice(Device);
    WIFIREQUEST PendingKeyRequest = NULL;
    KIRQL Irql;
    UNREFERENCED_PARAMETER(Translated);

    if (Ctx->Thread != NULL) {
        InterlockedExchange(&Ctx->Stop, 1);
        KeSetEvent(&Ctx->Wake, IO_NO_INCREMENT, FALSE);
        (VOID)KeWaitForSingleObject(Ctx->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Ctx->Thread);
        Ctx->Thread = NULL;
    }
    KeAcquireSpinLock(&Ctx->Lock, &Irql);
    PendingKeyRequest = Ctx->KeyRequest;
    Ctx->KeyRequest = NULL;
    Ctx->KeyPending = FALSE;
    Ctx->PendingKeyCount = 0;
    RtlSecureZeroMemory(Ctx->PendingKeys, sizeof(Ctx->PendingKeys));
    KeReleaseSpinLock(&Ctx->Lock, Irql);
    if (PendingKeyRequest != NULL) WifiRequestComplete(PendingKeyRequest, STATUS_CANCELLED, sizeof(WDI_MESSAGE_HEADER));
    BcmSetRxHandler(NULL, NULL);
    BcmTrace(L"WlanCommands", Ctx->Commands);
    BcmTrace(L"WlanUnsupported", Ctx->Unsupported);
    BcmTrace(L"WlanLastUnsupported", Ctx->LastUnsupported);
    BcmReleaseHardware();
    return STATUS_SUCCESS;
}

static NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WIFI_DEVICE_CONFIG Config;
    WDFDEVICE Device;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    Status = NetDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(Status)) return Status;
    Status = WifiDeviceInitConfig(DeviceInit);
    if (!NT_SUCCESS(Status)) return Status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WLAN_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) return Status;
    PWLAN_DEVICE Ctx = WlanGetDevice(Device);
    Ctx->WdfTriageInfoPtr = WdfGetTriageInfo();
    Ctx->Device = Device;
    WIFI_DEVICE_CONFIG_INIT(&Config, WDI_VERSION_LATEST, EvtSendCommand, EvtCreateAdapter, EvtCreateWifiDirect);
    Status = WifiDeviceInitialize(Device, &Config);
    BcmTrace(L"WlanWifiInit", (ULONG)Status);
    if (NT_SUCCESS(Status)) BcmTrace(L"WlanOsWdiVersion", WifiDeviceGetOsWdiVersion(Device));
    return Status;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    BcmSetRegistryPath(RegistryPath);
    WDF_DRIVER_CONFIG_INIT(&Config, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
