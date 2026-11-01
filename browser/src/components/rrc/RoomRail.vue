<!-- The left rail: rooms grouped under their hub, then the hubs heard on the
     mesh and not yet saved, then "+ Add hub". A hub header carries a status
     dot and the round trip, or the firmware's status text when it is not up.
     Every row only selects what the centre column shows; nothing here
     connects or joins. -->
<template>
  <div class="rail">
    <div class="lbl">ROOMS</div>
    <div v-if="groups.length === 0" class="none">No hubs yet</div>
    <template v-for="g in groups" :key="g.hub.hash">
      <div class="hub" :class="{ on: sel.kind === 'hub' && sel.id === g.hub.hash }"
           :title="g.hub.hash" @click="emit('open-hub', g.hub.hash)">
        <span class="hdot" :class="dotClass(g.hub)" />
        <span class="hn">{{ g.hub.name || g.hub.hash.slice(0, 8) }}</span>
        <span class="rt">{{ hubRight(g.hub) }}</span>
      </div>
      <div v-for="r in g.rooms" :key="r.rid"
           class="rm" :class="{ on: sel.kind === 'room' && sel.id === r.rid, out: r.state !== 'in',
                                quiet: r.notify === 2 }"
           :title="r.preview" @click="emit('open-room', r.rid)">
        <span class="rn">{{ roomLabel(r.name) }}</span>
        <span class="sp" />
        <span v-if="r.mentions > 0" class="at">@</span>
        <span v-if="r.unread > 0" class="pill" :class="{ dim: r.notify === 2 }">{{ r.unread }}</span>
      </div>
    </template>

    <template v-if="heard.length">
      <div class="lbl mesh">ON THE MESH</div>
      <div v-for="h in heard" :key="h.hash"
           class="heard" :class="{ on: sel.kind === 'hub' && sel.id === h.hash }"
           :title="h.hash" @click="emit('open-hub', h.hash)">
        <span class="hn">{{ h.name || h.hash.slice(0, 8) }}</span>
        <span class="sp" />
        <span class="meta">{{ h.hops }} hop{{ h.hops === 1 ? '' : 's' }}<template v-if="h.lastSeen"> · {{ ago(h.lastSeen) }}</template></span>
      </div>
    </template>

    <div v-if="!adding" class="add" @click="startAdd">+ Add hub</div>
    <div v-else class="addbox">
      <input ref="addEl" v-model="addText" class="addin" placeholder="hub hash or rrc:// link"
             spellcheck="false" autocomplete="off"
             @keydown.enter="doAdd" @keydown.esc="adding = false" />
      <div v-if="addErr" class="adderr">{{ addErr }}</div>
      <div class="addbtns">
        <button class="btn" @click="adding = false">Cancel</button>
        <button class="btn p" :disabled="!addText.trim()" @click="doAdd">Add</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { nextTick, ref } from 'vue'
import type { RrcHub, RrcRoom, RrcHeard } from '../../modules/rrc'
import { roomLabel, ago, parseRrcLink, type RrcTarget } from '../../lib/rrcText'

defineProps<{
  groups: { hub: RrcHub; rooms: RrcRoom[] }[]
  heard: RrcHeard[]
  sel: { kind: '' | 'room' | 'hub'; id: string }
}>()
const emit = defineEmits<{
  'open-room': [rid: string]
  'open-hub': [hash: string]
  add: [target: RrcTarget]
}>()

function dotClass(h: RrcHub): string {
  switch (h.state) {
    case 'live': return 'up'
    case 'linking': case 'hello': return 'busy'
    case 'retry': case 'held': case 'need_ident': return 'down'
    default: return 'off'
  }
}

/* Round trip while up; the firmware's sentence otherwise. */
function hubRight(h: RrcHub): string {
  if (h.state === 'live') return h.rtt > 0 ? `${(h.rtt / 1000).toFixed(1)} s` : ''
  return h.status
}

const adding = ref(false)
const addText = ref('')
const addErr = ref('')
const addEl = ref<HTMLInputElement | null>(null)

function startAdd() {
  adding.value = true
  addText.value = ''
  addErr.value = ''
  nextTick(() => addEl.value?.focus())
}

function doAdd() {
  const t = parseRrcLink(addText.value)
  if (!t) { addErr.value = 'A hub is 32 hex characters, or an rrc:// link.'; return }
  adding.value = false
  emit('add', t)
}
</script>

<style scoped>
.rail { height: 100%; overflow-y: auto; background: #1c1c1c; padding-bottom: 12px;
        font-size: calc(13px * var(--rfs, 1)); color: #e8e8e8; user-select: none; }
.lbl { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; letter-spacing: .8px; padding: 12px 14px 4px; }
.lbl.mesh { margin-top: 10px; }
.none { color: #777; font-style: italic; padding: 4px 14px; font-size: calc(12px * var(--rfs, 1)); }
.hub { display: flex; align-items: center; gap: 8px; padding: 7px 14px 3px; font-weight: 700; cursor: pointer; }
.hub:hover, .rm:hover, .heard:hover { background: #262626; }
.hub.on, .heard.on { background: #2c3443; }
.hn { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; min-width: 0; }
.rt { margin-left: auto; font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; font-weight: 400;
      white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 55%; }
.hdot { width: 8px; height: 8px; border-radius: 50%; flex: none; background: #555; }
.hdot.up { background: #4abf6a; }
.hdot.busy { background: #d6a12a; }
.hdot.down { background: #c0504d; }
.rm { display: flex; align-items: center; gap: 6px; padding: 4px 14px 4px 30px; color: #cfcfcf; cursor: pointer; }
.rm.on { background: #2c3443; color: #fff; box-shadow: inset 3px 0 0 #2c6bed; }
.rm.out { color: #777; }
.rn { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; min-width: 0; }
.sp { flex: 1; }
.at { color: #e8a33a; font-weight: 700; font-size: calc(12px * var(--rfs, 1)); }
.pill { background: #2c6bed; color: #fff; border-radius: 9px; padding: 0 6px; font-size: calc(11px * var(--rfs, 1));
        font-weight: 700; line-height: calc(17px * var(--rfs, 1)); }
.pill.dim { background: #444; color: #bbb; }
.heard { display: flex; align-items: center; gap: 6px; padding: 3px 14px 3px 30px; color: #9a9a9a;
         font-size: calc(12px * var(--rfs, 1)); cursor: pointer; }
.meta { white-space: nowrap; font-size: calc(11px * var(--rfs, 1)); }
.add { padding: 8px 14px; color: #7fa6ff; cursor: pointer; margin-top: 6px; }
.add:hover { color: #a9c2ff; }
.addbox { padding: 8px 12px; }
.addin { width: 100%; background: #232323; color: #e8e8e8; border: 1px solid #3a3a3a; border-radius: 6px;
         padding: 6px 8px; font-size: calc(12px * var(--rfs, 1)); outline: none; font-family: inherit; }
.addin:focus { border-color: #2c6bed; }
.adderr { color: #d98a8a; font-size: calc(11px * var(--rfs, 1)); margin-top: 4px; }
.addbtns { display: flex; justify-content: flex-end; gap: 6px; margin-top: 6px; }
.btn { background: none; border: 1px solid #555; color: #e8e8e8; border-radius: 6px; padding: 3px 10px;
       font-size: calc(12px * var(--rfs, 1)); cursor: pointer; }
.btn.p { background: #2c6bed; border-color: #2c6bed; font-weight: 700; color: #fff; }
.btn:disabled { opacity: .5; cursor: default; }
</style>
