# rrc — Reticulum Relay Chat on spangap

```
node                                        hub (rrc.hub)
  ── Link + LINKIDENTIFY ──────────────────▶
  ── HELLO {name, version, caps} + NICK ───▶   resent every 4 s until WELCOME
  ◀─ WELCOME {hub, version, caps, limits} ──
  ── JOIN #room [key] ─────────────────────▶   every room wanted on this hub
  ◀─ JOINED ─────────────────────────────────   may never come: see below
  ◀─ NOTICE "room <r>: registered; …" ──────   also means we are in
  ── MSG "/who #room", MSG "/list" ────────▶   answered as NOTICEs, read quietly
  ◀▶ MSG / ACTION / NOTICE / JOINED / PARTED / PING / PONG
```

The node is the RRC client. The rrc task holds the Links to the hubs, keeps
what is said in every room it is in, and counts what nobody has read yet, so the
device is its own bouncer: still in the room with the browser closed and the
screen dark, and reading a room on either surface clears it on both. The browser
window, the LCD app and the CLI are views of that state and writers of the
commands it takes. Hubs keep no history, so the node has what it heard while it
was connected and nothing from before.

It speaks [RRC](https://rrc.kc1awv.net) v1 plus the extensions of the reference
hub, `kc1awv/rrcd`: the numeric limits map in WELCOME, the slash commands, the
`rrc.hub` announce, resource envelopes for a long MOTD or notice. It advertises
capabilities 0 (resources) and 1 (ACTION).

Every envelope rides one Link packet (about 430 bytes of payload), and hubs
differ in what they do with one that does not fit. rrcd and rrc-hub, with
their `include_joined_member_list` option on, send the joiner a JOINED listing
every member's identity, and in a room of more than about 22 people that
packet is dropped unsent. So the node also takes the "room <r>: …" notice a
hub sends only to a successful joiner as being in the room. A long notice
comes as a Resource of kind `notice`, or as several NOTICEs: each line its
own, a long line cut into fixed-size pieces anywhere, even inside a word.
What follows the first line of a `/who` or `/list` reply within 3 s is joined
to it (a new `/list` line on a newline, any other piece directly) and the
whole is parsed again, so the member list and the room directory are
complete and no piece shows as a notice of its own.

## Using it

**Hubs.** Hubs heard announcing `rrc.hub` are listed under *On the mesh*; a hub
can also be added by its 32-hex hash. Wherever a hub hash is typed — the add
fields, `rrc.cmd.*`, the `rrc` command — spaces anywhere are ignored, and the
full 64-hex hash is taken as its leading 32. Connecting is always an explicit act, and
a connected hub is reconnected after a reboot or a lost Link (5 s, 15 s, 45 s,
2 min, then every 5 min, ±20 %) until it is disconnected. At most
`s.rrc.max_hubs` hubs are connected at once; each holds one Reticulum Link.

**Who you are there.** The Link is identified as an LXMF identity, so whoever
is in the room can message you back and a hub that tells you about mentions by
LXMF reaches your inbox. With one LXMF identity on the device it is used; with
several, the first connect to a hub asks, and the answer is kept for that hub.
The node's own identity is the last choice in that list. A hub knows you by the
identity: a founder's operator rights, bans and invites all key on it.

**Rooms.** Joining a room that does not exist yet creates it on the hub. A room
with a key (`+k`) asks for it once; the key is kept under `secrets.rrc.keys`.
Leaving a room forgets it and its log.

**Typing.** `/me` sends an action, `/join #room [key]`, `/part`, `/nick <name>`
and `/clear` are handled on the device, and anything else starting with `/` goes
to the hub, whose answer is shown dimmed and kept only in RAM. `//` sends a
literal slash. A line longer than the hub's body limit (350 bytes on rrcd) is
refused before it is sent; it is never split.

**Nick.** The nick rides on every envelope; there is no message that announces
a change, so the hub and the room learn a new one with our next line. `/nick`
(and `rrc.cmd.nick`) applies it locally at once: our roster entry in every room
we are in on that hub, and a "You are now known as …" line in each.

**Links.** A room is linked as `rrc://#<room>@<hub>`, or just `#<room>@<hub>`
(at the start of the text or after a space, `(` or `,`); `rrc://<hub>/<room>`,
`rrc@<hub>` and a bare `#room` (on the line's own hub) are understood too. In
the web log, and from LXMF and Nomad on both surfaces (through `rrc.url_web` /
`rrc.url_lcd`), opening one connects the hub if it isn't and joins the room
and opens it; a hub-only link opens the hub. The LCD log does not make links
tappable.

**Delivery.** A line is *queued* until the node's pacing lets it go, *relayed*
(✓) when the hub echoes it back, *failed* (✗) when no echo comes within
max(15 s, 4 × the round trip) or the Link drops. A failed line can be retried;
the retry carries the same message id.

**People.** Nicks are advisory and not unique; a person is their identity. The
member list holds the first 12 hex of each identity (what rrcd's `/who` gives),
and the full identity once that person has said something, from which their
LXMF address follows. Ignoring someone hides their lines from then on.

The list is asked for with a quiet `/who` on entering a room and on
`rrc.cmd.who`. After that, a join or leave that names the identity (a hub's
`include_joined_member_list`), or a leave whose nick is one member's, updates
it on the spot. Any other join or leave makes it stale, and the `/who` that
refreshes it goes only when all of these hold: the room has had no join or
leave for 10 s, its last `/who` was at least a minute ago, and somebody is
looking at its member list. That last is a lease a frontend holds through
`rrc.cmd.watch` while the room is open with its member list showing, the
window is in front (the web window the front one in a visible tab, the LCD
app shown with the screen out of standby) and there has been input there in
the last five minutes. A stale list waits, unasked for, until then.

## Storage

Persistent (`s.`):

| Key | Meaning |
|---|---|
| `s.rrc.nick` | Default nick; empty = the LXMF display name of the identity in use. |
| `s.rrc.sound` | `off` · `mentions` · `all` — the chirp (`/fixed/rrc/chirp.wav`). |
| `s.rrc.keep` | Lines kept per room (default 500). |
| `s.rrc.max_hubs` | Hubs connected at once (1–8, default 4). |
| `s.rrc.presence` | Join/leave lines, shown while a room is open (never logged): `collapsed` (the frontends fold runs) · `shown` · `hidden` (not kept at all). |
| `s.rrc.hubs.<hub>.{name,ident,nick,auto}` | A saved hub. `ident` = `lxmf<n>` \| `node`; `auto` = 1 while it should be connected. |
| `s.rrc.ignore.<id12>` | 1 = lines from this identity are not logged. |
| `secrets.rrc.keys.<rid>` | A room's `+k` key. |

Record stores (fetch them with `{"fetch": …}`; see spangap-core storage-internals §0):

| Store | Records | Fields |
|---|---|---|
| `s.rrc.rooms` (browser-mirrored) | one per room, keyed `<rid>` | `hub` (32 hex), `name`, `seq` (newest line), `read_seq`, `unread`, `mentions`, `last_ts` (unix s), `joined` (1 = wanted), `notify` (0 all · 1 mentions · 2 off), `preview` (finished one-line text of the newest line) |
| `s.rrc.log.<rid>` (fetched while open) | one per line, keyed by the 8-digit `seq` | `ts` (unix s), `kind` (0 message · 1 action · 2 notice · 4 system; 3 join/leave is the frontends' own, from the RAM ring), `state` (0 queued · 1 relayed · 2 failed · 3 received), `mention`, `own`, `src` (12 hex), `id` (16 hex), `nick`, `text` |

A room's `name` is its wire name: trimmed, no leading `#`, lowercased — the form
NomadNet, thatSFguy's Go hub and the Reticulum Mobile App send, so a room typed
as `#Mesh` here is the room they call `mesh`. Frontends show it as `#mesh`.
`<rid>` is the first four bytes, in hex, of sha256(hub hash ‖ that name).

A join or leave is never logged and never reaches flash. It takes the room's
next `seq` as a line would, leaving a hole in the log, and goes to a RAM ring
of the room's latest 32 (`rrc.rooms.<rid>.pres.*`). A frontend shows those
that arrive while it has the room open, in their holes, as lines of kind 3
whose `text` is `joined` or `left` and whose `nick` is who, and forgets them
when the room closes. So `seq` in `s.rrc.rooms` is the newest *logged* line,
and the window `s.rrc.keep` holds that many seqs back from it, holes included:
each new line prunes every line at or below `seq − keep`, so lowering the
setting trims the backlog with the room's next line.

RAM:

| Key | Meaning |
|---|---|
| `rrc.hubs.<hub>.state` | `idle` · `need_ident` · `linking` · `hello` · `live` · `retry` · `held` |
| `rrc.hubs.<hub>.status` | That state as a finished sentence ("Connected", "Retrying in 40 s", "Choose who to chat as"). |
| `rrc.hubs.<hub>.label` | The hub's name to show: the saved name, else its own from WELCOME, else its announced one, else the first 8 hex. |
| `rrc.hubs.<hub>.{name,motd,rtt,hops,iface,limit_body,nick,as,self}` | The hub's own name from WELCOME, its greeting, round trip in ms, hops, interface, body limit in bytes, our nick there, who we are there (a label) and our 12-hex identity prefix. |
| `rrc.hubs.<hub>.rooms_dir` | The hub's public rooms from `/list`: one `<room>\|<topic>` per line. |
| `rrc.hubs.<hub>.notes.<n>` / `.notes_next` | A ring of 16 hub notices and command replies, `<unix s>\|<rid or empty>\|<text>`; the newest is `(notes_next − 1) mod 16`. |
| `rrc.hubs.<hub>.watch` | Written by a frontend: 1 while the hub's screen is open, which makes the node measure the round trip (one PING a minute). |
| `rrc.heard.<hub>` | `<monotonic s>\|<hops>\|<name>` from the hub's announce. Age = now − (`sys.boot_time` + monotonic). |
| `rrc.rooms.<rid>.{state,topic,mode,error,members}` | `state` = `out` · `joining` · `in`; `error` = why a join or a send was refused, finished text, empty when fine. |
| `rrc.rooms.<rid>.who.<id12>` | `<nick>\|<identity hex or empty>\|<lxmf.delivery hex or empty>` |
| `rrc.rooms.<rid>.pres.<n>`, `.pres_next` | The room's latest 32 joins and leaves, `<ts>\|<seq>\|<j\|l>\|<id12>\|<nick>` in slot `n mod 32`; `pres_next` counts every one written. |
| `rrc.unread`, `rrc.mentions` | Totals over the rooms not set to Off. |
| `rrc.url_web`, `rrc.url_lcd` | Written by another app to open RRC: `<hub>[/<room>]\|<nonce>`. The hub is connected if it isn't; a room is joined and opened. |

Commands (self-clearing; write the key, the task unsets it and acts):

| Key | Value |
|---|---|
| `rrc.cmd.add` | `<hub>[\|<name>]` — save a hub without connecting. |
| `rrc.cmd.connect` | `<hub>[\|<sel>]` — connect, and remember `sel` (`lxmf<n>` \| `node`) as who to chat as. |
| `rrc.cmd.disconnect` | `<hub>` |
| `rrc.cmd.forget` | `<hub>` — the hub, its rooms and their logs. |
| `rrc.cmd.join` | `<hub>\|<room>[\|<key>]` — connects the hub if it is not. |
| `rrc.cmd.part` | `<rid>` |
| `rrc.cmd.send` | `<rid>\|<nonce>\|<text>` — the text as typed, slash commands included. |
| `rrc.cmd.retry` | `<rid>\|<seq>` |
| `rrc.cmd.read` | `<rid>\|<seq>` — the newest line a frontend has shown at the bottom with the screen awake. |
| `rrc.cmd.notify` | `<rid>\|all\|mentions\|off` |
| `rrc.cmd.nick` | `<hub>\|<nick>` — empty = the default. |
| `rrc.cmd.ignore` | `<id12>\|<0\|1>` |
| `rrc.cmd.who` | `<rid>` — refresh the member list. |
| `rrc.cmd.watch` | `web\|<rid>` or `lcd\|<rid>` — that frontend has the room's member list in front of somebody; held 45 s, so renewed every 15 s while it lasts. An empty `<rid>` ends it. |

## CLI

```
rrc                              saved hubs and their rooms
rrc heard                        hubs heard announcing
rrc connect <hub> [lxmf<n>|node]
rrc disconnect <hub>
rrc forget <hub>
rrc join <hub> <room> [key]
rrc part <room>
rrc say <room> <text>            /me, /who, /list… work too
rrc log <room> [n]
rrc who <room>
rrc notes <hub>
```

## Testing

`esp-idf/test/host/` holds the CBOR codec's host test, against vectors made by
`cbor2` (the encoder rrcd uses):

```sh
cd esp-idf/test/host
g++ -std=c++17 -I../../src test_cbor.cpp ../../src/rrc_cbor.cpp -o /tmp/test_cbor && /tmp/test_cbor
```

Against a real hub: run `rrcd` (pip install from `kc1awv/rrcd`) with a
`configdir` whose interfaces reach the device, join a room from the CLI and talk
to it from any other RRC client.
