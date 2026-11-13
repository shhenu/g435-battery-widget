/* SPDX-License-Identifier: MIT. Hardware-independent protocol regression tests. */
#include <stdio.h>
#include <string.h>
#include "../native/g435_protocol.h"

static int failures;
#define CHECK(expression) do { if (!(expression)) { \
    printf("FAIL line %d: %s\n", __LINE__, #expression); ++failures; } } while (0)

static const unsigned char kLocal[] = {0x50,0x49,0x0e,0xc2,0,0,0,0,2,0,0xe0,0};
static const unsigned char kRemote[] = {0x50,0x49,0x0e,0xc3,0,0,0,0,2,0,0xe0,1};
static const unsigned char kBattery[] = {0x50,0x49,1,0xc4,0x1b,0x1e,0xc8,0,7,0,6,0x83,0,0x5a,0x0f,0,0};

static void Feed(G435_PI_PARSER *parser, const unsigned char *bytes, unsigned length) {
    unsigned char feature[64];
    CHECK(length <= 62);
    memset(feature, 0, sizeof(feature));
    feature[0] = 6;
    feature[63] = (unsigned char)length;
    memcpy(feature + 1, bytes, length);
    CHECK(G435FeedFeature(parser, feature));
}

static void TestDiscovery(void) {
    CHECK(G435SelectTransport(0x046d,0x0adf,0xff00,65,65,0) == G435_TRANSPORT_CRC64);
    CHECK(G435SelectTransport(0x046d,0x0acb,0xffc0,7,515,64) == G435_TRANSPORT_BRIDGE6);
    CHECK(G435SelectTransport(0x046d,0x0acb,0xffc0,7,0,64) == G435_TRANSPORT_BRIDGE6);
    CHECK(G435SelectTransport(0x046d,0x0acb,0xffc0,0,0,64) == G435_TRANSPORT_BRIDGE6);
    CHECK(G435SelectTransport(0x046d,0x0acb,0xffc3,7,515,64) == G435_TRANSPORT_NONE);
    CHECK(G435SelectTransport(0x046d,0x0adf,0x000c,65,65,0) == G435_TRANSPORT_NONE);
    CHECK(G435SelectTransport(0x046d,0x0acb,0xffc0,7,515,63) == G435_TRANSPORT_NONE);
    CHECK(G435SelectTransport(0x046d,0x0adf,0xff00,65,64,0) == G435_TRANSPORT_NONE);
    CHECK(G435SelectTransport(0x046d,0xc092,0xff00,65,65,0) == G435_TRANSPORT_NONE);
    CHECK(G435SelectTransport(0x1234,0x0acb,0xffc0,7,515,64) == G435_TRANSPORT_NONE);
    CHECK(G435SelectTransport(0x046d,0xffff,0xffc0,7,515,64) == G435_TRANSPORT_NONE);
}

static void TestCapturedPackets(void) {
    unsigned char packet[515], oversize[256], feature[64];
    const unsigned char expectedMode[] = {6,4,3,6,0,0x50,0x41,0x0e,0,1,0xe0};
    const unsigned char expectedBattery[] = {6,4,3,7,0,0x50,0x41,6,3,1,0,0x31};
    const unsigned char featureMode[] = {6,4,3,6,0x50,0x41,0x0e,0,1,0xe0};
    const unsigned char featureBattery[] = {6,4,3,7,0x50,0x41,6,3,1,0,0x31};
    const unsigned char ack[] = {6,0x5a}, already[] = {6,0xfe}, wrongAck[] = {6,0x42};
    const unsigned char mode[] = {0xe0}, battery[] = {0,0x31};
    G435MakeBridgeControl(packet, 1);
    CHECK(packet[0] == 6 && packet[1] == 1 && packet[2] == 3 && packet[514] == 0);
    CHECK(G435MakeBridgePa(packet,0x0e,0,mode,1,0));
    CHECK(memcmp(packet, expectedMode, sizeof(expectedMode)) == 0);
    CHECK(packet[sizeof(expectedMode)] == 0 && packet[514] == 0);
    CHECK(G435BridgeFeaturePacket(feature,packet));
    CHECK(memcmp(feature,featureMode,sizeof(featureMode)) == 0 && feature[63] == 0);
    CHECK(G435MakeBridgePa(packet,6,3,battery,2,1));
    CHECK(memcmp(packet, expectedBattery, sizeof(expectedBattery)) == 0);
    CHECK(G435BridgeFeaturePacket(feature,packet));
    CHECK(memcmp(feature,featureBattery,sizeof(featureBattery)) == 0 && feature[63] == 0);
    CHECK(!G435MakeBridgePa(packet,6,3,oversize,256,1));
    CHECK(!G435MakeBridgePa(packet,6,3,battery,0,1));
    CHECK(G435IsBridgeAck(ack,2) && G435IsBridgeAck(already,2));
    CHECK(!G435IsBridgeAck(wrongAck,2) && !G435IsBridgeAck(ack,1));
}

static void TestReplyParser(void) {
    G435_PI_PARSER parser;
    unsigned char feature[64], service[40], malformed[40], wrongStatus[sizeof(kBattery)];
    int value = -1;
    memset(&parser, 0, sizeof(parser));
    Feed(&parser,kLocal,sizeof(kLocal));
    CHECK(G435TakeReply(&parser,G435_REPLY_MODE,&value) == 1 && value == 0);
    Feed(&parser,kRemote,1);
    CHECK(G435TakeReply(&parser,G435_REPLY_MODE,&value) == 0);
    Feed(&parser,kRemote+1,4);
    CHECK(G435TakeReply(&parser,G435_REPLY_MODE,&value) == 0);
    Feed(&parser,kRemote+5,sizeof(kRemote)-5);
    CHECK(G435TakeReply(&parser,G435_REPLY_MODE,&value) == 1 && value == 1);
    Feed(&parser,kBattery,13);
    CHECK(G435TakeReply(&parser,G435_REPLY_BATTERY,&value) == 0);
    Feed(&parser,kBattery+13,sizeof(kBattery)-13);
    CHECK(G435TakeReply(&parser,G435_REPLY_BATTERY,&value) == 1 && value == 3930);

    service[0] = 1; service[1] = 0xc0;
    memcpy(service+2,kRemote,sizeof(kRemote));
    Feed(&parser,service,2+sizeof(kRemote));
    CHECK(G435TakeReply(&parser,G435_REPLY_MODE,&value) == 1 && value == 1);
    memcpy(malformed,kLocal,sizeof(kLocal));
    malformed[8] = malformed[9] = 255;
    memcpy(malformed+sizeof(kLocal),kRemote,sizeof(kRemote));
    Feed(&parser,malformed,sizeof(kLocal)+sizeof(kRemote));
    CHECK(G435TakeReply(&parser,G435_REPLY_MODE,&value) == 1 && value == 1);

    memcpy(wrongStatus,kBattery,sizeof(kBattery));
    wrongStatus[12] = 1;
    Feed(&parser,wrongStatus,sizeof(wrongStatus));
    CHECK(G435TakeReply(&parser,G435_REPLY_BATTERY,&value) == -1);
    wrongStatus[12] = 0; wrongStatus[13] = wrongStatus[14] = 0;
    Feed(&parser,wrongStatus,sizeof(wrongStatus));
    CHECK(G435TakeReply(&parser,G435_REPLY_BATTERY,&value) == -1);

    memset(feature,0,sizeof(feature)); feature[0] = 6; feature[63] = 63;
    CHECK(!G435FeedFeature(&parser,feature));
    feature[63] = 1; parser.length = G435_PI_BUFFER_SIZE;
    CHECK(!G435FeedFeature(&parser,feature) && parser.length == 0);

    /* A second public capture, padded with FF instead of a fragment count. */
    {
        const unsigned char rawCapture[] = {6,0x50,0x49,1,0xc8,0x75,0x48,0x0d,
                                            0,7,0,6,0x82,0,0x32,0x10,0,0};
        memset(feature,0xff,sizeof(feature));
        memcpy(feature,rawCapture,sizeof(rawCapture));
        CHECK(G435FeedRawFeature(&parser,feature));
        CHECK(G435TakeReply(&parser,G435_REPLY_BATTERY,&value) == 1 && value == 4146);
        memset(feature,0xff,sizeof(feature)); feature[0] = 6;
        CHECK(G435FeedRawFeature(&parser,feature));
        CHECK(parser.length == 0);
    }
}

typedef struct {
    int failure;
    int enters, exits, modeChanges, batteryCommands, unsafeWrites;
    int lastCommand, remote, badFlags;
} FAKE_BRIDGE;

static int FakeWrite(void *context, const unsigned char *packet, unsigned length, int allowCancel) {
    FAKE_BRIDGE *fake = (FAKE_BRIDGE *)context;
    fake->lastCommand = packet[1];
    if (length != 515 || packet[0] != 6 || packet[2] != 3) ++fake->unsafeWrites;
    if (packet[1] == 1) {
        ++fake->enters;
        return fake->failure != 1;
    }
    if (packet[1] == 8) {
        ++fake->exits;
        if (allowCancel) ++fake->badFlags;
        return fake->failure != 7;
    }
    if (!allowCancel) ++fake->badFlags;
    if (packet[1] != 4) { ++fake->unsafeWrites; return 0; }
    if (packet[7] == 0x0e && packet[10] == 0xe0) return 1;
    if (packet[7] == 0x0e && packet[10] == 0xe1 && packet[11] == 1) {
        ++fake->modeChanges;
        if (fake->failure != 8) fake->remote = 1;
        return 1;
    }
    if (packet[7] == 6 && packet[9] == 1 && packet[10] == 0 && packet[11] == 0x31) {
        ++fake->batteryCommands;
        return fake->failure != 9;
    }
    ++fake->unsafeWrites;
    return 0;
}

static int FakeAck(void *context, int allowCancel) {
    FAKE_BRIDGE *fake = (FAKE_BRIDGE *)context;
    if (fake->lastCommand == 1) {
        if (!allowCancel) ++fake->badFlags;
        return fake->failure != 2;
    }
    if (fake->lastCommand != 8 || allowCancel) ++fake->badFlags;
    return 1;
}

static int FakeReply(void *context, int kind, int *value) {
    FAKE_BRIDGE *fake = (FAKE_BRIDGE *)context;
    if (kind == G435_REPLY_MODE) {
        *value = fake->remote;
        return fake->failure != 3;
    }
    *value = fake->failure == 6 ? 0 : 3930;
    return fake->failure != 5;
}

static int FakePause(void *context, unsigned milliseconds) {
    FAKE_BRIDGE *fake = (FAKE_BRIDGE *)context;
    CHECK(milliseconds == 100);
    return fake->failure != 4;
}

static unsigned long FakeError(void *context) { (void)context; return 4660; }

static void TestBridgeCleanup(void) {
    int failure;
    for (failure = 0; failure <= 9; ++failure) {
        FAKE_BRIDGE fake;
        G435_BRIDGE_IO io;
        G435_BRIDGE_RESULT result;
        memset(&fake,0,sizeof(fake)); fake.failure = failure;
        io.context = &fake; io.write = FakeWrite; io.ack = FakeAck;
        io.reply = FakeReply; io.pause = FakePause; io.lastError = FakeError;
        result = G435RunBridge(&io);
        CHECK(result.ok == (failure == 0));
        CHECK(fake.enters == 1 && fake.exits == (failure == 7 ? 3 : 1));
        CHECK(result.cleanupSucceeded == (failure != 7));
        CHECK(fake.unsafeWrites == 0 && fake.badFlags == 0);
        if (failure == 0) CHECK(result.millivolts == 3930 && fake.modeChanges == 1);
        else CHECK(result.error == 4660);
        if (failure == 8) CHECK(fake.modeChanges == 5 && fake.batteryCommands == 0);
    }
}

int main(void) {
    TestDiscovery();
    TestCapturedPackets();
    TestReplyParser();
    TestBridgeCleanup();
    if (failures) { printf("%d protocol checks failed\n", failures); return 1; }
    puts("G435 protocol checks passed: both revisions, captured frames, malformed replies, cleanup failures/cancellation.");
    return 0;
}
