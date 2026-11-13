/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 shhenu
 * G435 0ADF protocol adapted from work by Oguz Kayra Bozkurt (2026, MIT).
 * See THIRD_PARTY_NOTICES.md for the upstream copyright and license.
 * Small Win32 tray application without C/C++ standard-library startup.
 */
#include <windows.h>
#include <shellapi.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include "g435_protocol.h"

#define WM_TRAY (WM_APP + 1)
#define WM_REFRESH (WM_APP + 2)
#define WM_RESULT (WM_APP + 3)
#define TIMER_REFRESH 1
#define MENU_REFRESH 1001
#define MENU_EXIT 1002
#define MENU_DIAGNOSTICS 1003
#define TIMER_DEVICE_CHANGE 2
#define APP_VERSION L"0.2.1"
#define POLL_INTERVAL (15UL * 60UL * 1000UL)
#define HID_REPORT_SIZE 65

/* Retain the compiler's stack-cookie checks without the CRT startup. */
void __cdecl __security_init_cookie(void);

typedef struct {
    BOOL ok;
    int millivolts;
    DWORD error;
    int transport;
    BOOL cleanupFailed;
} BATTERY_RESULT;

typedef CONFIGRET (WINAPI *CM_LIST_SIZE_FN)(PULONG, LPGUID, DEVINSTID_W, ULONG);
typedef CONFIGRET (WINAPI *CM_LIST_FN)(LPGUID, DEVINSTID_W, PZZWSTR, ULONG, ULONG);
typedef BOOLEAN (__stdcall *HID_ATTRIBUTES_FN)(HANDLE, PHIDD_ATTRIBUTES);
typedef BOOLEAN (__stdcall *HID_PREPARSED_FN)(HANDLE, PHIDP_PREPARSED_DATA *);
typedef BOOLEAN (__stdcall *HID_FREE_FN)(PHIDP_PREPARSED_DATA);
typedef NTSTATUS (__stdcall *HID_CAPS_FN)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
typedef BOOL (WINAPI *SHELL_NOTIFY_FN)(DWORD, PNOTIFYICONDATAW);
typedef BOOL (WINAPI *DISABLE_IME_FN)(DWORD);
typedef BOOLEAN (__stdcall *HID_FEATURE_FN)(HANDLE, PVOID, ULONG);
typedef BOOLEAN (__stdcall *HID_PRODUCT_FN)(HANDLE, PVOID, ULONG);

typedef struct {
    HANDLE device;
    OVERLAPPED readOperation;
    OVERLAPPED writeOperation;
    BYTE input[HID_REPORT_SIZE];
    BOOL readPending;
    BOOL sessionStarted;
    BYTE sequence;
    WORD inputLength;
    WORD outputLength;
    int transport;
    int bridgeMethod;
    HMODULE hidModule;
    HID_FEATURE_FN feature;
    HID_FEATURE_FN setFeature;
    G435_PI_PARSER parser;
} HID_SESSION;

static const GUID kHidGuid = {
    0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}
};
static const WCHAR kWindowClass[] = L"G435BatteryWidgetNative";
static const WCHAR kPollMutex[] = L"Local\\G435BatteryWidget.Poll";
static const WCHAR kTrayMutex[] = L"Local\\G435BatteryWidget.Tray";
static HWND gWindow;
static HICON gIcon;
static NOTIFYICONDATAW gNotify;
static HANDLE gCancel;
static HANDLE gWorker;
static BOOL gExiting;
static UINT gTaskbarCreated;
static BATTERY_RESULT gResult;
static HANDLE gDiagnosticOutput;
static BOOL WriteConsoleText(const WCHAR *text);
static DWORD PrintDiagnostics(void);
static DWORD SaveDiagnosticReport(void);

/* Explicit byte loops also keep compiler-generated CRT calls out of the binary. */
static void ZeroBytes(void *data, SIZE_T count) {
    volatile BYTE *bytes = (volatile BYTE *)data;
    while (count--) *bytes++ = 0;
}

static void CopyBytes(void *destination, const void *source, SIZE_T count) {
    volatile BYTE *out = (volatile BYTE *)destination;
    const BYTE *in = (const BYTE *)source;
    while (count--) *out++ = *in++;
}

static WCHAR LowerAscii(WCHAR value) {
    return (value >= L'A' && value <= L'Z') ? (WCHAR)(value + (L'a' - L'A')) : value;
}

static BOOL ContainsAscii(const WCHAR *text, const WCHAR *needle) {
    const WCHAR *a;
    const WCHAR *b;
    for (; *text; ++text) {
        a = text;
        b = needle;
        while (*a && *b && LowerAscii(*a) == LowerAscii(*b)) { ++a; ++b; }
        if (!*b) return TRUE;
    }
    return FALSE;
}

static void AppendWide(WCHAR *buffer, UINT capacity, UINT *used, const WCHAR *text) {
    while (*text && *used + 1 < capacity) buffer[(*used)++] = *text++;
    buffer[*used] = 0;
}

static void AppendNumber(WCHAR *buffer, UINT capacity, UINT *used, DWORD number) {
    WCHAR reverse[10];
    UINT count = 0;
    do { reverse[count++] = (WCHAR)(L'0' + number % 10); number /= 10; } while (number);
    while (count && *used + 1 < capacity) buffer[(*used)++] = reverse[--count];
    buffer[*used] = 0;
}

static void AppendHex(WCHAR *buffer, UINT capacity, UINT *used, DWORD number) {
    static const WCHAR digits[] = L"0123456789ABCDEF";
    int shift;
    for (shift = 12; shift >= 0 && *used + 1 < capacity; shift -= 4)
        buffer[(*used)++] = digits[(number >> shift) & 15];
    buffer[*used] = 0;
}

static int MillivoltsToPercent(int millivolts) {
    static const int volts[] = {4200, 4150, 4100, 4050, 4000, 3950, 3900, 3850,
                               3800, 3750, 3700, 3650, 3600, 3550, 3500, 3400};
    static const int percent[] = {100, 95, 90, 85, 80, 75, 70, 62,
                                 55, 45, 35, 25, 15, 10, 5, 0};
    UINT i;
    if (millivolts >= volts[0]) return 100;
    for (i = 0; i + 1 < ARRAYSIZE(volts); ++i) {
        if (millivolts >= volts[i + 1]) {
            int span = volts[i] - volts[i + 1];
            return percent[i + 1] + ((millivolts - volts[i + 1])
                * (percent[i] - percent[i + 1]) + span / 2) / span;
        }
    }
    return 0;
}

static WORD Crc16(const BYTE *data, UINT length) {
    WORD crc = 0;
    UINT i, bit;
    for (i = 0; i < length; ++i) {
        crc ^= (WORD)((WORD)data[i] << 8);
        for (bit = 0; bit < 8; ++bit)
            crc = (WORD)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

/* A zero report-ID byte precedes the 64-byte 0ADF payload on Windows. */
static void MakeFrame(BYTE output[HID_REPORT_SIZE], BYTE command,
                      const BYTE *body, UINT length) {
    WORD crc;
    ZeroBytes(output, HID_REPORT_SIZE);
    output[1] = command;
    output[2] = (BYTE)length;
    CopyBytes(output + 5, body, length);
    crc = Crc16(output + 1, 64);
    output[3] = (BYTE)crc;
    output[4] = (BYTE)(crc >> 8);
}

static BOOL IsCancelled(void) {
    return gCancel && WaitForSingleObject(gCancel, 0) == WAIT_OBJECT_0;
}

static void CancelOperation(HANDLE device, OVERLAPPED *operation) {
    DWORD transferred;
    CancelIoEx(device, operation);
    /* The driver must finish using the OVERLAPPED and buffer before reuse/free. */
    GetOverlappedResult(device, operation, &transferred, TRUE);
}

static BOOL BeginRead(HID_SESSION *session) {
    DWORD transferred;
    if (session->inputLength == 0) return TRUE;
    ResetEvent(session->readOperation.hEvent);
    if (!ReadFile(session->device, session->input, session->inputLength,
                  &transferred, &session->readOperation)
        && GetLastError() != ERROR_IO_PENDING) return FALSE;
    session->readPending = TRUE;
    return TRUE;
}

static BOOL WritePacket(HID_SESSION *session, const BYTE *packet, DWORD length,
                        BOOL allowCancel) {
    DWORD transferred = 0;
    DWORD wait;
    HANDLE events[2];
    if (allowCancel && IsCancelled()) { SetLastError(ERROR_CANCELLED); return FALSE; }
    ResetEvent(session->writeOperation.hEvent);
    if (!WriteFile(session->device, packet, length,
                   &transferred, &session->writeOperation)) {
        if (GetLastError() != ERROR_IO_PENDING) return FALSE;
        events[0] = session->writeOperation.hEvent;
        events[1] = gCancel;
        wait = WaitForMultipleObjects(allowCancel ? 2 : 1, events, FALSE, 1000);
        if (wait != WAIT_OBJECT_0) {
            CancelOperation(session->device, &session->writeOperation);
            SetLastError(wait == WAIT_OBJECT_0 + 1 ? ERROR_CANCELLED : ERROR_TIMEOUT);
            return FALSE;
        }
        if (!GetOverlappedResult(session->device, &session->writeOperation,
                                 &transferred, FALSE)) return FALSE;
    }
    if (transferred != length) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
    return TRUE;
}

static int ParseMillivolts(const BYTE *packet, DWORD length) {
    return G435ParseBatteryReport(packet, length);
}

static BOOL CollectReports(HID_SESSION *session, DWORD milliseconds, int *millivolts) {
    DWORD started = GetTickCount();
    HANDLE events[2];
    events[0] = session->readOperation.hEvent;
    events[1] = gCancel;
    for (;;) {
        DWORD elapsed = GetTickCount() - started;
        DWORD wait, transferred;
        if (elapsed >= milliseconds) return TRUE;
        wait = WaitForMultipleObjects(2, events, FALSE, milliseconds - elapsed);
        if (wait == WAIT_TIMEOUT) return TRUE;
        if (wait != WAIT_OBJECT_0) { SetLastError(ERROR_CANCELLED); return FALSE; }
        if (!GetOverlappedResult(session->device, &session->readOperation,
                                 &transferred, FALSE)) return FALSE;
        session->readPending = FALSE;
        if (millivolts) {
            int value = -1;
            if (transferred == 65) value = ParseMillivolts(session->input + 1, 64);
            else if (transferred == 64) value = ParseMillivolts(session->input, 64);
            if (value >= 0) *millivolts = value;
        }
        if (!BeginRead(session)) return FALSE;
    }
}

static BOOL Transact(HID_SESSION *session, BYTE command, const BYTE *body,
                     UINT length, DWORD wait, int *millivolts) {
    BYTE output[HID_REPORT_SIZE];
    if (length > 60) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    MakeFrame(output, command, body, length);
    return WritePacket(session, output, sizeof(output), TRUE)
        && CollectReports(session, wait, millivolts);
}

static BOOL SendPa(HID_SESSION *session, BYTE category, BYTE lengthField,
                   const BYTE *payload, UINT length, DWORD wait, int *millivolts) {
    BYTE body[60];
    if (length > 55) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    body[0] = 0x50;
    body[1] = 0x41;
    body[2] = category;
    body[3] = session->sequence;
    body[4] = lengthField;
    session->sequence = (BYTE)((session->sequence % 127) + 1);
    CopyBytes(body + 5, payload, length);
    return Transact(session, 0x80, body, length + 5, wait, millivolts);
}

static void ReportInterface(const HIDD_ATTRIBUTES *attr, const HIDP_CAPS *caps,
                            const WCHAR *product, int transport) {
    WCHAR line[240];
    UINT used = 0;
    AppendWide(line, ARRAYSIZE(line), &used, L"VID=");
    AppendHex(line, ARRAYSIZE(line), &used, attr->VendorID);
    AppendWide(line, ARRAYSIZE(line), &used, L" PID=");
    AppendHex(line, ARRAYSIZE(line), &used, attr->ProductID);
    AppendWide(line, ARRAYSIZE(line), &used, L" revision=");
    AppendHex(line, ARRAYSIZE(line), &used, attr->VersionNumber);
    AppendWide(line, ARRAYSIZE(line), &used, L" usage=");
    AppendHex(line, ARRAYSIZE(line), &used, caps->UsagePage);
    AppendWide(line, ARRAYSIZE(line), &used, L" input=");
    AppendNumber(line, ARRAYSIZE(line), &used, caps->InputReportByteLength);
    AppendWide(line, ARRAYSIZE(line), &used, L" output=");
    AppendNumber(line, ARRAYSIZE(line), &used, caps->OutputReportByteLength);
    AppendWide(line, ARRAYSIZE(line), &used, L" feature=");
    AppendNumber(line, ARRAYSIZE(line), &used, caps->FeatureReportByteLength);
    AppendWide(line, ARRAYSIZE(line), &used, L" transport=");
    AppendWide(line, ARRAYSIZE(line), &used,
        transport == G435_TRANSPORT_CRC64 ? L"CRC64"
        : transport == G435_TRANSPORT_BRIDGE6 ? L"BRIDGE6" : L"unsupported/non-control interface");
    AppendWide(line, ARRAYSIZE(line), &used, L"\r\n  Product: ");
    AppendWide(line, ARRAYSIZE(line), &used, product);
    AppendWide(line, ARRAYSIZE(line), &used, L"\r\n");
    WriteConsoleText(line);
}

/* Diagnostics uses metadata-only handles and never sends device commands. */
static HANDLE ScanDongles(HID_SESSION *session, BOOL diagnose) {
    HMODULE config = NULL, hid = NULL;
    CM_LIST_SIZE_FN listSize;
    CM_LIST_FN list;
    HID_ATTRIBUTES_FN attributes;
    HID_PREPARSED_FN preparsed;
    HID_FREE_FN freePreparsed;
    HID_CAPS_FN capabilities;
    HID_FEATURE_FN getFeature;
    HID_FEATURE_FN setFeature;
    HID_PRODUCT_FN getProduct;
    WCHAR *paths = NULL;
    HANDLE device = INVALID_HANDLE_VALUE;
    GUID guid;
    CONFIGRET status = CR_FAILURE;
    ULONG characters = 0;
    UINT attempt;
    DWORD error = ERROR_NOT_FOUND;
    DWORD openError = 0;
    UINT seen = 0, supported = 0;
    BOOL knownProduct = FALSE;

    config = LoadLibraryExW(L"cfgmgr32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!config) { error = GetLastError(); goto done; }
    hid = LoadLibraryExW(L"hid.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!hid) { error = GetLastError(); goto done; }
    listSize = (CM_LIST_SIZE_FN)GetProcAddress(config, "CM_Get_Device_Interface_List_SizeW");
    list = (CM_LIST_FN)GetProcAddress(config, "CM_Get_Device_Interface_ListW");
    attributes = (HID_ATTRIBUTES_FN)GetProcAddress(hid, "HidD_GetAttributes");
    preparsed = (HID_PREPARSED_FN)GetProcAddress(hid, "HidD_GetPreparsedData");
    freePreparsed = (HID_FREE_FN)GetProcAddress(hid, "HidD_FreePreparsedData");
    capabilities = (HID_CAPS_FN)GetProcAddress(hid, "HidP_GetCaps");
    getFeature = (HID_FEATURE_FN)GetProcAddress(hid, "HidD_GetFeature");
    setFeature = (HID_FEATURE_FN)GetProcAddress(hid, "HidD_SetFeature");
    getProduct = (HID_PRODUCT_FN)GetProcAddress(hid, "HidD_GetProductString");
    if (!listSize || !list || !attributes || !preparsed || !freePreparsed || !capabilities) {
        error = ERROR_PROC_NOT_FOUND;
        goto done;
    }
    CopyBytes(&guid, &kHidGuid, sizeof(guid));
    for (attempt = 0; attempt < 3; ++attempt) {
        if (listSize(&characters, &guid, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT)
            != CR_SUCCESS || characters < 2 || characters > 512 * 1024) {
            error = ERROR_GEN_FAILURE;
            goto done;
        }
        paths = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  (SIZE_T)characters * sizeof(WCHAR));
        if (!paths) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
        status = list(&guid, NULL, paths, characters, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (status != CR_BUFFER_SMALL) break;
        HeapFree(GetProcessHeap(), 0, paths);
        paths = NULL;
    }
    if (status == CR_SUCCESS && paths) {
        WCHAR *path = paths;
        const WCHAR *end = paths + characters;
        while (path < end && *path && !IsCancelled()) {
            WCHAR *next = path;
            while (next < end && *next) ++next;
            if (next >= end) break;
            if (diagnose || ContainsAscii(path, L"vid_046d")) {
                HANDLE probe = CreateFileW(path, 0,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED, NULL);
                if (probe != INVALID_HANDLE_VALUE) {
                    HIDD_ATTRIBUTES attr;
                    PHIDP_PREPARSED_DATA data = NULL;
                    HIDP_CAPS caps;
                    WCHAR product[96];
                    int transport = G435_TRANSPORT_NONE;
                    ZeroBytes(&attr, sizeof(attr));
                    ZeroBytes(&caps, sizeof(caps));
                    ZeroBytes(product, sizeof(product));
                    attr.Size = sizeof(attr);
                    if (attributes(probe, &attr)) {
                        if (getProduct && (diagnose || (attr.VendorID == 0x046d
                            && attr.ProductID != 0x0adf && attr.ProductID != 0x0acb)))
                            getProduct(probe, product, sizeof(product));
                        product[ARRAYSIZE(product) - 1] = 0;
                        if (preparsed(probe, &data)) {
                            if (capabilities(data, &caps) == HIDP_STATUS_SUCCESS)
                                transport = G435SelectTransport(attr.VendorID, attr.ProductID,
                                    caps.UsagePage, caps.InputReportByteLength,
                                    caps.OutputReportByteLength, caps.FeatureReportByteLength);
                            freePreparsed(data);
                        }
                        if (attr.VendorID == 0x046d || ContainsAscii(product, L"G435")) {
                            ++seen;
                            if (diagnose) ReportInterface(&attr, &caps, product, transport);
                        }
                        if (ContainsAscii(product, L"G435") || (attr.VendorID == 0x046d
                            && (attr.ProductID == 0x0adf || attr.ProductID == 0x0acb))) knownProduct = TRUE;
                        if (transport != G435_TRANSPORT_NONE) {
                            ++supported;
                            if (!diagnose) {
                                CloseHandle(probe);
                                probe = INVALID_HANDLE_VALUE;
                                device = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED, NULL);
                                if (device != INVALID_HANDLE_VALUE) {
                                    session->inputLength = caps.InputReportByteLength;
                                    session->outputLength = caps.OutputReportByteLength;
                                    session->transport = transport;
                                    session->feature = getFeature;
                                    session->setFeature = setFeature;
                                    session->hidModule = hid;
                                    hid = NULL;
                                    break;
                                }
                                openError = GetLastError();
                            }
                        }
                    }
                    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
                } else if (ContainsAscii(path, L"vid_046d&pid_0adf")
                           || ContainsAscii(path, L"vid_046d&pid_0acb")) openError = GetLastError();
            }
            path = next + 1;
        }
    }
    if (status != CR_SUCCESS) error = ERROR_GEN_FAILURE;
    else if (openError) error = openError;
    else if (knownProduct) error = ERROR_NOT_SUPPORTED;
done:
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    if (hid) FreeLibrary(hid);
    if (config) FreeLibrary(config);
    if (diagnose) {
        WCHAR summary[160];
        UINT used = 0;
        AppendWide(summary, ARRAYSIZE(summary), &used, L"\r\nRelevant interfaces: ");
        AppendNumber(summary, ARRAYSIZE(summary), &used, seen);
        AppendWide(summary, ARRAYSIZE(summary), &used, L"; supported control interfaces: ");
        AppendNumber(summary, ARRAYSIZE(summary), &used, supported);
        AppendWide(summary, ARRAYSIZE(summary), &used, L"\r\n");
        if (!supported) {
            AppendWide(summary, ARRAYSIZE(summary), &used, L"Discovery Windows error: ");
            AppendNumber(summary, ARRAYSIZE(summary), &used, error);
            AppendWide(summary, ARRAYSIZE(summary), &used, L"\r\n");
        }
        WriteConsoleText(summary);
    }
    if (device == INVALID_HANDLE_VALUE) SetLastError(IsCancelled() ? ERROR_CANCELLED : error);
    return device;
}

static int TakeBridgeInput(HID_SESSION *session, BOOL *ack) {
    DWORD length;
    *ack = FALSE;
    if (session->inputLength == 0) return 0;
    if (!session->readPending && !BeginRead(session)) return -1;
    if (WaitForSingleObject(session->readOperation.hEvent, 0) != WAIT_OBJECT_0) return 0;
    session->readPending = FALSE;
    if (!GetOverlappedResult(session->device, &session->readOperation, &length, FALSE)) return -1;
    *ack = G435IsBridgeAck(session->input, length);
    if (!BeginRead(session)) return -1;
    return 1;
}

static int BridgeWrite(void *context, const G435_BYTE *packet, G435_UINT length, int allowCancel) {
    HID_SESSION *session = (HID_SESSION *)context;
    int count;
    BOOL ack;
    /* Do not mistake an ACK from an earlier command for the bridge-exit ACK. */
    for (count = 0; count < 64; ++count) {
        int ready = TakeBridgeInput(session, &ack);
        if (ready < 0 && allowCancel) return 0;
        if (ready <= 0) break;
    }
    if (allowCancel && IsCancelled()) { SetLastError(ERROR_CANCELLED); return 0; }
    if (session->bridgeMethod == G435_BRIDGE_FEATURE64) {
        BYTE feature[64];
        if (!session->setFeature) { SetLastError(ERROR_PROC_NOT_FOUND); return 0; }
        if (!G435BridgeFeaturePacket(feature, packet)) { SetLastError(ERROR_INVALID_DATA); return 0; }
        return session->setFeature(session->device, feature, sizeof(feature));
    }
    if (session->outputLength != G435_BRIDGE_OUTPUT_SIZE) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    return WritePacket(session, packet, length, allowCancel);
}

static BOOL ReadBridgeFeature(HID_SESSION *session, BYTE report[64]) {
    ZeroBytes(report, 64);
    report[0] = 6;
    return session->feature && session->feature(session->device, report, 64);
}

static int BridgeAck(void *context, int allowCancel) {
    HID_SESSION *session = (HID_SESSION *)context;
    DWORD started = GetTickCount();
    HANDLE events[2];
    events[0] = session->readOperation.hEvent;
    events[1] = gCancel;
    while (GetTickCount() - started < 1000) {
        BYTE feature[64];
        BOOL ack;
        int ready;
        if (allowCancel && IsCancelled()) { SetLastError(ERROR_CANCELLED); return 0; }
        ready = TakeBridgeInput(session, &ack);
        if (ready < 0) return 0;
        if (ready > 0 && ack) return 1;
        if (ReadBridgeFeature(session, feature)) {
            if (G435IsBridgeAck(feature, sizeof(feature))) return 1;
            if (session->bridgeMethod == G435_BRIDGE_FEATURE64)
                G435FeedRawFeature(&session->parser, feature);
            else G435FeedFeature(&session->parser, feature);
        }
        WaitForMultipleObjects(allowCancel ? 2 : 1, events, FALSE, 20);
    }
    SetLastError(ERROR_TIMEOUT);
    return 0;
}

static int BridgeReply(void *context, int kind, int *value) {
    HID_SESSION *session = (HID_SESSION *)context;
    DWORD started = GetTickCount();
    while (GetTickCount() - started < 2000) {
        BYTE feature[64];
        int reply;
        if (IsCancelled()) { SetLastError(ERROR_CANCELLED); return 0; }
        reply = G435TakeReply(&session->parser, kind, value);
        if (reply > 0) { SetLastError(ERROR_SUCCESS); return 1; }
        if (reply < 0) { SetLastError(ERROR_INVALID_DATA); return 0; }
        if (ReadBridgeFeature(session, feature) && !(session->bridgeMethod == G435_BRIDGE_FEATURE64
            ? G435FeedRawFeature(&session->parser, feature) : G435FeedFeature(&session->parser, feature))) {
            SetLastError(ERROR_INVALID_DATA);
            return 0;
        }
        if (WaitForSingleObject(gCancel, 10) == WAIT_OBJECT_0) {
            SetLastError(ERROR_CANCELLED);
            return 0;
        }
    }
    SetLastError(ERROR_TIMEOUT);
    return 0;
}

static int BridgePause(void *context, G435_UINT milliseconds) {
    UNREFERENCED_PARAMETER(context);
    if (WaitForSingleObject(gCancel, milliseconds) == WAIT_OBJECT_0) {
        SetLastError(ERROR_CANCELLED);
        return 0;
    }
    return 1;
}

static unsigned long BridgeError(void *context) {
    UNREFERENCED_PARAMETER(context);
    return GetLastError();
}

static BATTERY_RESULT ReadBattery(void) {
    static const BYTE enable[] = {1};
    static const BYTE disable[] = {2};
    static const BYTE init[] = {0x50, 0x41, 0x06, 0x01, 0x0d, 0x00, 0x25,
        0x34, 0x12, 0x5a, 0x5a, 0x01, 0, 0, 0, 0xf0, 0, 0, 0};
    static const BYTE getMode[] = {0xe0};
    static const BYTE setMode[] = {0xe1, 1};
    static const BYTE battery[] = {0, 0x31};
    HID_SESSION session;
    BATTERY_RESULT result;
    HANDLE pollMutex;
    DWORD wait;

    ZeroBytes(&session, sizeof(session));
    ZeroBytes(&result, sizeof(result));
    result.millivolts = -1;
    session.device = INVALID_HANDLE_VALUE;
    pollMutex = CreateMutexW(NULL, FALSE, kPollMutex);
    if (!pollMutex) { result.error = GetLastError(); return result; }
    wait = WaitForSingleObject(pollMutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        result.error = ERROR_BUSY;
        CloseHandle(pollMutex);
        return result;
    }

    session.device = ScanDongles(&session, FALSE);
    if (session.device == INVALID_HANDLE_VALUE) { result.error = GetLastError(); goto done; }
    result.transport = session.transport;
    session.readOperation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    session.writeOperation.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!session.readOperation.hEvent || !session.writeOperation.hEvent || !BeginRead(&session)) {
        result.error = GetLastError();
        goto done;
    }
    if (session.transport == G435_TRANSPORT_BRIDGE6) {
        G435_BRIDGE_IO io;
        G435_BRIDGE_RESULT bridge;
        if (!session.feature) { result.error = ERROR_PROC_NOT_FOUND; goto done; }
        io.context = &session;
        io.write = BridgeWrite;
        io.ack = BridgeAck;
        io.reply = BridgeReply;
        io.pause = BridgePause;
        io.lastError = BridgeError;
        session.bridgeMethod = session.setFeature ? G435_BRIDGE_FEATURE64 : G435_BRIDGE_OUTPUT515;
        bridge = G435RunBridge(&io);
        /* Never try another wire format until the previous bridge session is
           confirmed closed, and never restart a cancelled request. */
        if (!bridge.ok && bridge.cleanupSucceeded && !IsCancelled()
            && session.bridgeMethod == G435_BRIDGE_FEATURE64
            && session.outputLength == G435_BRIDGE_OUTPUT_SIZE) {
            session.bridgeMethod = G435_BRIDGE_OUTPUT515;
            session.parser.length = 0;
            bridge = G435RunBridge(&io);
        }
        result.ok = bridge.ok;
        result.millivolts = bridge.millivolts;
        result.cleanupFailed = !bridge.cleanupSucceeded;
        if (!bridge.ok) result.error = bridge.error ? bridge.error : ERROR_TIMEOUT;
        goto done;
    }
    session.sequence = 1;
    if (!Transact(&session, 0x84, enable, 1, 50, NULL)) goto io_error;
    session.sessionStarted = TRUE;
    if (!Transact(&session, 0x85, enable, 1, 50, NULL)
        || !Transact(&session, 0x80, init, sizeof(init), 450, NULL)) goto io_error;
    session.sequence = 2;
    if (!SendPa(&session, 0x0e, 1, getMode, 1, 120, NULL)
        || !SendPa(&session, 0x0e, 2, setMode, 2, 120, NULL)
        || !SendPa(&session, 0x0e, 1, getMode, 1, 120, NULL)
        || !SendPa(&session, 0x06, 1, battery, 2, 250, &result.millivolts)) goto io_error;
    result.ok = result.millivolts >= 0;
    if (!result.ok) result.error = ERROR_TIMEOUT;
    goto done;
io_error:
    result.error = GetLastError();
done:
    if (session.device != INVALID_HANDLE_VALUE) {
        if (session.sessionStarted) {
            BYTE output[HID_REPORT_SIZE];
            MakeFrame(output, 0x85, disable, 1);
            /* End the session even when a request was cancelled or failed. */
            WritePacket(&session, output, sizeof(output), FALSE);
        }
        if (session.readPending) CancelOperation(session.device, &session.readOperation);
        CloseHandle(session.device);
    }
    if (session.readOperation.hEvent) CloseHandle(session.readOperation.hEvent);
    if (session.writeOperation.hEvent) CloseHandle(session.writeOperation.hEvent);
    if (session.hidModule) FreeLibrary(session.hidModule);
    ReleaseMutex(pollMutex);
    CloseHandle(pollMutex);
    return result;
}

static BOOL ShellNotify(DWORD operation) {
    BOOL result = FALSE;
    HMODULE shell = LoadLibraryExW(L"shell32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (shell) {
        SHELL_NOTIFY_FN notify = (SHELL_NOTIFY_FN)GetProcAddress(shell, "Shell_NotifyIconW");
        if (notify) result = notify(operation, &gNotify);
        FreeLibrary(shell);
    }
    return result;
}

static void DisableUnusedTextInput(void) {
    /* This application has no edit controls. Avoid a per-window IME/TSF context. */
    HMODULE input = LoadLibraryExW(L"imm32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (input) {
        DISABLE_IME_FN disable = (DISABLE_IME_FN)GetProcAddress(input, "ImmDisableIME");
        if (disable) disable(GetCurrentThreadId());
        FreeLibrary(input);
    }
}

static HICON MakeBatteryIcon(int percent) {
    BITMAPINFO bitmapInfo;
    ICONINFO iconInfo;
    HBITMAP bitmap, mask;
    HICON icon;
    DWORD *pixels = NULL;
    DWORD color;
    BYTE maskBits[32];
    int x, y, fill;
    ZeroBytes(&bitmapInfo, sizeof(bitmapInfo));
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = 16;
    bitmapInfo.bmiHeader.biHeight = -16;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(NULL, &bitmapInfo, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    if (!bitmap) return CopyIcon(LoadIconW(NULL, IDI_APPLICATION));
    color = percent < 0 ? 0xff8c8c8c : percent <= 20 ? 0xffdc4646
        : percent <= 50 ? 0xffdca028 : 0xff46be50;
    fill = percent < 0 ? 10 : (percent * 10 + 50) / 100;
    for (y = 0; y < 16; ++y) {
        for (x = 0; x < 16; ++x) {
            BOOL outline = x >= 1 && x <= 12 && y >= 4 && y <= 11
                && (x == 1 || x == 12 || y == 4 || y == 11);
            BOOL tip = x >= 13 && x <= 14 && y >= 6 && y <= 9;
            BOOL inside = x >= 2 && x <= 11 && y >= 5 && y <= 10 && x - 2 < fill;
            pixels[y * 16 + x] = outline || tip ? 0xffffffff : inside ? color : 0;
        }
    }
    ZeroBytes(maskBits, sizeof(maskBits));
    mask = CreateBitmap(16, 16, 1, 1, maskBits);
    ZeroBytes(&iconInfo, sizeof(iconInfo));
    iconInfo.fIcon = TRUE;
    iconInfo.hbmMask = mask;
    iconInfo.hbmColor = bitmap;
    icon = mask ? CreateIconIndirect(&iconInfo) : NULL;
    if (mask) DeleteObject(mask);
    DeleteObject(bitmap);
    return icon ? icon : CopyIcon(LoadIconW(NULL, IDI_APPLICATION));
}

static void ApplyResult(void) {
    UINT used = 0;
    int percent = gResult.ok ? MillivoltsToPercent(gResult.millivolts) : -1;
    HICON previous = gIcon;
    HICON icon = MakeBatteryIcon(percent);
    if (icon) gIcon = icon;
    gNotify.hIcon = gIcon;
    AppendWide(gNotify.szTip, ARRAYSIZE(gNotify.szTip), &used, L"G435: ");
    if (gResult.ok) {
        AppendNumber(gNotify.szTip, ARRAYSIZE(gNotify.szTip), &used, (DWORD)percent);
        AppendWide(gNotify.szTip, ARRAYSIZE(gNotify.szTip), &used, L"% (");
        AppendNumber(gNotify.szTip, ARRAYSIZE(gNotify.szTip), &used, (DWORD)gResult.millivolts);
        AppendWide(gNotify.szTip, ARRAYSIZE(gNotify.szTip), &used, L" мВ)");
    } else {
        AppendWide(gNotify.szTip, ARRAYSIZE(gNotify.szTip), &used,
            gResult.cleanupFailed ? L"переподключи свисток: не завершён служебный режим"
            : gResult.error == ERROR_NOT_FOUND ? L"свисток не найден; меню «Диагностика»"
            : gResult.error == ERROR_NOT_SUPPORTED ? L"другой HID-интерфейс; меню «Диагностика»"
            : gResult.error == ERROR_ACCESS_DENIED ? L"нет доступа к USB; меню «Диагностика»"
            : gResult.error == ERROR_BUSY ? L"идёт другой запрос" : L"нет ответа от наушников");
        AppendWide(gNotify.szTip, ARRAYSIZE(gNotify.szTip), &used, L" (код ");
        AppendNumber(gNotify.szTip, ARRAYSIZE(gNotify.szTip), &used, gResult.error);
        AppendWide(gNotify.szTip, ARRAYSIZE(gNotify.szTip), &used, L")");
    }
    ShellNotify(NIM_MODIFY);
    if (icon && previous) DestroyIcon(previous);
}

static DWORD WINAPI ReadWorker(void *unused) {
    UNREFERENCED_PARAMETER(unused);
    gResult = ReadBattery();
    PostMessageW(gWindow, WM_RESULT, 0, 0);
    return 0;
}

static void Refresh(void) {
    if (gWorker || gExiting) return;
    ResetEvent(gCancel);
    gWorker = CreateThread(NULL, 64 * 1024, ReadWorker, NULL,
                           STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
}

static void ExitWidget(HWND window) {
    gExiting = TRUE;
    KillTimer(window, TIMER_REFRESH);
    KillTimer(window, TIMER_DEVICE_CHANGE);
    SetEvent(gCancel);
    if (!gWorker) DestroyWindow(window);
}

static void ShowMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    POINT point;
    UINT command;
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | (gWorker ? MF_GRAYED : 0), MENU_REFRESH, L"Обновить");
    AppendMenuW(menu, MF_STRING, MENU_DIAGNOSTICS, L"Диагностика");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_EXIT, L"Выйти");
    GetCursorPos(&point);
    SetForegroundWindow(window);
    command = (UINT)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        point.x, point.y, 0, window, NULL);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
    if (command) PostMessageW(window, WM_COMMAND, command, 0);
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (gTaskbarCreated && message == gTaskbarCreated) { ShellNotify(NIM_ADD); return 0; }
    switch (message) {
    case WM_TRAY:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowMenu(window);
        else if (lParam == WM_LBUTTONDBLCLK) Refresh();
        return 0;
    case WM_REFRESH:
        Refresh();
        return 0;
    case WM_TIMER:
        if (wParam == TIMER_REFRESH) Refresh();
        else if (wParam == TIMER_DEVICE_CHANGE) { KillTimer(window, TIMER_DEVICE_CHANGE); Refresh(); }
        return 0;
    case WM_DEVICECHANGE:
        /* Retry a previously missing dongle. Do not let bridge-induced device
           notifications create a feedback loop of battery reads/audio drops. */
        if (wParam == 7 && !gWorker && (gResult.error == ERROR_NOT_FOUND
            || gResult.error == ERROR_NOT_SUPPORTED))
            SetTimer(window, TIMER_DEVICE_CHANGE, 1500, NULL); /* DBT_DEVNODES_CHANGED */
        return 0;
    case WM_RESULT:
        if (gWorker) {
            WaitForSingleObject(gWorker, INFINITE);
            CloseHandle(gWorker);
            gWorker = NULL;
        }
        if (gExiting) DestroyWindow(window);
        else ApplyResult();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == MENU_REFRESH) Refresh();
        else if (LOWORD(wParam) == MENU_EXIT) ExitWidget(window);
        else if (LOWORD(wParam) == MENU_DIAGNOSTICS) SaveDiagnosticReport();
        return 0;
    case WM_CLOSE:
        ExitWidget(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, TIMER_REFRESH);
        KillTimer(window, TIMER_DEVICE_CHANGE);
        ShellNotify(NIM_DELETE);
        if (gIcon) { DestroyIcon(gIcon); gIcon = NULL; }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

/* Parse complete command-line tokens without pulling in Shell32's argv helper. */
static BOOL HasArgument(const WCHAR *argument) {
    const WCHAR *text = GetCommandLineW();
    while (*text) {
        const WCHAR *start;
        UINT length = 0, i = 0;
        BOOL quoted = FALSE;
        while (*text == L' ' || *text == L'\t') ++text;
        if (*text == L'"') { quoted = TRUE; ++text; }
        start = text;
        while (*text && (quoted ? *text != L'"' : *text != L' ' && *text != L'\t')) {
            ++text;
            ++length;
        }
        while (argument[i] && i < length && argument[i] == start[i]) ++i;
        if (i == length && !argument[i]) return TRUE;
        if (*text == L'"') ++text;
    }
    return FALSE;
}

static BOOL WriteConsoleText(const WCHAR *text) {
    HANDLE output = gDiagnosticOutput ? gDiagnosticOutput : GetStdHandle(STD_OUTPUT_HANDLE);
    CHAR bytes[1024];
    DWORD written;
    int length;
    if (!output || output == INVALID_HANDLE_VALUE) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) return FALSE;
        output = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    length = WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes, sizeof(bytes), NULL, NULL);
    return length > 0 && WriteFile(output, bytes, (DWORD)(length - 1), &written, NULL);
}

static DWORD PrintDiagnostics(void) {
    WriteConsoleText(L"G435 Battery Widget " APP_VERSION L"\r\n"
#ifdef _WIN64
        L"Architecture: x64\r\n"
#else
        L"Architecture: x86\r\n"
#endif
        L"Supported LIGHTSPEED dongles: 046D:0ADF (CRC64), 046D:0ACB (BRIDGE6).\r\n"
        L"Read-only metadata scan: no battery/session commands are sent.\r\n"
        L"Serial numbers and device paths are not included.\r\n\r\n");
    ScanDongles(NULL, TRUE);
    WriteConsoleText(L"\r\nIf an unknown G435 control interface is listed, share this report.\r\n"
        L"For 0ACB, a normal battery read briefly interrupts audio.\r\n");
    return 0;
}

static DWORD SaveDiagnosticReport(void) {
    WCHAR path[MAX_PATH], notepad[MAX_PATH], command[2 * MAX_PATH + 8];
    DWORD length = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    UINT used;
    HANDLE file;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    if (!length || length >= ARRAYSIZE(path)) return 1;
    while (length && path[length - 1] != L'\\' && path[length - 1] != L'/') --length;
    if (length + 21 >= ARRAYSIZE(path)) return 1;
    used = length;
    AppendWide(path, ARRAYSIZE(path), &used, L"G435-diagnostics.txt");
    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        MessageBoxW(gWindow, L"Не удалось сохранить отчёт. Распакуй виджет в папку с доступом на запись.",
                    L"G435: диагностика", MB_OK | MB_ICONERROR);
        return 1;
    }
    gDiagnosticOutput = file;
    PrintDiagnostics();
    gDiagnosticOutput = NULL;
    CloseHandle(file);
    length = GetSystemDirectoryW(notepad, ARRAYSIZE(notepad));
    if (!length || length + 13 >= ARRAYSIZE(notepad)) return 0;
    used = length;
    AppendWide(notepad, ARRAYSIZE(notepad), &used, L"\\notepad.exe");
    used = 0;
    AppendWide(command, ARRAYSIZE(command), &used, L"\"");
    AppendWide(command, ARRAYSIZE(command), &used, notepad);
    AppendWide(command, ARRAYSIZE(command), &used, L"\" \"");
    AppendWide(command, ARRAYSIZE(command), &used, path);
    AppendWide(command, ARRAYSIZE(command), &used, L"\"");
    ZeroBytes(&startup, sizeof(startup));
    ZeroBytes(&process, sizeof(process));
    startup.cb = sizeof(startup);
    if (CreateProcessW(notepad, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    return 0;
}

static DWORD PrintReading(void) {
    WCHAR text[128];
    UINT used = 0;
    BATTERY_RESULT result = ReadBattery();
    BOOL json = HasArgument(L"--json");
    if (result.ok) {
        AppendWide(text, ARRAYSIZE(text), &used, json ? L"{\"percent\":" : L"G435: ");
        AppendNumber(text, ARRAYSIZE(text), &used, (DWORD)MillivoltsToPercent(result.millivolts));
        AppendWide(text, ARRAYSIZE(text), &used, json ? L",\"millivolts\":" : L"% (");
        AppendNumber(text, ARRAYSIZE(text), &used, (DWORD)result.millivolts);
        if (json) {
            AppendWide(text, ARRAYSIZE(text), &used, L",\"transport\":\"");
            AppendWide(text, ARRAYSIZE(text), &used,
                result.transport == G435_TRANSPORT_BRIDGE6 ? L"BRIDGE6" : L"CRC64");
            AppendWide(text, ARRAYSIZE(text), &used, L"\"}\r\n");
        } else AppendWide(text, ARRAYSIZE(text), &used, L" mV)\r\n");
    } else if (json) {
        AppendWide(text, ARRAYSIZE(text), &used, L"{\"error\":");
        AppendNumber(text, ARRAYSIZE(text), &used, result.error);
        AppendWide(text, ARRAYSIZE(text), &used, L",\"cleanup_failed\":");
        AppendWide(text, ARRAYSIZE(text), &used, result.cleanupFailed ? L"true" : L"false");
        AppendWide(text, ARRAYSIZE(text), &used, L"}\r\n");
    } else {
        AppendWide(text, ARRAYSIZE(text), &used, L"G435: battery read failed (Windows error ");
        AppendNumber(text, ARRAYSIZE(text), &used, result.error);
        AppendWide(text, ARRAYSIZE(text), &used, L")\r\n");
    }
    WriteConsoleText(text);
    return result.ok ? 0 : 1;
}

static DWORD RunWidget(void) {
    HANDLE singleton;
    WNDCLASSEXW windowClass;
    MSG message;
    int status;
    DWORD exitCode = 0;
    UINT used = 0;
    if (HasArgument(L"--diagnose")) return PrintDiagnostics();
    if (HasArgument(L"--report")) return SaveDiagnosticReport();
    if (HasArgument(L"--quit") || HasArgument(L"--refresh")) {
        HWND existing = FindWindowW(kWindowClass, L"G435 Battery Widget");
        if (existing) PostMessageW(existing, HasArgument(L"--quit") ? WM_CLOSE : WM_REFRESH, 0, 0);
        return existing ? 0 : 1;
    }
    gCancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!gCancel) return 1;
    if (HasArgument(L"--once")) {
        exitCode = PrintReading();
        CloseHandle(gCancel);
        return exitCode;
    }
    singleton = CreateMutexW(NULL, FALSE, kTrayMutex);
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleton) CloseHandle(singleton);
        CloseHandle(gCancel);
        return 0;
    }
    ZeroBytes(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = GetModuleHandleW(NULL);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) { exitCode = 1; goto done; }
    DisableUnusedTextInput();
    gWindow = CreateWindowExW(0, kWindowClass, L"G435 Battery Widget",
        WS_OVERLAPPED, 0, 0, 1, 1, NULL, NULL, windowClass.hInstance, NULL);
    if (!gWindow) { exitCode = 1; goto done; }
    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    gIcon = MakeBatteryIcon(-1);
    gNotify.cbSize = sizeof(gNotify);
    gNotify.hWnd = gWindow;
    gNotify.uID = 1;
    gNotify.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    gNotify.uCallbackMessage = WM_TRAY;
    gNotify.hIcon = gIcon;
    AppendWide(gNotify.szTip, ARRAYSIZE(gNotify.szTip), &used, L"G435: проверка заряда");
    ShellNotify(NIM_ADD);
    SetTimer(gWindow, TIMER_REFRESH, POLL_INTERVAL, NULL);
    PostMessageW(gWindow, WM_REFRESH, 0, 0);
    while ((status = GetMessageW(&message, NULL, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (status < 0) exitCode = 1;
done:
    CloseHandle(singleton);
    CloseHandle(gCancel);
    return exitCode;
}

/* Initialize /GS before calling any function containing protected stack buffers. */
__declspec(safebuffers) void __cdecl G435Entry(void) {
    __security_init_cookie();
    ExitProcess(RunWidget());
}
