/**
 * rrc — Reticulum Relay Chat client task.
 *
 * The node is the client: this task holds the Links to `rrc.hub`
 * destinations, keeps the room logs and counts what is unread. Every frontend
 * (the web window, the LCD app, the CLI) is a view over storage, exactly as
 * lxmf's are — storage is the API, one writer per field, commands are
 * self-clearing keys. Zero µReticulum includes: everything rides rnsd.h.
 *
 * Persistent:
 *   s.rrc.{nick,sound,keep,max_hubs,presence}      settings
 *   s.rrc.hubs.<hub>.{name,ident,nick,auto}        saved hubs (hub = 32-hex dest)
 *   s.rrc.rooms.<rid>.{hub,name,seq,read_seq,unread,mentions,last_ts,joined,
 *                      notify,preview}             room directory (record store)
 *   s.rrc.log.<rid>.<seq>.{ts,kind,state,mention,own,src,id,nick,text}
 *                                                  room log (record store per room)
 *   s.rrc.ignore.<id12>                            ignored people
 *   secrets.rrc.keys.<rid>                         +k room keys
 * RAM:
 *   rrc.hubs.<hub>.{state,status,name,rtt,hops,iface,motd,rooms_dir,limit_body,
 *                   nick,as,notes.<n>}
 *   rrc.heard.<hub>                                "<mono_s>|<hops>|<name>"
 *   rrc.rooms.<rid>.{state,topic,mode,error,members}
 *   rrc.rooms.<rid>.who.<id12>                     "<nick>|<identity hex>|<lxmf hex>"
 *   rrc.rooms.<rid>.{pres.<n>,pres_next}           RAM ring of joins/leaves (never logged)
 *   rrc.unread / rrc.mentions                      totals over rooms not set to off
 *   rrc.url_web / rrc.url_lcd                      open a hub or room from another app
 * Commands (self-clearing): rrc.cmd.{add,connect,disconnect,forget,join,part,
 *   send,retry,read,notify,nick,ignore,who,watch}. See README.md for each value.
 *
 * <rid> is the first four bytes, in hex, of sha256(hub hash ‖ lowercased room).
 */
#pragma once

#include "service.h"

/** Hubs the node holds a Link to at once, at most (s.rrc.max_hubs ≤ this). */
#define RRC_MAX_SLOTS 8

/** Boot-registered service: starts the rrc task with the RNS lifecycle. */
class RrcService : public Service {
public:
    void onInit() override;
};
