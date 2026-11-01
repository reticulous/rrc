<!-- One hub in the centre column: its status as the firmware words it, who we
     are there and under what nick, Connect / Disconnect / Forget, the message
     of the day, the hub's notices, and its room directory with a Join button
     on each row. A hub that is only heard (not saved) shows Connect and Save;
     opening it never connects. When the node needs to be told who to chat as
     (state need_ident), the identities are listed and Connect commits the
     choice. -->
<template>
  <div class="hubv">
    <div class="hd">
      <button v-if="showBack" class="back" title="Back" @click="emit('back')">‹</button>
      <div class="ttl">
        <div class="nm">{{ hub.name || hub.hash.slice(0, 8) }}</div>
        <div class="hh">{{ groupHex(hub.hash) }}</div>
      </div>
    </div>

    <div class="body">
      <div class="status">
        <span class="hdot" :class="dot" />
        <span v-if="hub.status">{{ hub.status }}</span>
        <span v-else-if="heard">Heard {{ heard.hops }} hop{{ heard.hops === 1 ? '' : 's' }} away<template v-if="heard.lastSeen">, {{ ago(heard.lastSeen) }} ago</template></span>
        <span v-else class="dim">Not heard on the mesh yet</span>
        <span v-if="hub.state === 'live' && hub.rtt > 0" class="dim"> · round trip {{ (hub.rtt / 1000).toFixed(1) }} s</span>
      </div>

      <div v-if="hub.state === 'need_ident'" class="chooser">
        <div class="k">Chat on this hub as</div>
        <div v-for="i in identities" :key="i.sel" class="idrow" :class="{ on: pick === i.sel }"
             @click="pick = i.sel">
          <span class="radio" />
          <span class="idn">{{ i.label }}</span>
          <span class="idh">{{ i.hash ? i.hash.slice(0, 8) : '—' }}</span>
        </div>
        <div class="hint">The hub knows you by this identity: room ownership, bans and invites key on it.</div>
        <div class="btns">
          <button class="btn p" :disabled="!pick" @click="emit('connect', pick)">Connect</button>
        </div>
      </div>

      <div class="btns">
        <button v-if="connected" class="btn" @click="emit('disconnect')">Disconnect</button>
        <button v-else-if="hub.state !== 'need_ident'" class="btn p" @click="emit('connect', '')">Connect</button>
        <button v-if="!hub.saved" class="btn" title="Keep this hub in the list without connecting"
                @click="emit('add')">Save</button>
        <button v-if="hub.saved" class="btn danger" @click="forget">Forget</button>
      </div>

      <div v-if="hub.saved || hub.as" class="sec">
        <div class="k">You</div>
        <div v-if="hub.as || identLabel" class="v">as {{ hub.as || identLabel }}<template v-if="hub.nick"> · nick <b>{{ hub.nick }}</b></template></div>
        <div class="nickrow">
          <input v-model="nickText" class="in" placeholder="nick on this hub (empty = the default)"
                 spellcheck="false" @keydown.enter="saveNick" />
          <button class="btn" :disabled="nickText.trim() === hub.savedNick" @click="saveNick">Set nick</button>
        </div>
      </div>

      <div v-if="hub.motd" class="sec">
        <div class="k">Message of the day</div>
        <pre class="motd">{{ hub.motd }}</pre>
      </div>

      <div class="sec">
        <div class="k">Rooms</div>
        <div v-if="hub.roomsDir.length === 0" class="dim">
          {{ connected ? 'This hub lists no public rooms.' : 'The hub’s public rooms are listed once connected.' }}
        </div>
        <div v-for="d in hub.roomsDir" :key="d.room" class="dir">
          <div class="dn">
            <div class="rn">{{ roomLabel(d.room) }}</div>
            <div v-if="d.topic" class="tp">{{ d.topic }}</div>
          </div>
          <button v-if="ridOf(d.room)" class="btn" @click="emit('open-room', ridOf(d.room))">Open</button>
          <button v-else class="btn p" @click="emit('join', d.room, '')">Join</button>
        </div>
        <div class="k sub">Join a room</div>
        <div class="joinrow">
          <input v-model="joinRoom" class="in" placeholder="#room" spellcheck="false" @keydown.enter="doJoin" />
          <input v-model="joinKey" class="in key" placeholder="key, if it has one" spellcheck="false"
                 @keydown.enter="doJoin" />
          <button class="btn p" :disabled="!bareRoom(joinRoom)" @click="doJoin">Join</button>
        </div>
        <div class="hint">A room that doesn’t exist yet is made when you join it.</div>
      </div>

      <div v-if="notes.length" class="sec">
        <div class="k">From the hub</div>
        <div v-for="n in notes" :key="n.order" class="note">
          <span class="nt">{{ n.ts ? hhmm(n.ts) : '' }}</span><span class="nx">{{ n.text }}</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import type { RrcHub, RrcHeard, RrcIdentity, RrcNote, RrcRoom } from '../../modules/rrc'
import { groupHex, roomLabel, bareRoom, ago, hhmm } from '../../lib/rrcText'

const props = defineProps<{
  hub: RrcHub
  heard: RrcHeard | null
  identities: RrcIdentity[]
  /* Our rooms on this hub. */
  rooms: RrcRoom[]
  /* The hub's notes for no room in particular, newest first. */
  notes: RrcNote[]
  /* A room named by the link that opened this view, ready to join. */
  prefillRoom: string
  showBack: boolean
}>()
const emit = defineEmits<{
  back: []
  connect: [sel: string]
  disconnect: []
  add: []
  forget: []
  nick: [nick: string]
  join: [room: string, key: string]
  'open-room': [rid: string]
}>()

const connected = computed(() => ['live', 'linking', 'hello', 'retry'].includes(props.hub.state))
const dot = computed(() => {
  switch (props.hub.state) {
    case 'live': return 'up'
    case 'linking': case 'hello': return 'busy'
    case 'retry': case 'held': case 'need_ident': return 'down'
    default: return 'off'
  }
})
/* The saved choice of who to chat as, while not connected. */
const identLabel = computed(() => props.identities.find(i => i.sel === props.hub.ident)?.label ?? '')

const ridOf = (room: string) => props.rooms.find(r => r.name === bareRoom(room))?.rid ?? ''

const pick = ref('')
const nickText = ref('')
const joinRoom = ref('')
const joinKey = ref('')
watch(() => props.hub.hash, () => {
  pick.value = props.hub.ident
  nickText.value = props.hub.savedNick
  joinKey.value = ''
}, { immediate: true })
watch(() => props.prefillRoom, (r) => { joinRoom.value = r ? roomLabel(r) : '' }, { immediate: true })

function saveNick() { emit('nick', nickText.value.trim()) }

function doJoin() {
  const r = bareRoom(joinRoom.value)
  if (!r) return
  emit('join', r, joinKey.value.trim())
  joinRoom.value = ''
  joinKey.value = ''
}

function forget() {
  const name = props.hub.name || props.hub.hash.slice(0, 8)
  if (!window.confirm(`Forget ${name}? Its rooms and everything said in them go from this node.`)) return
  emit('forget')
}
</script>

<style scoped>
.hubv { display: flex; flex-direction: column; height: 100%; min-height: 0; color: #e8e8e8;
        font-size: calc(13px * var(--rfs, 1)); }
.hd { display: flex; align-items: center; gap: 10px; padding: 10px 16px; border-bottom: 1px solid #333; }
.back { background: none; border: none; color: #9a9a9a; font-size: 22px; cursor: pointer; padding: 0 4px; line-height: 1; }
.ttl { min-width: 0; }
.nm { font-weight: 700; font-size: calc(15px * var(--rfs, 1)); }
.hh { font-family: "DejaVu Sans Mono", Menlo, monospace; color: #9a9a9a; font-size: calc(11px * var(--rfs, 1)); }
.body { flex: 1; overflow-y: auto; padding: 12px 16px 20px; }
.status { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.hdot { width: 8px; height: 8px; border-radius: 50%; flex: none; background: #555; }
.hdot.up { background: #4abf6a; }
.hdot.busy { background: #d6a12a; }
.hdot.down { background: #c0504d; }
.dim { color: #9a9a9a; }
.btns { display: flex; gap: 8px; margin: 12px 0 4px; flex-wrap: wrap; }
.btn { background: none; border: 1px solid #555; color: #e8e8e8; border-radius: 6px; padding: 5px 12px;
       font-size: calc(12px * var(--rfs, 1)); cursor: pointer; white-space: nowrap; }
.btn:hover:not(:disabled) { border-color: #777; }
.btn.p { background: #2c6bed; border-color: #2c6bed; color: #fff; font-weight: 700; }
.btn.danger { border-color: #8a4545; color: #e09090; }
.btn:disabled { opacity: .45; cursor: default; }
.sec { margin-top: 14px; padding-top: 10px; border-top: 1px solid #333; }
.k { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; letter-spacing: .6px; text-transform: uppercase; margin-bottom: 6px; }
.k.sub { margin-top: 12px; }
.v { margin-bottom: 6px; }
.in { flex: 1; min-width: 0; background: #1c1c1c; color: #e8e8e8; border: 1px solid #3a3a3a; border-radius: 6px;
      padding: 6px 8px; font-size: calc(12.5px * var(--rfs, 1)); outline: none; font-family: inherit; }
.in:focus { border-color: #2c6bed; }
.in.key { flex: 0.8; }
.nickrow, .joinrow { display: flex; gap: 8px; align-items: center; }
.motd { white-space: pre-wrap; word-break: break-word; margin: 0; font-family: "DejaVu Sans Mono", Menlo, monospace;
        font-size: calc(12px * var(--rfs, 1)); color: #cfcfcf; background: #1c1c1c; border-radius: 6px; padding: 8px 10px;
        max-height: 260px; overflow-y: auto; }
.dir { display: flex; align-items: center; gap: 10px; padding: 5px 0; border-bottom: 1px solid #2c2c2c; }
.dn { flex: 1; min-width: 0; }
.rn { font-weight: 700; }
.tp { color: #9a9a9a; font-size: calc(12px * var(--rfs, 1)); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.hint { color: #777; font-size: calc(11px * var(--rfs, 1)); margin-top: 6px; }
.note { display: flex; gap: 10px; font-family: "DejaVu Sans Mono", Menlo, monospace; font-size: calc(12px * var(--rfs, 1));
        color: #8fa3b6; padding: 2px 0; }
.nt { color: #6a6a6a; flex: none; }
.nx { white-space: pre-wrap; word-break: break-word; }
.chooser { margin-top: 12px; background: #2a2a2a; border: 1px solid #3a3a3a; border-radius: 8px; padding: 10px 12px; }
.idrow { display: flex; align-items: center; gap: 10px; padding: 5px 4px; cursor: pointer; border-radius: 5px; }
.idrow:hover { background: #323232; }
.radio { width: 12px; height: 12px; border-radius: 50%; border: 2px solid #777; flex: none; }
.idrow.on .radio { border-color: #2c6bed; background: #2c6bed; box-shadow: inset 0 0 0 2px #2a2a2a; }
.idn { flex: 1; }
.idh { font-family: "DejaVu Sans Mono", Menlo, monospace; color: #777; font-size: calc(11px * var(--rfs, 1)); }
</style>
