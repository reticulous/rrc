<!-- About the open room: its topic, the hub it is on, who we are there, how
     loudly it may notify, and Leave (which forgets the room and its log). -->
<template>
  <div class="scrim" @click.self="emit('close')">
    <div class="sheet">
      <div class="hd">
        <span class="rn">{{ roomLabel(room.name) }}</span>
        <span class="x" title="Close" @click="emit('close')">✕</span>
      </div>
      <div class="sec">
        <div class="k">Topic</div>
        <div class="v">{{ room.topic || 'No topic set' }}</div>
        <div v-if="room.mode" class="v dim">Mode {{ room.mode }}</div>
      </div>
      <div class="sec">
        <div class="k">Hub</div>
        <div class="v link" @click="emit('open-hub', hub.hash)">{{ hub.name || hub.hash.slice(0, 8) }}</div>
        <div class="v mono">{{ groupHex(hub.hash) }}</div>
        <div class="v dim">{{ hub.status }}<template v-if="hub.state === 'live' && hub.rtt > 0"> · round trip {{ (hub.rtt / 1000).toFixed(1) }} s</template></div>
      </div>
      <div v-if="hub.nick" class="sec">
        <div class="v">You are <b>{{ hub.nick }}</b><template v-if="hub.as"> as {{ hub.as }}</template></div>
      </div>
      <div class="sec">
        <div class="k">Notify</div>
        <div class="seg">
          <button v-for="(n, i) in NOTIFY" :key="n.v" :class="{ on: room.notify === i }"
                  @click="emit('notify', n.v)">{{ n.l }}</button>
        </div>
      </div>
      <div class="foot">
        <button class="danger" @click="leave">Leave {{ roomLabel(room.name) }}</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import type { RrcHub, RrcRoom, NotifyLevel } from '../../modules/rrc'
import { roomLabel, groupHex } from '../../lib/rrcText'

const props = defineProps<{ room: RrcRoom; hub: RrcHub }>()
const emit = defineEmits<{
  close: []
  notify: [level: NotifyLevel]
  part: []
  'open-hub': [hash: string]
}>()

/* Index = the room record's `notify` value. */
const NOTIFY: { v: NotifyLevel; l: string }[] = [
  { v: 'all', l: 'All' }, { v: 'mentions', l: 'Mentions' }, { v: 'off', l: 'Off' },
]

function leave() {
  if (!window.confirm(`Leave ${roomLabel(props.room.name)}? What was said there goes from this node too.`)) return
  emit('part')
}
</script>

<style scoped>
.scrim { position: absolute; inset: 0; z-index: 8; background: rgba(0, 0, 0, .45);
         display: flex; align-items: flex-start; justify-content: center; padding-top: 60px; }
.sheet { width: 340px; max-width: calc(100% - 24px); background: #2a2a2a; border: 1px solid #454545; border-radius: 10px;
         padding: 14px 16px; color: #e8e8e8; font-size: calc(13px * var(--rfs, 1));
         box-shadow: 0 24px 60px rgba(0, 0, 0, .6); }
.hd { display: flex; align-items: center; margin-bottom: 8px; }
.rn { font-weight: 700; font-size: calc(16px * var(--rfs, 1)); flex: 1; }
.x { color: #9a9a9a; cursor: pointer; }
.x:hover { color: #fff; }
.sec { padding: 8px 0; border-top: 1px solid #333; }
.k { font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a; letter-spacing: .6px; text-transform: uppercase; margin-bottom: 3px; }
.v { line-height: 1.4; word-break: break-word; }
.dim { color: #9a9a9a; font-size: calc(12px * var(--rfs, 1)); }
.mono { font-family: "DejaVu Sans Mono", Menlo, monospace; color: #9a9a9a; font-size: calc(12px * var(--rfs, 1)); }
.link { color: #7fa6ff; cursor: pointer; }
.seg { display: inline-flex; border: 1px solid #555; border-radius: 6px; overflow: hidden; }
.seg button { background: none; border: none; color: #cfcfcf; padding: 4px 12px; cursor: pointer;
              font-size: calc(12px * var(--rfs, 1)); border-left: 1px solid #555; }
.seg button:first-child { border-left: none; }
.seg button.on { background: #2c6bed; color: #fff; font-weight: 700; }
.foot { border-top: 1px solid #333; padding-top: 10px; display: flex; justify-content: flex-end; }
.danger { background: none; border: 1px solid #8a4545; color: #e09090; border-radius: 6px; padding: 5px 12px;
          cursor: pointer; font-size: calc(12px * var(--rfs, 1)); }
.danger:hover { background: rgba(200, 80, 80, .12); }
</style>
