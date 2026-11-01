<!-- The line being typed into a room. Enter sends; Up in an empty field brings
     back our last line; `@` and Tab complete nicks from the room's members.
     A byte counter appears only past 80 % of the hub's body limit, and a line
     over it can't be sent: the firmware refuses it rather than split it. The
     draft lives in the state layer, per room, for the session. -->
<template>
  <div class="comp">
    <div v-if="error" class="err">{{ error }}</div>
    <div class="row">
      <div class="fwrap">
        <div v-if="ac.open && cands.length" class="ac">
          <div v-for="(c, i) in cands" :key="c.id12" :class="{ on: i === ac.idx }"
               @mousedown.prevent="accept(i)">
            <span class="acn" :style="{ color: nickColour(hueOf(c.id12)) }">{{ c.nick }}</span>
            <span class="ach">{{ c.id12.slice(0, 4) }}</span>
          </div>
        </div>
        <input ref="inp" v-model="text" class="f" :placeholder="placeholder"
               spellcheck="true" autocomplete="off"
               @keydown="onKey" @input="onInput" @blur="ac.open = false" />
      </div>
      <span v-if="showCount" class="cnt" :class="{ over }">{{ bytes }}/{{ limit }}</span>
      <button class="send" :disabled="!canSend" @click="doSend">Send</button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, reactive, ref } from 'vue'
import type { RrcMember } from '../../modules/rrc'
import { utf8Bytes, nickColour } from '../../lib/rrcText'

const props = defineProps<{
  modelValue: string
  placeholder: string
  members: RrcMember[]
  /* The hub's body limit in bytes; 0 = not known yet. */
  limit: number
  /* Our last line in the room, as it would be typed again. */
  lastOwn: string
  error: string
  hueOf: (id12: string) => number
}>()
const emit = defineEmits<{
  'update:modelValue': [v: string]
  send: [text: string]
}>()

const text = computed({
  get: () => props.modelValue,
  set: v => emit('update:modelValue', v),
})
const inp = ref<HTMLInputElement | null>(null)

/* What reaches the hub: "/me " and the "//" escape are the device's to strip. */
const body = computed(() => {
  const t = text.value
  if (t.startsWith('//')) return t.slice(1)
  if (t.startsWith('/me ')) return t.slice(4)
  return t
})
const bytes = computed(() => utf8Bytes(body.value))
const showCount = computed(() => props.limit > 0 && bytes.value > props.limit * 0.8)
const over = computed(() => props.limit > 0 && bytes.value > props.limit)
const canSend = computed(() => text.value.trim().length > 0 && !over.value)

function doSend() {
  if (!canSend.value) return
  emit('send', text.value)
  text.value = ''
  ac.open = false
}

/* ── Nick completion ── the word before the caret: "@pre" opens the list as
 * it is typed; Tab opens it for a bare prefix (or completes a single match). */
const ac = reactive({ open: false, idx: 0, start: 0, end: 0, at: false, prefix: '' })

const cands = computed(() => {
  const p = ac.prefix.toLowerCase()
  const seen = new Set<string>()
  return props.members.filter((m) => {
    if (!m.nick || seen.has(m.nick)) return false
    seen.add(m.nick)
    return m.nick.toLowerCase().startsWith(p)
  }).slice(0, 8)
})

function wordAtCaret(): { start: number; end: number; at: boolean; prefix: string } | null {
  const el = inp.value
  if (!el) return null
  const end = el.selectionStart ?? text.value.length
  const before = text.value.slice(0, end)
  const m = /(^|\s)(@?)([^\s@]*)$/.exec(before)
  if (!m) return null
  const start = end - m[2]!.length - m[3]!.length
  return { start, end, at: m[2] === '@', prefix: m[3]! }
}

function openAt(w: { start: number; end: number; at: boolean; prefix: string }) {
  Object.assign(ac, w, { open: true, idx: 0 })
}

function onInput() {
  const w = wordAtCaret()
  if (w && w.at) openAt(w)
  else if (ac.open && w && !ac.at) Object.assign(ac, w)
  else ac.open = false
}

function accept(i: number) {
  const c = cands.value[i]
  if (!c) return
  const t = text.value
  const ins = ac.at ? `@${c.nick} ` : ac.start === 0 ? `${c.nick}: ` : `${c.nick} `
  text.value = t.slice(0, ac.start) + ins + t.slice(ac.end).replace(/^ /, '')
  ac.open = false
  const pos = ac.start + ins.length
  requestAnimationFrame(() => { inp.value?.setSelectionRange(pos, pos) })
}

function onKey(e: KeyboardEvent) {
  if (ac.open && cands.value.length) {
    if (e.key === 'ArrowDown') { ac.idx = (ac.idx + 1) % cands.value.length; e.preventDefault(); return }
    if (e.key === 'ArrowUp') { ac.idx = (ac.idx + cands.value.length - 1) % cands.value.length; e.preventDefault(); return }
    if (e.key === 'Tab' || e.key === 'Enter') { accept(ac.idx); e.preventDefault(); return }
    if (e.key === 'Escape') { ac.open = false; e.preventDefault(); return }
  }
  if (e.key === 'Tab' && !e.shiftKey) {
    const w = wordAtCaret()
    if (w && (w.prefix || w.at)) {
      e.preventDefault()
      openAt(w)
      if (cands.value.length === 1) accept(0)
    }
    return
  }
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); doSend(); return }
  if (e.key === 'ArrowUp' && !text.value && props.lastOwn) {
    e.preventDefault()
    text.value = props.lastOwn
  }
}

/* Room switches and "Mention" put text here and hand over the cursor. */
function focus() { inp.value?.focus() }
function insert(s: string) {
  const t = text.value
  text.value = t && !t.endsWith(' ') ? `${t} ${s}` : t + s
  requestAnimationFrame(() => {
    const el = inp.value
    if (!el) return
    el.focus()
    el.setSelectionRange(el.value.length, el.value.length)
  })
}
defineExpose({ focus, insert })
</script>

<style scoped>
.comp { border-top: 1px solid #333; padding: 10px 14px; }
.err { color: #e09b6a; font-size: calc(12px * var(--rfs, 1)); margin-bottom: 6px; }
.row { display: flex; gap: 10px; align-items: center; }
.fwrap { flex: 1; position: relative; min-width: 0; }
.f { width: 100%; background: #1c1c1c; color: #e8e8e8; border: 1px solid #3a3a3a; border-radius: 6px;
     padding: 8px 10px; font-size: calc(13.5px * var(--rfs, 1)); outline: none; font-family: inherit; }
.f:focus { border-color: #2c6bed; }
.cnt { color: #e8a33a; font-size: calc(11px * var(--rfs, 1)); white-space: nowrap; font-variant-numeric: tabular-nums; }
.cnt.over { color: #e05555; font-weight: 700; }
.send { background: #2c6bed; color: #fff; border: none; border-radius: 6px; padding: 7px 14px; font-weight: 700;
        font-size: calc(13px * var(--rfs, 1)); cursor: pointer; }
.send:disabled { background: #3a3a3a; color: #777; cursor: default; }
.ac { position: absolute; left: 0; bottom: calc(100% + 4px); background: #2f2f2f; border: 1px solid #444;
      border-radius: 6px; padding: 4px 0; font-size: calc(13px * var(--rfs, 1)); z-index: 5;
      box-shadow: 0 10px 24px rgba(0, 0, 0, .5); min-width: 170px; }
.ac div { padding: 3px 10px; cursor: pointer; display: flex; gap: 8px; align-items: baseline; }
.ac div.on, .ac div:hover { background: #2c3443; }
.acn { font-weight: 700; }
.ach { color: #777; font-size: calc(11px * var(--rfs, 1)); margin-left: auto; }
</style>
