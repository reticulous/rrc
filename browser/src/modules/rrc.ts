/**
 * rrc — Reticulum Relay Chat window (SPA side).
 *
 * The node is the RRC client; this window is a view over its storage and a
 * writer of its command keys (see ../../../README.md, the storage contract):
 *   reads  = computed() over the reactive device mirror
 *   writes = self-clearing rrc.cmd.* keys via device.sendJson()
 *
 *   s.rrc.hubs.<hub>.{name,ident,nick,auto}      saved hubs
 *   rrc.hubs.<hub>.{state,status,name,motd,rtt,hops,iface,limit_body,nick,as,self,
 *                   rooms_dir,notes.<n>,notes_next,watch}
 *   rrc.heard.<hub>            "<monotonic s>|<hops>|<name>"
 *   s.rrc.rooms                record store, browser-mirrored: fetched once per stream
 *   s.rrc.log.<rid>            record store per room: fetched while the room is open
 *   rrc.rooms.<rid>.{state,topic,mode,error,members,who.<id12>}
 *   rrc.url_web                "<hub>[/<room>]|<nonce>" — another app opening RRC
 *
 * This is the only module that touches useDeviceStore; the components take
 * what it computes and call what it returns.
 */
import { ref, reactive, computed, watch, effectScope, type ComputedRef, type Ref } from 'vue'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { registerApp } from 'spangap-browser/lib/apps'
import { registerWindowMount } from 'spangap-browser/lib/windowMounts'
import RrcWindow from '../panels/RrcWindow.vue'
import { bareRoom } from '../lib/rrcText'

/* Visibility ref for the RRC floating window. */
export const rrcVisible = ref(false)

/** Focus nonce — bumped to raise the window even when already open; the
 *  window mount binds it to the window's `focus-token` prop. */
export const rrcFocus = ref(0)

/* Dock "RRC" action: only ever show + raise, never hide. */
export function showRrc() { rrcVisible.value = true; rrcFocus.value++ }

/** Open request — from `rrc.url_web` (another app) or a link tapped in a line.
 *  RrcWindow watches it and shows the room when it is one of ours, else the
 *  hub with the room ready to join. The nonce makes a repeat fire again. */
export const rrcOpenReq = ref<{ hub: string; room: string; nonce: number } | null>(null)

export function openRrc(hub: string, room = '') {
  showRrc()
  rrcOpenReq.value = { hub, room: bareRoom(room), nonce: Date.now() }
}

export type HubState = 'idle' | 'need_ident' | 'linking' | 'hello' | 'live' | 'retry' | 'held' | ''

export interface RrcNote { ts: number; rid: string; text: string; order: number }

export interface RrcHub {
  hash: string
  /** In s.rrc.hubs — a hub the node keeps. */
  saved: boolean
  /** Saved name, else the hub's own from WELCOME, else its announce; '' = none. */
  name: string
  state: HubState
  /** The state as the firmware's finished sentence. */
  status: string
  rtt: number
  hops: number
  iface: string
  limitBody: number
  /** Our nick there now (RAM) and the one saved for the hub ('' = default). */
  nick: string
  savedNick: string
  /** Who to chat as, saved: `lxmf<n>` | `node` | ''. */
  ident: string
  auto: boolean
  /** Who we are there, as a label, and our 12-hex identity prefix. */
  as: string
  self: string
  motd: string
  roomsDir: { room: string; topic: string }[]
}

export interface RrcHeard { hash: string; name: string; hops: number; lastSeen: number; mono: number }

export interface RrcRoom {
  rid: string
  hub: string
  /** Wire name: lowercased, no '#'. Shown as "#" + name. */
  name: string
  seq: number
  readSeq: number
  unread: number
  mentions: number
  lastTs: number
  joined: boolean
  notify: 0 | 1 | 2
  preview: string
  /* RAM */
  state: 'out' | 'joining' | 'in' | ''
  topic: string
  mode: string
  error: string
  members: number
}

export interface RrcMember { id12: string; nick: string; identity: string; lxmf: string }

export enum LineKind { Msg = 0, Action = 1, Notice = 2, Presence = 3, System = 4 }
export enum LineState { Queued = 0, Relayed = 1, Failed = 2, Received = 3 }

export interface RrcLine {
  seq: number
  ts: number
  kind: LineKind
  state: LineState
  mention: boolean
  own: boolean
  src: string
  id: string
  nick: string
  text: string
}

/** One identity the node can chat as. `sel` is what the firmware takes. */
export interface RrcIdentity { sel: string; label: string; hash: string; lxmf: boolean }

export type NotifyLevel = 'all' | 'mentions' | 'off'
export const NOTIFY_NAMES: NotifyLevel[] = ['all', 'mentions', 'off']

type Patch = Record<string, any>

function nest(path: string, val: any): Patch {
  const parts = path.split('.')
  const root: Patch = {}
  let cur = root
  for (let i = 0; i < parts.length - 1; i++) cur = (cur[parts[i]!] = {})
  cur[parts[parts.length - 1]!] = val
  return root
}

function num(v: unknown, d = 0): number { const n = Number(v); return Number.isFinite(n) ? n : d }
function str(v: unknown): string { return v == null ? '' : String(v) }

const HASH_RE = /^[0-9a-f]{32}$/
const isHash = (k: string) => HASH_RE.test(k)

/* Session memory, not persisted: a draft per room, and which rooms' logs this
 * stream has already shipped. */
const drafts = reactive<Record<string, string>>({})
const shippedLogs = new Set<string>()
let shippedEpoch = -1
let roomsEpoch = -1

/** The room the window shows ('' = none); its log is kept shipped. */
const openRid = ref('')

export interface UseRrc {
  hubs: ComputedRef<RrcHub[]>
  /** A hub by hash — saved, live, heard or merely named; null for a non-hash. */
  hubOf: (hash: string) => RrcHub | null
  /** Heard announcing and not saved, newest first. */
  heard: ComputedRef<RrcHeard[]>
  heardOf: (hash: string) => RrcHeard | null
  rooms: ComputedRef<RrcRoom[]>
  roomOf: (rid: string) => RrcRoom | null
  /** The rid of `room` on `hub`, '' when the node isn't keeping it. */
  findRoom: (hub: string, room: string) => string
  roster: (rid: string) => RrcMember[]
  /** id12 → lxmf.delivery hex, over every room's roster and our own slots. */
  lxmfOf: (hub: string, id12: string) => string
  ignored: ComputedRef<Set<string>>
  log: (rid: string) => RrcLine[]
  /** The room on screen; its log is fetched once per stream while it is. */
  openRoom: Ref<string>
  notes: (hub: string) => RrcNote[]
  presence: ComputedRef<string>
  identities: ComputedRef<RrcIdentity[]>
  draft: (rid: string) => string
  setDraft: (rid: string, v: string) => void

  add: (hub: string, name?: string) => void
  connect: (hub: string, sel?: string) => void
  disconnect: (hub: string) => void
  forget: (hub: string) => void
  join: (hub: string, room: string, key?: string) => void
  part: (rid: string) => void
  send: (rid: string, text: string) => void
  retry: (rid: string, seq: number) => void
  markRead: (rid: string, seq: number) => void
  setNotify: (rid: string, level: NotifyLevel) => void
  setNick: (hub: string, nick: string) => void
  ignore: (id12: string, on: boolean) => void
  who: (rid: string) => void
  watchHub: (hub: string, on: boolean) => void
  openLxmf: (hash: string) => void
  openNomad: (hash: string, path: string) => void
}

let _inst: UseRrc | null = null

/** The RRC state layer. One instance for the page, in its own effect scope so
 *  no component's unmount stops it. */
export function useRrc(): UseRrc {
  if (_inst) return _inst
  _inst = effectScope(true).run(build)!
  return _inst
}

function build(): UseRrc {
  const device = useDeviceStore()

  /* The room directory is a browser-mirrored record store: it rides no dump,
   * so ask once per stream (syncEpoch — one per completed dump) and the device
   * mirrors every change after. */
  watch(() => device.syncEpoch, (epoch) => {
    if (!device.synced || roomsEpoch === epoch) return
    roomsEpoch = epoch
    device.sendCommand({ fetch: 's.rrc.rooms' })
  }, { immediate: true })

  /* A room's log ships on request and is live from then on. Ask ONCE per room
   * per stream: a fetch is a resync that empties the subtree and refills it,
   * which would be the log visibly rebuilding under the reader. A subtree that
   * has gone missing (the device dropped it from its open set) is the signal to
   * ask again. A new stream forgets what was shipped with the old one. */
  function shipLog(rid: string) {
    if (!rid || !device.synced) return
    if (shippedEpoch !== device.syncEpoch) { shippedEpoch = device.syncEpoch; shippedLogs.clear() }
    const path = `s.rrc.log.${rid}`
    if (shippedLogs.has(path) && device.get(path) !== undefined) return
    device.sendCommand({ fetch: path })
    shippedLogs.add(path)
  }

  /* A (re)dump is a new stream, and the device's open set went with the old
   * one: re-ask for the room on screen, or it sits there not updating. */
  watch(() => device.syncEpoch, () => { if (openRid.value) shipLog(openRid.value) })
  watch(openRid, (rid) => { if (rid) shipLog(rid) }, { immediate: true })

  const heardRaw = computed(() => {
    const out = new Map<string, RrcHeard>()
    const tree = device.get('rrc.heard') ?? {}
    /* Announce stamps are device monotonic seconds; sys.boot_time is the
     * wall-clock instant of boot, so boot + mono is the unix time heard
     * (0 while the device clock is not yet valid). */
    const boot = num(device.get('sys.boot_time'))
    for (const [hash, raw] of Object.entries(tree)) {
      if (!isHash(hash) || typeof raw !== 'string') continue
      const p1 = raw.indexOf('|')
      const p2 = p1 >= 0 ? raw.indexOf('|', p1 + 1) : -1
      if (p2 < 0) continue
      const mono = num(raw.slice(0, p1))
      out.set(hash, {
        hash, mono,
        hops: num(raw.slice(p1 + 1, p2)),
        name: raw.slice(p2 + 1),
        lastSeen: boot > 0 && mono > 0 ? boot + mono : 0,
      })
    }
    return out
  })

  function parseHub(hash: string, s: any, r: any): RrcHub {
    const dir = str(r?.rooms_dir).split('\n').filter(Boolean).map((l) => {
      const bar = l.indexOf('|')
      return { room: bareRoom(bar >= 0 ? l.slice(0, bar) : l), topic: bar >= 0 ? l.slice(bar + 1) : '' }
    })
    return {
      hash,
      saved: s != null,
      name: str(s?.name) || str(r?.name) || heardRaw.value.get(hash)?.name || '',
      state: str(r?.state) as HubState,
      status: str(r?.status),
      rtt: num(r?.rtt),
      hops: num(r?.hops, -1),
      iface: str(r?.iface),
      limitBody: num(r?.limit_body),
      nick: str(r?.nick),
      savedNick: str(s?.nick),
      ident: str(s?.ident),
      auto: num(s?.auto) === 1,
      as: str(r?.as),
      self: str(r?.self).toLowerCase(),
      motd: str(r?.motd),
      roomsDir: dir,
    }
  }

  /* Saved hubs merged with the ones the task has RAM state for. */
  const hubs = computed<RrcHub[]>(() => {
    const saved = device.get('s.rrc.hubs') ?? {}
    const ram = device.get('rrc.hubs') ?? {}
    const keys = new Set([...Object.keys(saved), ...Object.keys(ram)].filter(isHash))
    return [...keys].map(h => parseHub(h, saved[h], ram[h]))
      .sort((a, b) => (a.name || a.hash).localeCompare(b.name || b.hash))
  })

  const hubOf = (hash: string): RrcHub | null => {
    if (!isHash(hash)) return null
    const found = hubs.value.find(h => h.hash === hash)
    /* A hub only heard, or only linked to: no state yet, but it has a view. */
    return found ?? parseHub(hash, undefined, undefined)
  }

  /* Heard and not yet saved, newest first. */
  const heard = computed<RrcHeard[]>(() => {
    const saved = device.get('s.rrc.hubs') ?? {}
    return [...heardRaw.value.values()].filter(h => saved[h.hash] == null)
      .sort((a, b) => b.mono - a.mono)
  })

  const rooms = computed<RrcRoom[]>(() => {
    const tree = device.get('s.rrc.rooms') ?? {}
    const ram = device.get('rrc.rooms') ?? {}
    const out: RrcRoom[] = []
    for (const [rid, r] of Object.entries<any>(tree)) {
      if (!r || typeof r !== 'object') continue
      const x = ram[rid] ?? {}
      out.push({
        rid,
        hub: str(r.hub).toLowerCase(),
        name: bareRoom(str(r.name)),
        seq: num(r.seq),
        readSeq: num(r.read_seq),
        unread: num(r.unread),
        mentions: num(r.mentions),
        lastTs: num(r.last_ts),
        joined: num(r.joined) === 1,
        notify: Math.min(2, Math.max(0, num(r.notify, 1))) as 0 | 1 | 2,
        preview: str(r.preview),
        state: str(x.state) as RrcRoom['state'],
        topic: str(x.topic),
        mode: str(x.mode),
        error: str(x.error),
        members: num(x.members),
      })
    }
    /* Most recent activity first, as the LCD's flat list. */
    return out.sort((a, b) => b.lastTs - a.lastTs || a.name.localeCompare(b.name))
  })

  const roomOf = (rid: string) => rooms.value.find(r => r.rid === rid) ?? null

  const findRoom = (hub: string, room: string) => {
    const n = bareRoom(room)
    return rooms.value.find(r => r.hub === hub && r.name === n)?.rid ?? ''
  }

  const roster = (rid: string): RrcMember[] => {
    const tree = device.get(`rrc.rooms.${rid}.who`) ?? {}
    const out: RrcMember[] = []
    for (const [id12, raw] of Object.entries(tree)) {
      if (typeof raw !== 'string') continue
      /* "<nick>|<identity hex or empty>|<lxmf.delivery hex or empty>" */
      const [nick = '', identity = '', lxmf = ''] = raw.split('|')
      out.push({ id12: id12.toLowerCase(), nick, identity: identity.toLowerCase(),
                 lxmf: lxmf.toLowerCase() })
    }
    return out.sort((a, b) =>
      (a.nick || '~' + a.id12).localeCompare(b.nick || '~' + b.id12, undefined, { sensitivity: 'base' }))
  }

  /* Every lxmf.delivery address the rosters have learnt, by 12-hex identity. */
  const lxmfById = computed(() => {
    const out = new Map<string, string>()
    const ram = device.get('rrc.rooms') ?? {}
    for (const r of Object.values<any>(ram)) {
      for (const [id12, raw] of Object.entries(r?.who ?? {})) {
        if (typeof raw !== 'string') continue
        const lx = raw.split('|')[2] ?? ''
        if (HASH_RE.test(lx.toLowerCase())) out.set(id12.toLowerCase(), lx.toLowerCase())
      }
    }
    return out
  })

  /* Our own lines carry our 12-hex prefix; when we chat as an LXMF identity its
   * address is the one our own avatar is drawn from. */
  const lxmfOf = (hub: string, id12: string): string => {
    const k = id12.toLowerCase()
    const known = lxmfById.value.get(k)
    if (known) return known
    const h = hubOf(hub)
    if (h && h.self === k && h.ident.startsWith('lxmf')) {
      const d = str(device.get(`lxmf.id.${h.ident.slice(4)}.dest_hash`)).toLowerCase()
      if (HASH_RE.test(d)) return d
    }
    return ''
  }

  const ignored = computed(() => {
    const tree = device.get('s.rrc.ignore') ?? {}
    return new Set(Object.entries(tree).filter(([, v]) => num(v) === 1).map(([k]) => k.toLowerCase()))
  })

  const log = (rid: string): RrcLine[] => {
    const tree = device.get(`s.rrc.log.${rid}`) ?? {}
    const out: RrcLine[] = []
    for (const [k, r] of Object.entries<any>(tree)) {
      if (!r || typeof r !== 'object') continue
      out.push({
        seq: num(k),
        ts: num(r.ts),
        kind: num(r.kind) as LineKind,
        state: num(r.state, LineState.Received) as LineState,
        mention: num(r.mention) === 1,
        own: num(r.own) === 1,
        src: str(r.src).toLowerCase(),
        id: str(r.id),
        nick: str(r.nick),
        text: str(r.text),
      })
    }
    return out.sort((a, b) => a.seq - b.seq)
  }

  /* The hub's notice ring, oldest first. `notes_next` counts every note ever
   * written; slot i was written (notes_next − 1 − i) mod 16 notes ago. */
  const notes = (hub: string): RrcNote[] => {
    const ring = device.get(`rrc.hubs.${hub}.notes`) ?? {}
    const next = num(device.get(`rrc.hubs.${hub}.notes_next`))
    const out: RrcNote[] = []
    for (const [k, raw] of Object.entries(ring)) {
      if (typeof raw !== 'string') continue
      const p1 = raw.indexOf('|')
      const p2 = p1 >= 0 ? raw.indexOf('|', p1 + 1) : -1
      if (p2 < 0) continue
      const age = (((next - 1 - num(k)) % 16) + 16) % 16
      out.push({ ts: num(raw.slice(0, p1)), rid: raw.slice(p1 + 1, p2),
                 text: raw.slice(p2 + 1), order: next - age })
    }
    return out.sort((a, b) => a.order - b.order)
  }

  const presence = computed(() => str(device.get('s.rrc.presence')) || 'collapsed')

  /* The LXMF identities this device holds, then the node's own — read off the
   * lxmf tree, as nomad's ID button does. A slot counts once its destination is
   * up; a group holds a slot too, but nobody chats as a group. */
  const identities = computed<RrcIdentity[]>(() => {
    const out: RrcIdentity[] = []
    const ids = device.get('lxmf.id') ?? {}
    for (const [slot, v] of Object.entries<any>(ids)) {
      const hash = str(v?.dest_hash)
      if (!/^[0-9a-f]{32}$/i.test(hash)) continue
      if (str(device.get(`s.lxmf.id.${slot}.kind`)) === 'group') continue
      const name = str(device.get(`s.lxmf.id.${slot}.display_name`))
      out.push({ sel: `lxmf${slot}`, label: name || `LXMF identity ${slot}`, hash, lxmf: true })
    }
    out.sort((a, b) => a.sel.localeCompare(b.sel, undefined, { numeric: true }))
    out.push({ sel: 'node', label: 'This node',
               hash: str(device.get('rnsd.identity_hash')), lxmf: false })
    return out
  })

  /* ── Commands. Each is one self-clearing key; the task unsets it and acts. ── */
  const cmd = (name: string, val: string) => device.sendJson(nest(`rrc.cmd.${name}`, val))
  const hubArg = (h: string) => { const x = h.trim().toLowerCase(); return isHash(x) ? x : '' }
  /* '|' delimits the fields; only the last may hold one. */
  const noBar = (s: string) => s.replace(/\|/g, '/')

  const add = (hub: string, name = '') => {
    const h = hubArg(hub); if (!h) return
    cmd('add', name ? `${h}|${name}` : h)
  }
  const connect = (hub: string, sel = '') => {
    const h = hubArg(hub); if (!h) return
    cmd('connect', sel ? `${h}|${sel}` : h)
  }
  const disconnect = (hub: string) => { const h = hubArg(hub); if (h) cmd('disconnect', h) }
  const forget = (hub: string) => { const h = hubArg(hub); if (h) cmd('forget', h) }
  const join = (hub: string, room: string, key = '') => {
    const h = hubArg(hub); const r = bareRoom(room)
    if (!h || !r) return
    cmd('join', `${h}|${noBar(r)}${key ? `|${key}` : ''}`)
  }
  const part = (rid: string) => { if (rid) cmd('part', rid) }
  /* The nonce makes two identical lines two sends. */
  const send = (rid: string, text: string) => {
    if (!rid || !text.trim()) return
    cmd('send', `${rid}|${Date.now()}|${text}`)
  }
  const retry = (rid: string, seq: number) => cmd('retry', `${rid}|${seq}`)
  /* Conditional, like lxmf's markConversationRead: a room already read up to
   * here writes nothing. */
  const markRead = (rid: string, seq: number) => {
    const r = roomOf(rid)
    if (!r || seq <= 0) return
    if (seq > r.readSeq || r.unread > 0) cmd('read', `${rid}|${seq}`)
  }
  const setNotify = (rid: string, level: NotifyLevel) => cmd('notify', `${rid}|${level}`)
  const setNick = (hub: string, nick: string) => {
    const h = hubArg(hub); if (h) cmd('nick', `${h}|${nick.trim()}`)
  }
  const ignore = (id12: string, on: boolean) => cmd('ignore', `${id12.toLowerCase()}|${on ? 1 : 0}`)
  const who = (rid: string) => { if (rid) cmd('who', rid) }
  /* 1 while the hub's view is open: the node measures the round trip then. */
  const watchHub = (hub: string, on: boolean) => {
    const h = hubArg(hub); if (!h) return
    if (num(device.get(`rrc.hubs.${h}.watch`)) === (on ? 1 : 0)) return
    device.sendJson(nest(`rrc.hubs.${h}.watch`, on ? 1 : 0))
  }

  /* Hand-offs to the other apps through their shared keys: nothing is called,
   * the other app reacts to the write. The nonce makes a repeat tap fresh. */
  const openLxmf = (hash: string) => {
    const h = hash.trim().toLowerCase()
    if (HASH_RE.test(h)) device.sendJson(nest('lxmf.url_web', `${h}:${Date.now()}`))
  }
  const openNomad = (hash: string, path: string) => {
    const h = hash.trim().toLowerCase()
    if (HASH_RE.test(h))
      device.sendJson(nest('nomad.url_web', `${h}:${path || '/page/index.mu'}|${Date.now()}`))
  }

  return {
    hubs, hubOf, heard, heardOf: (hash) => heardRaw.value.get(hash) ?? null, rooms, roomOf, findRoom, roster, lxmfOf, ignored, log, openRoom: openRid,
    notes, presence, identities,
    draft: (rid) => drafts[rid] ?? '',
    setDraft: (rid, v) => { if (rid) drafts[rid] = v },
    add, connect, disconnect, forget, join, part, send, retry, markRead, setNotify,
    setNick, ignore, who, watchHub, openLxmf, openNomad,
  }
}

export function registerRrc() {
  const device = useDeviceStore()

  /* Another app opening RRC (nomad's rrc:// and rrc@ links) writes
   * "<hub>[/<room>]|<nonce>". A value that arrives with the page's first dump
   * is a request from before this page, and one a re-dump brings back has
   * been handled: each value is acted on once, and only once synced. */
  let handled = ''
  watch(() => str(device.get('rrc.url_web')), (raw) => {
    if (!raw || raw === handled) return
    const fresh = device.synced
    handled = raw
    if (!fresh) return
    const bar = raw.lastIndexOf('|')
    const url = (bar >= 0 ? raw.slice(0, bar) : raw).trim()
    const slash = url.indexOf('/')
    const hub = (slash >= 0 ? url.slice(0, slash) : url).toLowerCase()
    if (!HASH_RE.test(hub)) return
    openRrc(hub, slash >= 0 ? url.slice(slash + 1) : '')
  })

  /* Dock app — foregrounds the RRC window. */
  registerApp({ id: 'rrc', label: 'RRC', icon: 'rrc', placement: 6,
                open: showRrc, isOpen: () => rrcVisible.value })

  registerWindowMount({ id: 'rrc', title: 'RRC', component: RrcWindow,
                        visible: rrcVisible, focusToken: rrcFocus })
}
