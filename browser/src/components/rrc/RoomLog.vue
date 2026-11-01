<!-- A room's log as IRC lines, not bubbles: a time gutter shown on the first
     line of each minute, the nick in the colour of the person's identity, the
     text with a hanging indent. Actions read "* nick text", notices and the
     hub's replies are dimmed, a run of joins and leaves folds into one grey
     line when s.rrc.presence is "collapsed", a mention has an amber edge, and
     our own lines carry their delivery state (a failed one retries on click).
     The "N new" divider sits where the room had been read up to when it was
     opened. Reading the newest line at the bottom, with the window showing,
     emits `read` with its seq. -->
<template>
  <div ref="scroller" class="log" @scroll="onScroll">
    <div v-if="rows.length === 0" class="empty">Nothing said here yet while this node was listening.</div>
    <div class="grid" :class="{ compact }">
    <template v-for="r in rows" :key="r.key">
      <div v-if="r.t === 'day'" class="day"><span>{{ r.label }}</span></div>
      <div v-else-if="r.t === 'new'" class="wnew">{{ r.count }} new</div>
      <template v-else>
      <div v-if="compact && r.gutter" class="tline">{{ r.gutter }}</div>
      <div v-if="r.t === 'pres'" class="wl pres">
        <span v-if="!compact" class="g">{{ r.gutter }}</span><span class="nk">⋯</span><span class="x">{{ r.text }}</span>
      </div>
      <div v-else-if="r.t === 'note'" class="wl note">
        <span v-if="!compact" class="g">{{ r.gutter }}</span><span class="nk">hub</span><span class="x">{{ r.text }}</span>
      </div>
      <div v-else class="wl" :class="lineClass(r.line)">
        <span v-if="!compact" class="g">{{ r.gutter }}</span>
        <span v-if="r.line.kind === LineKind.Action" class="nk" :style="{ color: r.colour }">*</span>
        <span v-else class="nk nkc" :style="{ color: r.colour }" :title="r.line.src"
              @click="emit('nick', r.line.src, r.line.nick)">{{ r.line.nick || r.line.src.slice(0, 8) }}</span>
        <span class="x">
          <b v-if="r.line.kind === LineKind.Action" class="nkc" :style="{ color: r.colour }"
             @click="emit('nick', r.line.src, r.line.nick)">{{ r.line.nick }}</b><template
            v-if="r.line.kind === LineKind.Action"> </template>
          <template v-for="(s, i) in r.segs" :key="i">
            <span v-if="s.t === 'text'">{{ s.text }}</span>
            <a v-else href="#" @click.prevent="emit('link', s)">{{ s.text }}</a>
          </template>
          <span v-if="r.line.own && r.line.state === LineState.Queued" class="st q" title="Queued">◷</span>
          <span v-else-if="r.line.own && r.line.state === LineState.Relayed" class="st" title="Relayed by the hub">✓</span>
          <span v-else-if="r.line.own && r.line.state === LineState.Failed" class="st f"
                title="Not relayed — click to retry" @click="emit('retry', r.line.seq)">✗ retry</span>
        </span>
      </div>
      </template>
    </template>
    </div>
  </div>
</template>

<script lang="ts">
/* Where each room was left: the scroll offset to come back to, or null for
 * "at the newest", which follows whatever arrived meanwhile. Module scope so it
 * outlives the pane (the compact layout unmounts it); a reload starts every
 * room at its newest line. */
const leftAt = new Map<string, number | null>()
</script>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { LineKind, LineState, type RrcLine, type RrcNote } from '../../modules/rrc'
import { segmentLine, foldPresence, hhmm, dayKey, dayLabel, nickColour, type Segment } from '../../lib/rrcText'

const props = defineProps<{
  rid: string
  lines: RrcLine[]
  /* The hub's notes and command replies that belong to this room. */
  notes: RrcNote[]
  presence: string
  /* read_seq as it was when the room was opened: the "N new" divider. */
  newMark: number
  hueOf: (src: string) => number
  /* The window is showing this pane. */
  active: boolean
  /* Phone layout: the time goes on a line of its own above the lines it
   * stamps, leaving the width to the names and the text. */
  compact?: boolean
}>()
const emit = defineEmits<{
  read: [seq: number]
  retry: [seq: number]
  link: [seg: Segment]
  nick: [src: string, nick: string]
}>()

type Row =
  | { t: 'day'; key: string; label: string }
  | { t: 'new'; key: string; count: number }
  | { t: 'pres'; key: string; gutter: string; text: string }
  | { t: 'note'; key: string; gutter: string; text: string }
  | { t: 'line'; key: string; gutter: string; line: RrcLine; colour: string; segs: Segment[] }

const counts = (l: RrcLine) => !l.own && l.kind <= LineKind.Notice

const rows = computed<Row[]>(() => {
  const out: Row[] = []
  const notes = props.notes
  let ni = 0
  let lastMin = -1
  let lastDay = ''
  const gutter = (ts: number, key: string) => {
    const d = dayKey(ts)
    if (ts && d !== lastDay) { lastDay = d; out.push({ t: 'day', key: `d${key}`, label: dayLabel(ts) }) }
    const m = Math.floor(ts / 60)
    if (m === lastMin) return ''
    lastMin = m
    return ts ? hhmm(ts) : ''
  }
  let run: RrcLine[] = []
  const flushRun = () => {
    if (!run.length) return
    const first = run[0]!
    out.push({ t: 'pres', key: `p${first.seq}`, gutter: gutter(first.ts, `p${first.seq}`), text: foldPresence(run) })
    run = []
  }
  /* A note goes after the line nearest its time: before the first line later
   * than it. */
  const flushNotes = (before: number) => {
    while (ni < notes.length && notes[ni]!.ts < before) {
      flushRun()
      const n = notes[ni++]!
      out.push({ t: 'note', key: `n${n.order}`, gutter: gutter(n.ts, `n${n.order}`), text: n.text })
    }
  }
  const newCount = props.lines.filter(l => l.seq > props.newMark && counts(l)).length
  let newShown = false
  const collapsed = props.presence === 'collapsed'

  for (const l of props.lines) {
    flushNotes(l.ts)
    if (!newShown && newCount > 0 && l.seq > props.newMark && counts(l)) {
      flushRun()
      out.push({ t: 'new', key: 'new', count: newCount })
      newShown = true
    }
    if (l.kind === LineKind.Presence) {
      if (collapsed) { run.push(l); continue }
      out.push({ t: 'pres', key: `p${l.seq}`, gutter: gutter(l.ts, `p${l.seq}`),
                 text: `${l.nick || '?'} ${l.text}` })
      continue
    }
    flushRun()
    out.push({ t: 'line', key: `l${l.seq}`, gutter: gutter(l.ts, `l${l.seq}`), line: l,
               colour: nickColour(props.hueOf(l.src)), segs: segmentLine(l.text) })
  }
  flushRun()
  flushNotes(Infinity)
  return out
})

function lineClass(l: RrcLine) {
  return {
    act: l.kind === LineKind.Action,
    ntc: l.kind === LineKind.Notice,
    sys: l.kind === LineKind.System,
    mn: l.mention,
  }
}

const newest = computed(() => props.lines.length ? props.lines[props.lines.length - 1]!.seq : 0)

const scroller = ref<HTMLElement | null>(null)

function toBottom() {
  nextTick(() => {
    const el = scroller.value
    if (el) el.scrollTop = el.scrollHeight
  })
}
defineExpose({ toBottom })

/* Within 48 px of the bottom counts as looking at the newest. */
function atBottom(): boolean {
  const el = scroller.value
  return !el || el.scrollHeight - el.scrollTop - el.clientHeight <= 48
}
function reading(): boolean {
  return props.active && atBottom() && document.visibilityState === 'visible' && document.hasFocus()
}
function maybeRead() {
  if (props.rid && newest.value > 0 && reading()) emit('read', newest.value)
}

function remember(rid: string) {
  const el = scroller.value
  if (el && rid) leftAt.set(rid, atBottom() ? null : el.scrollTop)
}

watch(() => props.rid, (rid, prev) => {
  if (prev) remember(prev)
  const y = rid ? leftAt.get(rid) : null
  if (y == null) toBottom()
  else nextTick(() => {
    const el = scroller.value
    if (el) el.scrollTop = Math.min(y, el.scrollHeight - el.clientHeight)
  })
  nextTick(maybeRead)
}, { immediate: true })

/* New lines follow to the bottom only when the reader was already there
 * (this runs before the DOM update, so atBottom() is the old position). */
watch(rows, () => {
  if (atBottom()) { toBottom(); nextTick(maybeRead) }
})
watch(() => props.active, (a) => { if (a) nextTick(maybeRead) })

function onScroll() { remember(props.rid); maybeRead() }

onMounted(() => {
  document.addEventListener('visibilitychange', maybeRead)
  window.addEventListener('focus', maybeRead)
})
onBeforeUnmount(() => {
  remember(props.rid)
  document.removeEventListener('visibilitychange', maybeRead)
  window.removeEventListener('focus', maybeRead)
})
</script>

<style scoped>
.log { flex: 1; min-height: 0; overflow-y: auto; padding: 10px 0;
       font-size: calc(13.5px * var(--rfs, 1)); line-height: 1.48; color: #e8e8e8; }
.empty { color: #777; font-style: italic; text-align: center; padding: 24px; font-size: calc(13px * var(--rfs, 1)); }
.day { text-align: center; margin: 10px 0 6px; }
.day span { color: #9a9a9a; font-size: calc(11px * var(--rfs, 1)); background: #2a2a2a; border-radius: 6px; padding: 1px 10px; }
/* Gutter | nick | text, one grid for the whole log so the nick column is as
 * wide as the widest nick on screen (to a cap) and no wider. Each line is a
 * subgrid row: it keeps a box of its own for hover and the mention edge while
 * sharing the log's columns. The text column is the hanging indent. */
.grid { display: grid; grid-template-columns: auto fit-content(calc(14em * var(--rfs, 1))) 1fr; }
.grid.compact { grid-template-columns: fit-content(calc(10em * var(--rfs, 1))) 1fr; }
.grid > * { grid-column: 1 / -1; }
.wl { display: grid; grid-template-columns: subgrid; padding: 1px 16px 1px 0; position: relative; }
.grid.compact .wl { padding-left: 10px; }
.tline { color: #6a6a6a; font-size: calc(11px * var(--rfs, 1)); padding: 4px 10px 0;
         font-variant-numeric: tabular-nums; }
.wl:hover { background: #282828; }
.g { color: #6a6a6a; font-size: calc(11px * var(--rfs, 1)); text-align: right; padding: 2px 12px 0 14px;
     font-variant-numeric: tabular-nums; }
.nk { font-weight: 700; text-align: right; padding-right: 12px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.nkc { cursor: pointer; }
.x { white-space: pre-wrap; word-break: break-word; min-width: 0; }
.wl.act .x { font-style: italic; }
.wl.ntc .x, .wl.ntc .nk { color: #8a8a8a; }
.wl.sys .x { color: #8a8a8a; font-style: italic; }
.wl.pres .x, .wl.pres .nk { color: #777; font-size: calc(12px * var(--rfs, 1)); }
.wl.note .nk { color: #6d7d8c; font-weight: 400; font-size: calc(11px * var(--rfs, 1)); padding-top: 2px; }
.wl.note .x { color: #8fa3b6; font-size: calc(12px * var(--rfs, 1)); font-family: "DejaVu Sans Mono", Menlo, monospace; }
.wl.mn { background: rgba(232, 163, 58, .07); }
.wl.mn .x { box-shadow: inset 2px 0 #e8a33a; padding-left: 6px; margin-left: -8px; }
.st { color: #5aa86f; font-size: calc(11px * var(--rfs, 1)); margin-left: 6px; font-style: normal; }
.st.q { color: #777; }
.st.f { color: #d96a6a; cursor: pointer; }
.st.f:hover { text-decoration: underline; }
a { color: #7fa6ff; text-decoration: none; border-bottom: 1px dotted #7fa6ff; cursor: pointer; }
.wnew { display: flex; align-items: center; gap: 10px; color: #e8a33a; font-size: calc(11px * var(--rfs, 1));
        margin: 6px 16px 6px 10px; }
.grid:not(.compact) > .wnew { grid-column: 3 / -1; margin-left: -8px; }
.wnew::before, .wnew::after { content: ""; flex: 1; height: 1px; background: #5a4220; }
</style>
