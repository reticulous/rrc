<!-- The RRC window. Three columns on a desktop: the rooms rail grouped by hub,
     the centre (a room's topic bar, log and composer, or a hub's view) and the
     member pane, which can be hidden. On a phone the columns slide, as lxmf's
     do: the rail, with the room or hub sweeping in over it, and the members as
     a sheet over the room. This is the only place layout is decided; the
     components below take what modules/rrc.ts computes and say what was
     asked for. -->
<template>
  <FloatingWindow
    id="rrc"
    :title="title"
    :visible="visible"
    :default-geom="defaultGeom"
    :min-size="{ w: 34, h: 20 }"
    @update:visible="v => emit('update:visible', v)"
  >
    <template #titlebar-right>
      <span class="rfs-btn" title="Smaller" @click="zoomOut">-</span>
      <span class="rfs-btn" title="Larger" @click="zoomIn">+</span>
    </template>

    <template #default>
      <div class="rrc" :style="{ '--rfs': scale }">
        <div class="cols" :class="{ mobile: compact, 'show-detail': compact && sel.kind !== '' }">
          <div class="railcol" :style="compact ? undefined : { width: railW + 'px' }">
            <RoomRail :groups="groups" :heard="rrc.heard.value" :sel="sel"
                      @open-room="selectRoom" @open-hub="h => selectHub(h)" @add="onAdd" />
          </div>

          <div class="center">
            <!-- a room -->
            <template v-if="sel.kind === 'room' && room && roomHub">
              <div class="topic" title="Room info" @click="infoOpen = true">
                <button v-if="compact" class="back" title="Back" @click.stop="clearSel">‹</button>
                <span class="rn">{{ roomLabel(room.name) }}</span>
                <span class="tp">{{ room.topic }}</span>
                <span class="hs">{{ roomHub.name || roomHub.hash.slice(0, 8) }}</span>
                <span class="mem" :class="{ on: membersShown }" title="Members"
                      @click.stop="toggleMembers">
                  <q-icon :name="matPeople" size="16px" /> {{ room.members || roster.length }}
                </span>
              </div>
              <div v-if="roomHub.state !== 'live' && roomHub.status" class="hubline"
                   @click="selectHub(roomHub.hash)">{{ roomHub.status }}</div>
              <RoomLog
                ref="logRef"
                :rid="room.rid"
                :lines="lines"
                :notes="roomNotes"
                :presence="rrc.presence.value"
                :new-mark="newMark"
                :hue-of="hueOf"
                :active="logActive"
                :compact="compact"
                @read="seq => rrc.markRead(room!.rid, seq)"
                @retry="seq => rrc.retry(room!.rid, seq)"
                @link="onLink"
                @nick="onNick"
              />
              <Composer
                ref="composerRef"
                :model-value="rrc.draft(room.rid)"
                :placeholder="`Message ${roomLabel(room.name)}`"
                :members="roster"
                :limit="roomHub.limitBody"
                :last-own="lastOwn"
                :error="room.error"
                :hue-of="hueOf"
                @update:model-value="v => rrc.setDraft(room!.rid, v)"
                @send="onSend"
              />
            </template>

            <!-- a hub -->
            <HubView
              v-else-if="sel.kind === 'hub' && selHub"
              :hub="selHub"
              :heard="rrc.heardOf(selHub.hash)"
              :identities="rrc.identities.value"
              :rooms="rrc.rooms.value.filter(r => r.hub === selHub!.hash)"
              :notes="hubNotes"
              :prefill-room="prefillRoom"
              :show-back="compact"
              @back="clearSel"
              @connect="sel2 => rrc.connect(selHub!.hash, sel2)"
              @disconnect="rrc.disconnect(selHub!.hash)"
              @add="rrc.add(selHub!.hash, selHub!.name)"
              @forget="onForget"
              @nick="n => rrc.setNick(selHub!.hash, n)"
              @join="(r, k) => joinAndOpen(selHub!.hash, r, k)"
              @open-room="selectRoom"
            />

            <!-- nothing chosen -->
            <div v-else-if="rrc.rooms.value.length === 0 && sel.kind === ''" class="emptyst">
              <div class="big">No rooms yet</div>
              <div class="dim">Pick a hub, connect, and join a room. This node stays in your rooms while no screen is open.</div>
              <template v-if="rrc.heard.value.length">
                <div class="k">Hubs heard on the mesh</div>
                <div v-for="h in rrc.heard.value" :key="h.hash" class="eh" @click="selectHub(h.hash)">
                  <span class="ehn">{{ h.name || h.hash.slice(0, 8) }}</span>
                  <span class="dim">{{ h.hops }} hop{{ h.hops === 1 ? '' : 's' }}<template v-if="h.lastSeen"> · {{ ago(h.lastSeen) }}</template></span>
                </div>
              </template>
              <div v-else class="dim small">No hub heard announcing yet. A hub can be added by its hash with “+ Add hub”.</div>
            </div>
            <div v-else class="placeholder">Pick a room or a hub.</div>
          </div>

          <div v-if="membersShown && !compact && room" class="memcol">
            <MemberPane :members="roster" :self="roomHub?.self ?? ''" :ignored="rrc.ignored.value"
                        :hue-of="hueOf" @open="openMember" @refresh="rrc.who(room.rid)" />
          </div>
        </div>

        <!-- compact: the members as a sheet over the room -->
        <div v-if="compact && membersShown && room" class="memsheet-bg" @click.self="membersShown = false">
          <div class="memsheet">
            <MemberPane :members="roster" :self="roomHub?.self ?? ''" :ignored="rrc.ignored.value"
                        :hue-of="hueOf" @open="openMember" @refresh="rrc.who(room.rid)" />
          </div>
        </div>

        <MemberCard
          v-if="card && roomHub"
          :member="card"
          :hue="hueOf(card.id12)"
          :hub-name="roomHub.name || roomHub.hash.slice(0, 8)"
          :shared-rooms="sharedRooms(card.id12)"
          :ignored="rrc.ignored.value.has(card.id12)"
          :is-self="card.id12 === roomHub.self"
          @close="card = null"
          @message="onMessage"
          @mention="onMention"
          @ignore="(id, on) => rrc.ignore(id, on)"
        />

        <RoomInfo
          v-if="infoOpen && room && roomHub"
          :room="room"
          :hub="roomHub"
          @close="infoOpen = false"
          @notify="lv => rrc.setNotify(room!.rid, lv)"
          @part="onPart"
          @open-hub="h => { infoOpen = false; selectHub(h) }"
        />
      </div>
    </template>
  </FloatingWindow>
</template>

<script lang="ts">
/* What the window shows, kept across remounts for the session. */
import { reactive as _reactive } from 'vue'
const sel = _reactive<{ kind: '' | 'room' | 'hub'; id: string }>({ kind: '', id: '' })
</script>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue'
import { matPeople } from '@quasar/extras/material-icons'
import FloatingWindow from 'spangap-browser/components/FloatingWindow.vue'
import { useCompact } from 'spangap-browser/lib/viewport'
import { useWinZoom } from 'rns/lib/winZoom'
import RoomRail from '../components/rrc/RoomRail.vue'
import RoomLog from '../components/rrc/RoomLog.vue'
import Composer from '../components/rrc/Composer.vue'
import MemberPane from '../components/rrc/MemberPane.vue'
import MemberCard from '../components/rrc/MemberCard.vue'
import RoomInfo from '../components/rrc/RoomInfo.vue'
import HubView from '../components/rrc/HubView.vue'
import { useRrc, rrcOpenReq, LineKind, type RrcRoom, type RrcMember } from '../modules/rrc'
import { roomLabel, bareRoom, ago, peerAvatar, type Segment, type RrcTarget } from '../lib/rrcText'

const props = defineProps<{ visible: boolean; title: string }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const defaultGeom = { x: 8, y: 6, w: 80, h: 80 }
const rrc = useRrc()
const { scale, zoomIn, zoomOut } = useWinZoom('rrc')
const compact = useCompact()

const railW = 236

/* ── Selection ── */
const prefillRoom = ref('')
/* A room asked for (joined, or tapped as #room) and not in the directory yet:
 * shown as soon as it appears. */
const pending = ref<{ hub: string; room: string } | null>(null)

function selectRoom(rid: string) {
  sel.kind = 'room'; sel.id = rid
  card.value = null
  infoOpen.value = false
}
function selectHub(hash: string, room = '') {
  sel.kind = 'hub'; sel.id = hash
  prefillRoom.value = room
  card.value = null
}
function clearSel() { sel.kind = ''; sel.id = '' }

/* A link or another app naming a hub, and perhaps a room on it: the room if
 * the node keeps it, else the hub with the room ready to join. */
function openTarget(t: RrcTarget) {
  const rid = t.room ? rrc.findRoom(t.hub, t.room) : ''
  if (rid) selectRoom(rid)
  else selectHub(t.hub, t.room)
}
watch(rrcOpenReq, (r) => { if (r) openTarget({ hub: r.hub, room: r.room }) }, { immediate: true })

function joinAndOpen(hub: string, room: string, key = '') {
  const rid = rrc.findRoom(hub, room)
  if (rid) { selectRoom(rid); return }
  rrc.join(hub, room, key)
  pending.value = { hub, room: bareRoom(room) }
}
watch(rrc.rooms, () => {
  const p = pending.value
  if (!p) return
  const rid = rrc.findRoom(p.hub, p.room)
  if (rid) { pending.value = null; selectRoom(rid) }
})

function onAdd(t: RrcTarget) {
  rrc.add(t.hub)
  openTarget(t)
}

function onForget() {
  rrc.forget(sel.id)
  clearSel()
}

/* ── The open room ── */
const room = computed<RrcRoom | null>(() => sel.kind === 'room' ? rrc.roomOf(sel.id) : null)
const roomHub = computed(() => room.value ? rrc.hubOf(room.value.hub) : null)
const selHub = computed(() => sel.kind === 'hub' ? rrc.hubOf(sel.id) : null)

watch(() => (sel.kind === 'room' ? sel.id : ''), (rid) => { rrc.openRoom.value = rid }, { immediate: true })

const lines = computed(() => room.value ? rrc.log(room.value.rid) : [])
const roster = computed(() => room.value ? rrc.roster(room.value.rid) : [])
const notesOfHub = computed(() => room.value ? rrc.notes(room.value.hub)
                                  : selHub.value ? rrc.notes(selHub.value.hash) : [])
const roomNotes = computed(() => room.value ? notesOfHub.value.filter(n => n.rid === room.value!.rid) : [])
const hubNotes = computed(() => notesOfHub.value.filter(n => !n.rid).reverse())

/* Where the room had been read up to when it was opened: the "N new" divider
 * stays put while the reader catches up. Our own line moves it to the end. */
const newMark = ref(0)
watch(() => room.value?.rid, () => { newMark.value = room.value?.readSeq ?? 0 }, { immediate: true })

const lastOwn = computed(() => {
  for (let i = lines.value.length - 1; i >= 0; i--) {
    const l = lines.value[i]!
    if (!l.own) continue
    if (l.kind === LineKind.Action) return `/me ${l.text}`
    if (l.kind === LineKind.Msg) return l.text
  }
  return ''
})

/* The log counts as read only while the window shows it. */
const logActive = computed(() => props.visible && (!compact.value || sel.kind === 'room'))

/* A person's colour is their LXMF avatar's when their address is known. */
function hueOf(src: string): number {
  const hub = room.value?.hub ?? ''
  return peerAvatar(rrc.lxmfOf(hub, src) || src, '').hue
}

const logRef = ref<InstanceType<typeof RoomLog> | null>(null)
const composerRef = ref<InstanceType<typeof Composer> | null>(null)

function onSend(text: string) {
  if (!room.value) return
  rrc.send(room.value.rid, text)
  newMark.value = room.value.seq + 1
  logRef.value?.toBottom()
}

watch(() => room.value?.rid, (rid) => { if (rid && !compact.value) nextTick(() => composerRef.value?.focus()) })

/* ── Links in a line ── */
function onLink(s: Segment) {
  switch (s.t) {
    case 'rrc': openTarget(s.target); break
    case 'lxmf': rrc.openLxmf(s.hash); break
    case 'nomad': rrc.openNomad(s.hash, s.path); break
    case 'room': if (room.value) joinAndOpen(room.value.hub, s.room); break
    case 'web': window.open(s.url, '_blank', 'noopener'); break
  }
}

/* ── Members ── */
const LS_MEMBERS = 'rrc.members'
const membersPref = ref(localStorage.getItem(LS_MEMBERS) !== '0')
const compactMembers = ref(false)
const membersShown = computed({
  get: () => compact.value ? compactMembers.value : membersPref.value,
  set: (v: boolean) => {
    if (compact.value) compactMembers.value = v
    else { membersPref.value = v; localStorage.setItem(LS_MEMBERS, v ? '1' : '0') }
  },
})
function toggleMembers() { membersShown.value = !membersShown.value }

const card = ref<RrcMember | null>(null)
function openMember(m: RrcMember) { card.value = m }
function onNick(src: string, nick: string) {
  card.value = roster.value.find(m => m.id12 === src)
    ?? { id12: src, nick, identity: '', lxmf: rrc.lxmfOf(room.value?.hub ?? '', src) }
}
/* The rooms on this hub where this identity is. */
function sharedRooms(id12: string): string[] {
  const hub = room.value?.hub
  if (!hub) return []
  return rrc.rooms.value.filter(r => r.hub === hub && rrc.roster(r.rid).some(m => m.id12 === id12))
    .map(r => roomLabel(r.name))
}
function onMessage(lxmf: string) {
  card.value = null
  rrc.openLxmf(lxmf)
}
function onMention(nick: string) {
  card.value = null
  if (compact.value) compactMembers.value = false
  composerRef.value?.insert(`@${nick} `)
}

/* ── Room info ── */
const infoOpen = ref(false)
function onPart() {
  if (!room.value) return
  rrc.part(room.value.rid)
  infoOpen.value = false
  clearSel()
}

/* ── The hub view asks the node to measure the round trip while it is open ── */
const watched = computed(() =>
  props.visible && sel.kind === 'hub' && selHub.value?.saved ? sel.id : '')
watch(watched, (cur, prev) => {
  if (prev) rrc.watchHub(prev, false)
  if (cur) rrc.watchHub(cur, true)
}, { immediate: true })
onBeforeUnmount(() => { if (watched.value) rrc.watchHub(watched.value, false) })

/* ── Rail groups: every saved hub, and any hub we have a room on ── */
const groups = computed(() => {
  const byHub = new Map<string, RrcRoom[]>()
  for (const r of rrc.rooms.value) {
    const a = byHub.get(r.hub)
    if (a) a.push(r); else byHub.set(r.hub, [r])
  }
  const out = rrc.hubs.value.filter(h => h.saved || byHub.has(h.hash))
    .map(h => ({ hub: h, rooms: byHub.get(h.hash) ?? [] }))
  for (const [hash, rs] of byHub) {
    if (out.some(g => g.hub.hash === hash)) continue
    const h = rrc.hubOf(hash)
    if (h) out.push({ hub: h, rooms: rs })
  }
  return out
})
</script>

<style scoped>
.rrc { position: relative; height: 100%; display: flex; flex-direction: column; background: #1d1d1d;
       overflow: hidden; color: #e8e8e8; }
.rfs-btn {
  display: inline-flex; align-items: center; justify-content: center;
  width: 18px; height: 18px; border-radius: 4px; font-size: 14px; font-weight: 700;
  color: rgba(255,255,255,0.5); cursor: pointer; font-family: system-ui; line-height: 1;
  user-select: none;
}
.rfs-btn:hover { color: rgba(255,255,255,0.9); background: rgba(255,255,255,0.1); }
.cols { flex: 1; display: flex; min-height: 0; }
.railcol { flex: none; border-right: 1px solid #2f2f2f; overflow: hidden; }
.center { flex: 1; min-width: 0; display: flex; flex-direction: column; background: #232323; }
.memcol { flex: none; width: calc(200px * var(--rfs, 1)); max-width: 30%; border-left: 1px solid #333; overflow: hidden; }

/* Compact: one column that slides — the rail, with the room or hub sweeping
 * in over it, the rail easing left underneath. */
.cols.mobile { position: relative; display: block; overflow: hidden; }
.cols.mobile .railcol, .cols.mobile .center {
  position: absolute; inset: 0; width: 100%; transition: transform 0.25s ease;
}
.cols.mobile .railcol { border-right: none; transform: translateX(0); }
.cols.mobile .center { transform: translateX(100%); z-index: 2; }
.cols.mobile.show-detail .railcol { transform: translateX(-25%); }
.cols.mobile.show-detail .center { transform: translateX(0); }

.topic { padding: 10px 16px; border-bottom: 1px solid #333; display: flex; gap: 10px; align-items: baseline;
         cursor: pointer; font-size: calc(13px * var(--rfs, 1)); }
.topic:hover { background: #262626; }
.back { background: none; border: none; color: #9a9a9a; font-size: 22px; cursor: pointer; padding: 0 4px;
        line-height: 1; align-self: center; }
.rn { font-weight: 700; font-size: calc(15px * var(--rfs, 1)); white-space: nowrap; }
.tp { color: #9a9a9a; flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.hs { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; white-space: nowrap; }
.mem { display: inline-flex; align-items: center; gap: 3px; color: #9a9a9a; font-size: calc(12px * var(--rfs, 1));
       padding: 1px 6px; border-radius: 5px; align-self: center; white-space: nowrap; }
.mem:hover { background: rgba(255,255,255,0.08); color: #e8e8e8; }
.mem.on { color: #e8e8e8; }
.hubline { padding: 4px 16px; background: #2a2a2a; color: #d6a12a; font-size: calc(12px * var(--rfs, 1));
           border-bottom: 1px solid #333; cursor: pointer; }
.placeholder { flex: 1; display: flex; align-items: center; justify-content: center; color: #777;
               font-size: calc(13px * var(--rfs, 1)); font-style: italic; }
.emptyst { flex: 1; overflow-y: auto; padding: 40px 32px; font-size: calc(13px * var(--rfs, 1)); }
.big { font-size: calc(17px * var(--rfs, 1)); font-weight: 700; margin-bottom: 6px; }
.dim { color: #9a9a9a; }
.small { margin-top: 18px; font-size: calc(12px * var(--rfs, 1)); }
.k { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; letter-spacing: .8px; text-transform: uppercase;
     margin: 22px 0 6px; }
.eh { display: flex; gap: 12px; align-items: baseline; padding: 6px 10px; border-radius: 6px; cursor: pointer;
      max-width: 420px; }
.eh:hover { background: #2a2a2a; }
.ehn { flex: 1; font-weight: 700; }
.memsheet-bg { position: absolute; inset: 0; z-index: 7; background: rgba(0,0,0,0.45);
               display: flex; justify-content: flex-end; }
.memsheet { width: 75%; max-width: 280px; height: 100%; }
</style>
