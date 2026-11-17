#ifndef T2R_RELAY_PROTOCOL_H
#define T2R_RELAY_PROTOCOL_H

#include <stdint.h>

// Message type - the first byte of every relay packet.
enum {
    RELAY_REGISTER     = 0,   // client -> relay : claim a (session, slot); also the keepalive
    RELAY_REGISTER_ACK = 1,   // relay  -> client: registration confirmed
    RELAY_DATA         = 2,   // client -> relay -> client(s): one game packet
    RELAY_PING         = 3,   // client -> relay : stateless latency probe (RelayPing); no session touched
    RELAY_PONG         = 4,   // relay  -> client: echo of the probe (RelayPing), for RTT measurement
};

#define RELAY_BROADCAST 0xFF
#define RELAY_MAX_SLOTS 8
#define RELAY_MAX_PAYLOAD 4096

#pragma pack(push, 1)

typedef struct {
    uint8_t  type;
    uint32_t sessionId;
    uint8_t  slot;
} RelayRegister;

typedef struct {
    uint8_t  type;
    uint32_t sessionId;
    uint8_t  srcSlot;
    uint8_t  destSlot;
} RelayDataHeader;

typedef struct {
    uint8_t  type;
    uint32_t nonce;
} RelayPing;

#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(RelayRegister)   == 6, "RelayRegister layout");
static_assert(sizeof(RelayDataHeader) == 7, "RelayDataHeader layout");
static_assert(sizeof(RelayPing)       == 5, "RelayPing layout");
#endif

#endif
