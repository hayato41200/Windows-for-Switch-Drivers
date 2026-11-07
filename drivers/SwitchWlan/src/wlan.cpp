/*
 * switchwlan: Nintendo Switch Wi-Fi (BCM4356 on Tegra210 PCIe) as a WiFiCx
 * client driver (Windows 11 22H2+: WiFiCx 1.0, NetAdapterCx 2.2, KMDF 1.33).
 *
 * Stage 4b: Windows' own Wi-Fi list. The control path follows the WDK WiFiCx
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
 * Not yet: connect (CONNECT fails cleanly), data path (Tx packets are dropped,
 * nothing is received), interrupts (the worker polls every 20 ms).
 */
#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <wificx.h>
#include "bcm.h"

#define WLAN_VERSION        0x00050000u     /* 0.5.0 */
#define WLAN_TAG            'nlWS'

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
    ULONG Scans, Commands, Unsupported, LastUnsupported;
} WLAN_DEVICE, *PWLAN_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WLAN_DEVICE, WlanGetDevice)

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
    WifiDeviceReceiveIndication(Ctx->Device, MessageId, Memory);
    WdfObjectDelete(Memory);
}

/* M4: the task's completion indication, same TransactionId as the request. */
static VOID WlanTaskComplete(PWLAN_DEVICE Ctx, UINT16 Indication, const WDI_MESSAGE_HEADER *Request, NDIS_STATUS Status)
{
    WlanIndicate(Ctx, Indication, Request, Request->TransactionId, Status, NULL, 0);
}

static ULONG PutTlv(PUCHAR Out, ULONG Off, USHORT Type, const VOID *Data, ULONG Len)
{
    RtlCopyMemory(Out + Off, &Type, 2);
    USHORT L16 = (USHORT)Len;
    RtlCopyMemory(Out + Off + 2, &L16, 2);
    if (Len && Data) RtlCopyMemory(Out + Off + 4, Data, Len);
    return Off + 4 + Len;
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

static VOID WlanThread(PVOID Context)
{
    PWLAN_DEVICE Ctx = (PWLAN_DEVICE)Context;

    while (!Ctx->Stop) {
        LARGE_INTEGER Timeout;
        KIRQL Irql;
        BOOLEAN Scan;
        WDI_MESSAGE_HEADER Header;

        Timeout.QuadPart = -20 * 10000LL;
        (VOID)KeWaitForSingleObject(&Ctx->Wake, Executive, KernelMode, FALSE, &Timeout);
        if (Ctx->Stop) break;
        KeAcquireSpinLock(&Ctx->Lock, &Irql);
        Scan = Ctx->ScanPending;
        Header = Ctx->ScanHeader;
        Ctx->ScanPending = FALSE;
        KeReleaseSpinLock(&Ctx->Lock, Irql);
        if (Scan) {
            WlanRunScan(Ctx, &Header);
        } else {
            BcmPoll();                      /* events between scans */
        }
    }
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
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));
        WlanTaskComplete(Ctx, WDI_INDICATION_DOT11_RESET_COMPLETE, &Copy, NDIS_STATUS_SUCCESS);
        break;

    case WDI_TASK_DISCONNECT:
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));
        WlanTaskComplete(Ctx, WDI_INDICATION_DISCONNECT_COMPLETE, &Copy, NDIS_STATUS_SUCCESS);
        break;

    case WDI_TASK_CONNECT:
        /* 0.5.0: not yet. Fail the task cleanly so the UI says it could not connect. */
        WifiRequestComplete(Request, STATUS_SUCCESS, sizeof(WDI_MESSAGE_HEADER));
        WlanTaskComplete(Ctx, WDI_INDICATION_CONNECT_COMPLETE, &Copy, NDIS_STATUS_FAILURE);
        break;

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

/* ------------------------------------------------------------- data path (stub) */

static VOID WlanReturnAll(NETPACKETQUEUE Queue, BOOLEAN Rx)
{
    NET_RING_COLLECTION const *Rings = Rx ? NetRxQueueGetRingCollection(Queue) : NetTxQueueGetRingCollection(Queue);
    NET_RING *Packets = NetRingCollectionGetPacketRing(Rings);
    NET_RING *Fragments = NetRingCollectionGetFragmentRing(Rings);
    UINT32 I;

    if (Rx) {
        for (I = Packets->BeginIndex; I != Packets->EndIndex; I = NetRingIncrementIndex(Packets, I)) {
            NetRingGetPacketAtIndex(Packets, I)->Ignore = 1;
        }
    }
    Packets->BeginIndex = Packets->NextIndex = Packets->EndIndex;
    Fragments->BeginIndex = Fragments->NextIndex = Fragments->EndIndex;
}

/* 0.5.0: no data path yet. Tx packets are given back unsent; Rx buffers are
 * kept until the queue is cancelled. */
static VOID EvtTxAdvance(NETPACKETQUEUE Queue) { WlanReturnAll(Queue, FALSE); }
static VOID EvtTxNotify(NETPACKETQUEUE Queue, BOOLEAN Enabled) { UNREFERENCED_PARAMETER(Queue); UNREFERENCED_PARAMETER(Enabled); }
static VOID EvtTxCancel(NETPACKETQUEUE Queue) { WlanReturnAll(Queue, FALSE); }
static VOID EvtRxAdvance(NETPACKETQUEUE Queue) { UNREFERENCED_PARAMETER(Queue); }
static VOID EvtRxNotify(NETPACKETQUEUE Queue, BOOLEAN Enabled) { UNREFERENCED_PARAMETER(Queue); UNREFERENCED_PARAMETER(Enabled); }
static VOID EvtRxCancel(NETPACKETQUEUE Queue) { WlanReturnAll(Queue, TRUE); }

static NTSTATUS EvtCreateTxQueue(NETADAPTER Adapter, NETTXQUEUE_INIT *Init)
{
    NET_PACKET_QUEUE_CONFIG Config;
    NETPACKETQUEUE Queue;
    UNREFERENCED_PARAMETER(Adapter);
    NET_PACKET_QUEUE_CONFIG_INIT(&Config, EvtTxAdvance, EvtTxNotify, EvtTxCancel);
    return NetTxQueueCreate(Init, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Queue);
}

static NTSTATUS EvtCreateRxQueue(NETADAPTER Adapter, NETRXQUEUE_INIT *Init)
{
    NET_PACKET_QUEUE_CONFIG Config;
    NETPACKETQUEUE Queue;
    UNREFERENCED_PARAMETER(Adapter);
    NET_PACKET_QUEUE_CONFIG_INIT(&Config, EvtRxAdvance, EvtRxNotify, EvtRxCancel);
    return NetRxQueueCreate(Init, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Queue);
}

/* ------------------------------------------------------------- adapter */

static NTSTATUS EvtCreateAdapter(WDFDEVICE Device, NETADAPTER_INIT *AdapterInit)
{
    PWLAN_DEVICE Ctx = WlanGetDevice(Device);
    NET_ADAPTER_DATAPATH_CALLBACKS Datapath;
    NETADAPTER Adapter;
    NTSTATUS Status;
    UCHAR Mac[6];

    BcmTrace(L"WlanAdapterType", (ULONG)WifiAdapterInitGetType(AdapterInit));
    if (WifiAdapterInitGetType(AdapterInit) != WIFI_ADAPTER_EXTENSIBLE_STATION) {
        return STATUS_NOT_SUPPORTED;
    }
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&Datapath, EvtCreateTxQueue, EvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(AdapterInit, &Datapath);
    Status = NetAdapterCreate(AdapterInit, WDF_NO_OBJECT_ATTRIBUTES, &Adapter);
    BcmTrace(L"WlanAdapterCreate", (ULONG)Status);
    if (!NT_SUCCESS(Status)) return Status;
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

    BcmTrace(L"WlanVersion", WLAN_VERSION);
    Status = BcmPrepareHardware(Translated);
    BcmTrace(L"WlanPrepareStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) return Status;
    BcmTrace(L"WlanChipReady", BcmIsReady());
    Status = WlanSetCapabilities(Device);
    if (!NT_SUCCESS(Status)) return Status;

    Ctx->Stop = 0;
    KeInitializeEvent(&Ctx->Wake, SynchronizationEvent, FALSE);
    KeInitializeSpinLock(&Ctx->Lock);
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, WlanThread, Ctx);
    if (!NT_SUCCESS(Status)) return Status;
    Status = ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&Ctx->Thread, NULL);
    ZwClose(Thread);
    return Status;
}

static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PWLAN_DEVICE Ctx = WlanGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);

    if (Ctx->Thread != NULL) {
        InterlockedExchange(&Ctx->Stop, 1);
        KeSetEvent(&Ctx->Wake, IO_NO_INCREMENT, FALSE);
        (VOID)KeWaitForSingleObject(Ctx->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Ctx->Thread);
        Ctx->Thread = NULL;
    }
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
