#ifndef _IN_NETSTUB_H
#define _IN_NETSTUB_H

// Stub definitions for when HAVE_NETWORKING is not defined
// These allow the code to compile without networking support

#ifndef HAVE_NETWORKING

// Forward declarations
struct prop;
struct chr;
struct coord;
struct gset;
struct netbuf;

// Empty net client structure
struct netclient {
	int dummy;
};

// Stub globals
#define NETMODE_NONE 0
#define NETMODE_SERVER 0
#define NETMODE_CLIENT 0

static s32 g_NetMode = 0;
static struct netbuf g_NetMsgRel;
static u32 g_NetNextSyncId = 0;

// Stub functions
static inline void netmsgSvcPropSpawnWrite(struct netbuf *buf, struct prop *prop) {}
static inline void netmsgSvcPropMoveWrite(struct netbuf *buf, struct prop *prop, struct coord *rot) {}
static inline void netmsgSvcPropUseWrite(struct netbuf *buf, struct prop *prop, struct netclient *client, s32 op) {}
static inline void netmsgSvcPropLiftWrite(struct netbuf *buf, struct prop *prop) {}
static inline void netmsgSvcPropPickupWrite(struct netbuf *buf, struct netclient *client, struct prop *prop, s32 result) {}
static inline void netmsgSvcPropDoorWrite(struct netbuf *buf, struct prop *prop, struct netclient *client) {}
static inline void netmsgSvcChrDisarmWrite(struct netbuf *buf, struct chr *chr, struct prop *attackerprop, s32 weaponnum, f32 damage, struct coord *pos) {}
static inline void netmsgSvcChrDamageWrite(struct netbuf *buf, struct chr *chr, f32 damage, struct coord *vector, struct gset *gset, struct prop *aprop, s32 hitpart, s32 bodypart, bool isplayer) {}
static inline void netmsgSvcPlayerStatsWrite(struct netbuf *buf, struct netclient *client) {}

#endif // !HAVE_NETWORKING

#endif // _IN_NETSTUB_H
