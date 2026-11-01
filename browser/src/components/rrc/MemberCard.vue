<!-- A person in a room: nick, identity hash in fours, their LXMF address and
     the rooms on this hub where we share their company. "Message on LXMF"
     hands the address to lxmf; "Mention" puts @nick into the composer;
     "Ignore" stops the node logging them. A person is their identity — every
     action here keys on it, never on the nick. -->
<template>
  <div class="scrim" @click.self="emit('close')">
    <div class="card">
      <div class="top">
        <div class="av" :style="avStyle">{{ glyph }}</div>
        <div class="who">
          <div class="nm" :style="{ color: nickColour(hue) }">{{ member.nick || 'no nick' }}</div>
          <div class="hh">{{ groupHex(member.identity || member.id12) }}<template v-if="!member.identity"> …</template></div>
        </div>
        <span class="x" title="Close" @click="emit('close')">✕</span>
      </div>
      <div class="meta">
        <div v-if="sharedRooms.length">In {{ sharedRooms.join(', ') }} on {{ hubName }}</div>
        <div v-if="member.lxmf">LXMF <span class="mono">{{ groupHex(member.lxmf) }}</span></div>
        <div v-else class="dim">LXMF address not known until they say something</div>
        <div v-if="isSelf" class="dim">This is you.</div>
      </div>
      <div class="acts">
        <button v-if="member.lxmf && !isSelf" class="p" @click="emit('message', member.lxmf)">Message on LXMF</button>
        <button v-if="member.nick" @click="emit('mention', member.nick)">Mention</button>
        <button v-if="!isSelf" @click="emit('ignore', member.id12, !ignored)">{{ ignored ? 'Unignore' : 'Ignore' }}</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import type { RrcMember } from '../../modules/rrc'
import { peerAvatar, nickColour, groupHex } from '../../lib/rrcText'

const props = defineProps<{
  member: RrcMember
  hue: number
  hubName: string
  /* "#room" labels of the rooms on this hub they are in. */
  sharedRooms: string[]
  ignored: boolean
  isSelf: boolean
}>()
const emit = defineEmits<{
  close: []
  message: [lxmf: string]
  mention: [nick: string]
  ignore: [id12: string, on: boolean]
}>()

const glyph = computed(() => peerAvatar(props.member.lxmf || props.member.id12, props.member.nick).glyph)
const avStyle = computed(() => ({
  background: `hsl(${props.hue} 45% 32%)`, color: `hsl(${props.hue} 70% 86%)`,
}))
</script>

<style scoped>
.scrim { position: absolute; inset: 0; z-index: 8; display: flex; align-items: flex-start; justify-content: flex-end;
         padding: 70px 40px 0 0; background: rgba(0, 0, 0, .25); }
.card { width: 300px; max-width: calc(100% - 24px); background: #2f2f2f; border: 1px solid #454545; border-radius: 10px;
        padding: 16px; box-shadow: 0 24px 60px rgba(0, 0, 0, .6); color: #e8e8e8; font-size: calc(13px * var(--rfs, 1)); }
.top { display: flex; gap: 12px; align-items: center; }
.av { width: 44px; height: 44px; border-radius: 50%; flex: none; font-size: 16px; font-weight: 700;
      display: flex; align-items: center; justify-content: center; }
.who { flex: 1; min-width: 0; }
.nm { font-weight: 700; font-size: calc(16px * var(--rfs, 1)); overflow: hidden; text-overflow: ellipsis; }
.hh, .mono { font-family: "DejaVu Sans Mono", Menlo, monospace; color: #9a9a9a; font-size: calc(12px * var(--rfs, 1)); }
.hh { margin-top: 2px; word-break: break-all; }
.x { align-self: flex-start; color: #9a9a9a; cursor: pointer; padding: 0 2px; }
.x:hover { color: #fff; }
.meta { color: #9a9a9a; font-size: calc(12px * var(--rfs, 1)); margin: 12px 0; display: flex; flex-direction: column; gap: 3px; }
.dim { color: #777; }
.acts { display: flex; gap: 8px; flex-wrap: wrap; }
.acts button { background: none; border: 1px solid #555; color: #e8e8e8; border-radius: 6px; padding: 5px 10px;
               font-size: calc(12px * var(--rfs, 1)); cursor: pointer; }
.acts button:hover { border-color: #777; }
.acts button.p { background: #2c6bed; border-color: #2c6bed; color: #fff; font-weight: 700; }
</style>
