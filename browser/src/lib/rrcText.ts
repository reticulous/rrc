/**
 * Pure text helpers for the RRC window: nick colours, link segmentation,
 * hash grouping, times and the join/leave fold. No storage access — the state
 * layer (modules/rrc.ts) is the only reader of the device mirror.
 */

/** A room name as typed or linked, in the form the firmware keys rooms by:
 *  trimmed, lowercased, without a leading '#'. */
export function bareRoom(name: string): string {
  return name.trim().replace(/^#+/, '').toLowerCase()
}

/** How a room is shown everywhere: "#" + its wire name. */
export function roomLabel(name: string): string {
  return `#${bareRoom(name)}`
}

/** lxmf's deterministic avatar descriptor, the same function as
 *  lxmf/modules/lxmf.ts peerAvatar(): hue from the first eight hex characters,
 *  glyph = up to two initials. Fed the person's lxmf.delivery address when it
 *  is known, so a nick here is the colour of their avatar in LXMF. */
export function peerAvatar(peer: string, name: string): { hue: number; glyph: string } {
  let h = 0
  for (let i = 0; i < Math.min(peer.length, 8); i++) h = (h * 31 + peer.charCodeAt(i)) >>> 0
  const named = name && !name.startsWith('(')
  const glyph = named
    ? name.trim().split(/\s+/).slice(0, 2).map(w => w[0]!.toUpperCase()).join('')
    : peer.slice(0, 2).toUpperCase()
  return { hue: h % 360, glyph: glyph || '?' }
}

/** Nick text colour for a hue, legible on the #232323 log. */
export function nickColour(hue: number): string {
  return `hsl(${hue} 60% 68%)`
}

/** "0c4d11fe93aa…" → "0c4d 11fe 93aa …" */
export function groupHex(hex: string): string {
  return (hex.match(/.{1,4}/g) ?? []).join(' ')
}

export function utf8Bytes(s: string): number {
  return new TextEncoder().encode(s).length
}

export function hhmm(ts: number): string {
  const d = new Date(ts * 1000)
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`
}

export function dayKey(ts: number): string {
  const d = new Date(ts * 1000)
  return `${d.getFullYear()}-${d.getMonth()}-${d.getDate()}`
}

export function dayLabel(ts: number): string {
  return new Date(ts * 1000).toLocaleDateString(undefined,
    { weekday: 'long', day: 'numeric', month: 'long' })
}

/** Age of a unix-seconds instant, short ("40 s", "5 min", "3 h", "2 d"). */
export function ago(unixS: number): string {
  if (!unixS) return ''
  const s = Math.max(0, Math.round(Date.now() / 1000 - unixS))
  if (s < 60) return `${s} s`
  if (s < 3600) return `${Math.floor(s / 60)} min`
  if (s < 86400) return `${Math.floor(s / 3600)} h`
  return `${Math.floor(s / 86400)} d`
}

/** A run of join/leave lines folded into one: "dana and 2 others joined ·
 *  ollie left". A name that both joined and left in the run counts once, under
 *  what it did last. */
export function foldPresence(run: { nick: string; text: string }[]): string {
  const last = new Map<string, string>()
  for (const r of run) {
    const n = r.nick || '?'
    last.delete(n)
    last.set(n, r.text)
  }
  const joined: string[] = [], left: string[] = []
  for (const [n, t] of last) (t === 'left' ? left : joined).push(n)
  const names = (a: string[]) =>
    a.length === 1 ? a[0]!
    : a.length === 2 ? `${a[0]} and ${a[1]}`
    : a.length === 3 ? `${a[0]}, ${a[1]} and ${a[2]}`
    : `${a[0]} and ${a.length - 1} others`
  const parts: string[] = []
  if (joined.length) parts.push(`${names(joined)} joined`)
  if (left.length) parts.push(`${names(left)} left`)
  return parts.join(' · ')
}

/* ── Links ─────────────────────────────────────────────────────────────── */

const HEX32 = /^[0-9a-f]{32}$/

/** Where an RRC link points: a hub, and a room on it ('' = the hub itself). */
export interface RrcTarget { hub: string; room: string }

/** `rrc://<hub>[:<dest>]/<room>`, `rrc@<hub>` or a bare 32-hex hub hash.
 *  The hub is the first 32-hex part; `:<dest>` names a destination of the
 *  same hub and is taken as the hub only when the first part isn't a hash. */
export function parseRrcLink(raw: string): RrcTarget | null {
  const s = raw.trim()
  let m = /^rrc@([0-9a-fA-F]{32})$/.exec(s)
  if (m) return { hub: m[1]!.toLowerCase(), room: '' }
  m = /^rrc:\/\/([^/:\s]*)(?::([^/\s]*))?(?:\/(\S*))?$/.exec(s)
  if (m) {
    const a = (m[1] ?? '').toLowerCase(), b = (m[2] ?? '').toLowerCase()
    const hub = HEX32.test(a) ? a : HEX32.test(b) ? b : ''
    if (!hub) return null
    let room = m[3] ?? ''
    try { room = decodeURIComponent(room) } catch { /* taken as written */ }
    return { hub, room: bareRoom(room.replace(/\/+$/, '')) }
  }
  if (HEX32.test(s.toLowerCase())) return { hub: s.toLowerCase(), room: '' }
  return null
}

export type Segment =
  | { t: 'text'; text: string }
  | { t: 'rrc'; text: string; target: RrcTarget }
  | { t: 'lxmf'; text: string; hash: string }
  | { t: 'nomad'; text: string; hash: string; path: string }
  | { t: 'room'; text: string; room: string }
  | { t: 'web'; text: string; url: string }

/** Split a line into plain runs and tappable links: rrc:// and rrc@ (open in
 *  RRC), lxmf@<hash> (LXMF), <hash>:/page/… (Nomad), http(s) URLs, and a bare
 *  #room token (that room on the line's hub). Trailing sentence punctuation is
 *  kept outside a link. */
export function segmentLine(text: string): Segment[] {
  const re = /(rrc:\/\/\S+)|(rrc@[0-9a-fA-F]{32})|(lxmf@[0-9a-fA-F]{32})|(https?:\/\/\S+)|([0-9a-fA-F]{32}):(\/\S+)|(^|[\s(,])(#[A-Za-z0-9][\w.\-]*)/g
  const trailing = /[.,;:!?)\]}'"]+$/
  const out: Segment[] = []
  let last = 0
  let m: RegExpExecArray | null
  const push = (start: number, seg: Segment) => {
    if (start > last) out.push({ t: 'text', text: text.slice(last, start) })
    out.push(seg)
    last = start + seg.text.length
    re.lastIndex = last
  }
  while ((m = re.exec(text))) {
    if (m[1]) {
      const s = m[1].replace(trailing, '')
      const target = parseRrcLink(s)
      if (target) push(m.index, { t: 'rrc', text: s, target })
    } else if (m[2]) {
      push(m.index, { t: 'rrc', text: m[2], target: { hub: m[2].slice(4).toLowerCase(), room: '' } })
    } else if (m[3]) {
      push(m.index, { t: 'lxmf', text: m[3], hash: m[3].slice(5).toLowerCase() })
    } else if (m[4]) {
      const url = m[4].replace(trailing, '')
      if (url) push(m.index, { t: 'web', text: url, url })
    } else if (m[5]) {
      /* Not inside a longer hex run (a 64-hex id). */
      if (m.index > 0 && /[0-9a-fA-F]/.test(text[m.index - 1]!)) continue
      const path = m[6]!.replace(trailing, '')
      if (path) push(m.index, { t: 'nomad', text: `${m[5]}:${path}`,
                                hash: m[5].toLowerCase(), path })
    } else if (m[8]) {
      const tok = m[8].replace(/[.\-]+$/, '')
      if (tok.length > 1) push(m.index + m[7]!.length, { t: 'room', text: tok, room: bareRoom(tok) })
    }
  }
  if (last < text.length) out.push({ t: 'text', text: text.slice(last) })
  return out
}
