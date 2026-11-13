/* SPDX-License-Identifier: MIT
 * G435 transport framing and bounded PI parser. No platform or runtime dependency.
 */
#ifndef G435_PROTOCOL_H
#define G435_PROTOCOL_H

#define G435_TRANSPORT_NONE 0
#define G435_TRANSPORT_CRC64 1
#define G435_TRANSPORT_BRIDGE6 2
#define G435_BRIDGE_FEATURE64 1
#define G435_BRIDGE_OUTPUT515 2
#define G435_BRIDGE_OUTPUT_SIZE 515
#define G435_PI_BUFFER_SIZE 512
#define G435_REPLY_MODE 1
#define G435_REPLY_BATTERY 2

typedef unsigned char G435_BYTE;
typedef unsigned int G435_UINT;

typedef struct {
    G435_BYTE bytes[G435_PI_BUFFER_SIZE];
    G435_UINT length;
} G435_PI_PARSER;

typedef struct {
    void *context;
    int (*write)(void *, const G435_BYTE *, G435_UINT, int);
    int (*ack)(void *, int);
    int (*reply)(void *, int, int *);
    int (*pause)(void *, G435_UINT);
    unsigned long (*lastError)(void *);
} G435_BRIDGE_IO;

typedef struct {
    int ok;
    int millivolts;
    int cleanupSucceeded;
    unsigned long error;
} G435_BRIDGE_RESULT;

typedef struct {
    int ok;
    int millivolts;
    unsigned long error;
    int transport;
    int cleanupFailed;
} G435_READING;

typedef struct {
    void *context;
    G435_READING (*candidate)(void *, G435_UINT);
    int (*cancelled)(void *);
} G435_READER_IO;

static G435_READING G435ReadAvailable(const G435_READER_IO *io) {
    G435_READING result;
    G435_UINT index;
    result.ok = 0; result.millivolts = -1; result.error = 0;
    result.transport = G435_TRANSPORT_NONE; result.cleanupFailed = 0;
    for (index = 0; index < 8 && !io->cancelled(io->context); ++index) {
        G435_READING current = io->candidate(io->context, index);
        if (index && !current.transport) break;
        result = current;
        if (current.ok || current.cleanupFailed || !current.transport || io->cancelled(io->context)) break;
    }
    return result;
}

/* PI header bytes 4..7 are opaque counters/timestamps and may contain any value.
   Locate and validate the complete OTA reply instead of matching a preceding 00. */
static int G435ParseBatteryForSequence(const G435_BYTE *bytes, G435_UINT length, int sequence) {
    G435_UINT offset;
    for (offset = 0; offset + 15 <= length; ++offset) {
        const G435_BYTE *frame = bytes + offset;
        G435_UINT payloadLength;
        int millivolts;
        if (frame[0] != 0x50 || frame[1] != 0x49 || (frame[2] != 1 && frame[2] != 6)) continue;
        payloadLength = frame[8] + ((G435_UINT)frame[9] << 8);
        if (payloadLength < 5 || payloadLength > length - offset - 10) continue;
        if (frame[10] != 6 || !(frame[11] & 0x80) || frame[12] != 0) continue;
        if (sequence >= 0 && (frame[11] & 0x7f) != (sequence & 0x7f)) continue;
        millivolts = frame[13] | ((int)frame[14] << 8);
        if (millivolts >= 2500 && millivolts <= 4500) return millivolts;
    }
    return -1;
}

static int G435ParseBatteryReport(const G435_BYTE *bytes, G435_UINT length) {
    return G435ParseBatteryForSequence(bytes, length, -1);
}

static void G435Copy(volatile G435_BYTE *out, const G435_BYTE *in, G435_UINT count) {
    while (count--) *out++ = *in++;
}

static int G435SelectTransport(unsigned short vendor, unsigned short product,
        unsigned short usage, unsigned short input, unsigned short output,
        unsigned short feature) {
    if (vendor != 0x046d) return G435_TRANSPORT_NONE;
    if (product == 0x0adf && usage == 0xff00 && input == 65 && output == 65)
        return G435_TRANSPORT_CRC64;
    if (product == 0x0acb && usage == 0xffc0 && (input == 0 || (input >= 2 && input <= 65))
        && feature == 64) return G435_TRANSPORT_BRIDGE6;
    return G435_TRANSPORT_NONE;
}

static unsigned short G435LowerAscii(unsigned short value) {
    return value >= 'A' && value <= 'Z' ? (unsigned short)(value + ('a' - 'A')) : value;
}

static int G435IsModelWord(unsigned short value) {
    value = G435LowerAscii(value);
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
}

/* USB product strings are UTF-16. Require the actual G435/G435SE model token,
   not a substring of an unrelated model such as G4350 or a generic G headset. */
static int G435IsModelName(const unsigned short *name) {
    G435_UINT i;
    if (!name) return 0;
    for (i = 0; name[i]; ++i) {
        const unsigned short *token = name + i;
        G435_UINT end;
        if (i && G435IsModelWord(name[i - 1])) continue;
        if (G435LowerAscii(token[0]) != 'g' || token[1] != '4'
            || token[2] != '3' || token[3] != '5') continue;
        end = 4;
        if (G435LowerAscii(token[end]) == 's' && G435LowerAscii(token[end + 1]) == 'e') end += 2;
        if (!G435IsModelWord(token[end])) return 1;
    }
    return 0;
}

/* New USB IDs need no user-supplied code when BOTH the Logitech G435 identity
   and an already implemented control-interface fingerprint are present. */
static int G435SelectNamedTransport(unsigned short vendor, unsigned short product,
        unsigned short usage, unsigned short input, unsigned short output,
        unsigned short feature, const unsigned short *name) {
    int known = G435SelectTransport(vendor, product, usage, input, output, feature);
    if (known || vendor != 0x046d || !G435IsModelName(name)) return known;
    if (usage == 0xff00 && input == 65 && output == 65) return G435_TRANSPORT_CRC64;
    if (usage == 0xffc0 && (input == 0 || (input >= 2 && input <= 65)) && feature == 64)
        return G435_TRANSPORT_BRIDGE6;
    return G435_TRANSPORT_NONE;
}

/* A healthy widget sleeps for 15 minutes. An unavailable headset recovers
   without manual refresh; failed bridge cleanup requires a physical reconnect. */
static G435_UINT G435NextPollDelay(int ok, int cleanupFailed, G435_UINT failures) {
    if (cleanupFailed) return 0;
    if (ok) return 15U * 60U * 1000U;
    if (failures <= 1) return 5000;
    if (failures == 2) return 15000;
    return 60000;
}

static void G435MakeBridgeControl(G435_BYTE *packet, G435_BYTE command) {
    G435_UINT i;
    volatile G435_BYTE *zero = packet;
    for (i = 0; i < G435_BRIDGE_OUTPUT_SIZE; ++i) zero[i] = 0;
    packet[0] = 6;
    packet[1] = command;
    packet[2] = 3;
}

static int G435MakeBridgePa(G435_BYTE *packet, G435_BYTE category, G435_BYTE sequence,
        const G435_BYTE *payload, G435_UINT length, int ota) {
    G435_UINT size = length + 5;
    if (length > 255 || (ota && length == 0)) return 0;
    G435MakeBridgeControl(packet, 4);
    packet[3] = (G435_BYTE)size;
    packet[4] = (G435_BYTE)(size >> 8);
    packet[5] = 0x50;
    packet[6] = 0x41;
    packet[7] = category;
    packet[8] = sequence;
    packet[9] = (G435_BYTE)(ota ? length - 1 : length);
    G435Copy(packet + 10, payload, length);
    return 1;
}

static int G435BridgeFeaturePacket(G435_BYTE feature[64], const G435_BYTE *output) {
    volatile G435_BYTE *zero = feature;
    G435_UINT i, length;
    if (output[0] != 6 || output[2] != 3) return 0;
    for (i = 0; i < 64; ++i) zero[i] = 0;
    feature[0] = 6; feature[1] = output[1]; feature[2] = 3;
    if (output[1] == 4) {
        length = output[3] + ((G435_UINT)output[4] << 8);
        if (length > 60) return 0;
        feature[3] = (G435_BYTE)length;
        G435Copy(feature + 4, output + 5, length);
    } else if (output[1] != 1 && output[1] != 8) return 0;
    return 1;
}

static int G435IsBridgeAck(const G435_BYTE *packet, G435_UINT length) {
    return length >= 2 && packet[0] == 6 && (packet[1] == 0x5a || packet[1] == 0xfe);
}

static int G435AppendPi(G435_PI_PARSER *parser, const G435_BYTE *fragment, G435_UINT length) {
    if (parser->length > G435_PI_BUFFER_SIZE || length > G435_PI_BUFFER_SIZE - parser->length) {
        parser->length = 0;
        return 0;
    }
    G435Copy(parser->bytes + parser->length, fragment, length);
    parser->length += length;
    return 1;
}

/* A feature report reserves byte 0 for RID and byte 63 for the fragment length. */
static int G435FeedFeature(G435_PI_PARSER *parser, const G435_BYTE *feature) {
    const G435_BYTE *fragment = feature + 1;
    G435_UINT length = feature[63], i;
    if (feature[0] != 6 || length > 62) return 0;
    if (length == 0) return 1;
    if (fragment[0] == 1) {
        for (i = 0; i + 1 < length; ++i)
            if (fragment[i] == 0x50 && fragment[i + 1] == 0x49) break;
        if (i + 1 >= length) return 1;
        fragment += i;
        length -= i;
    }
    return G435AppendPi(parser, fragment, length);
}

/* Feature-write firmware returns a complete PI frame followed by FF padding,
   without the output-transport fragment-length byte. Accept only complete frames. */
static int G435FeedRawFeature(G435_PI_PARSER *parser, const G435_BYTE *feature) {
    G435_UINT offset;
    if (feature[0] != 6) return 0;
    for (offset = 1; offset + 10 <= 64; ++offset) {
        if (feature[offset] == 0x50 && feature[offset + 1] == 0x49) {
            G435_UINT size = 10 + feature[offset + 8] + ((G435_UINT)feature[offset + 9] << 8);
            if (size <= 64 - offset) return G435AppendPi(parser, feature + offset, size);
        }
    }
    if (feature[1] != 0xff && feature[63] <= 62) return G435FeedFeature(parser, feature);
    return 1;
}

static void G435DropPrefix(G435_PI_PARSER *parser, G435_UINT length) {
    parser->length -= length;
    G435Copy(parser->bytes, parser->bytes + length, parser->length);
}

/* 1: desired reply, 0: need more bytes, -1: device status/voltage is invalid. */
static int G435TakeReply(G435_PI_PARSER *parser, int kind, int *value) {
    while (parser->length) {
        G435_BYTE *frame = parser->bytes;
        G435_UINT size;
        int found = 0, result = 0;
        if (frame[0] != 0x50 || (parser->length > 1 && frame[1] != 0x49)) {
            G435DropPrefix(parser, 1);
            continue;
        }
        if (parser->length < 10) return 0;
        size = 10 + frame[8] + ((G435_UINT)frame[9] << 8);
        if (size > G435_PI_BUFFER_SIZE) { G435DropPrefix(parser, 1); continue; }
        if (parser->length < size) return 0;
        if (kind == G435_REPLY_MODE && frame[2] == 0x0e && size >= 12 && frame[10] == 0xe0) {
            result = frame[11];
            found = 1;
        } else if (kind == G435_REPLY_BATTERY && (frame[2] == 1 || frame[2] == 6)
                   && size >= 13 && frame[10] == 6) {
            if (frame[12] != 0) found = -1;
            else if (size >= 15) {
                result = frame[13] | ((int)frame[14] << 8);
                found = result >= 2500 && result <= 4500 ? 1 : -1;
            }
        }
        G435DropPrefix(parser, size);
        if (found) { if (found > 0) *value = result; return found; }
    }
    return 0;
}

/* Every attempted bridge entry is paired with exit, including write/ACK failure. */
static G435_BRIDGE_RESULT G435RunBridge(const G435_BRIDGE_IO *io) {
    static const G435_BYTE getMode[] = {0xe0};
    static const G435_BYTE setMode[] = {0xe1, 1};
    static const G435_BYTE battery[] = {0, 0x31};
    G435_BRIDGE_RESULT result;
    G435_BYTE packet[G435_BRIDGE_OUTPUT_SIZE], sequence = 0;
    int mode = 0, attempts;
    result.ok = 0;
    result.millivolts = -1;
    result.cleanupSucceeded = 0;
    result.error = 0;
    G435MakeBridgeControl(packet, 1);
    if (!io->write(io->context, packet, sizeof(packet), 1) || !io->ack(io->context, 1)) goto failed;
    G435MakeBridgePa(packet, 0x0e, sequence++, getMode, 1, 0);
    if (!io->write(io->context, packet, sizeof(packet), 1)
        || !io->reply(io->context, G435_REPLY_MODE, &mode)) goto failed;
    for (attempts = 0; mode != 1 && attempts < 5; ++attempts) {
        G435MakeBridgePa(packet, 0x0e, sequence++, setMode, 2, 0);
        if (!io->write(io->context, packet, sizeof(packet), 1)
            || !io->pause(io->context, 100)) goto failed;
        G435MakeBridgePa(packet, 0x0e, sequence++, getMode, 1, 0);
        if (!io->write(io->context, packet, sizeof(packet), 1)
            || !io->reply(io->context, G435_REPLY_MODE, &mode)) goto failed;
    }
    if (mode != 1) goto failed;
    G435MakeBridgePa(packet, 6, sequence++, battery, 2, 1);
    if (!io->write(io->context, packet, sizeof(packet), 1)
        || !io->reply(io->context, G435_REPLY_BATTERY, &result.millivolts)) goto failed;
    if (result.millivolts < 2500 || result.millivolts > 4500) goto failed;
    result.ok = 1;
    goto cleanup;
failed:
    result.error = io->lastError(io->context);
cleanup:
    G435MakeBridgeControl(packet, 8);
    for (attempts = 0; attempts < 3; ++attempts) {
        if (io->write(io->context, packet, sizeof(packet), 0) && io->ack(io->context, 0)) {
            result.cleanupSucceeded = 1;
            break;
        }
    }
    if (!result.cleanupSucceeded) {
        result.ok = 0;
        if (!result.error) result.error = io->lastError(io->context);
    }
    return result;
}

#endif
