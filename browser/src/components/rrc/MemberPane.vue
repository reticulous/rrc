<!-- Who is in the room: one row per identity, with its avatar colour, nick and
     first four hex. A row opens the member card; it never acts by itself. -->
<template>
  <div class="members">
    <div class="lbl">
      <span>MEMBERS · {{ members.length }}</span>
      <span class="ref" title="Ask the hub for the member list again" @click="emit('refresh')">⟳</span>
    </div>
    <div v-for="m in members" :key="m.id12" class="m" :class="{ nonick: !m.nick, ign: ignored.has(m.id12) }"
         :title="m.identity || m.id12" @click="emit('open', m)">
      <span class="wav" :style="avStyle(m)">{{ glyph(m) }}</span>
      <span class="mn" :style="m.nick ? { color: nickColour(hueOf(m.id12)) } : undefined">{{ m.nick || groupHex(m.id12.slice(0, 8)) }}</span>
      <small v-if="m.id12 === self">you</small>
      <small v-else-if="ignored.has(m.id12)">ignored</small>
      <small v-else-if="!m.nick">no nick</small>
      <small v-else>{{ m.id12.slice(0, 4) }}</small>
    </div>
  </div>
</template>

<script setup lang="ts">
import type { RrcMember } from '../../modules/rrc'
import { peerAvatar, nickColour, groupHex } from '../../lib/rrcText'

const props = defineProps<{
  members: RrcMember[]
  self: string
  ignored: Set<string>
  hueOf: (id12: string) => number
}>()
const emit = defineEmits<{ open: [m: RrcMember]; refresh: [] }>()

function glyph(m: RrcMember) { return peerAvatar(m.lxmf || m.id12, m.nick).glyph }
function avStyle(m: RrcMember) {
  const h = props.hueOf(m.id12)
  return { background: `hsl(${h} 45% 32%)`, color: `hsl(${h} 70% 86%)` }
}
</script>

<style scoped>
.members { height: 100%; overflow-y: auto; background: #2a2a2a; padding: 10px 0;
           font-size: calc(13px * var(--rfs, 1)); color: #e8e8e8; user-select: none; }
.lbl { display: flex; align-items: center; font-size: calc(11px * var(--rfs, 1)); color: #9a9a9a;
       letter-spacing: .8px; padding: 2px 14px 6px; }
.ref { margin-left: auto; cursor: pointer; font-size: calc(19.5px * var(--rfs, 1)); letter-spacing: 0; line-height: 1; }
.ref:hover { color: #e8e8e8; }
.m { display: flex; align-items: center; gap: 8px; padding: 4px 14px; cursor: pointer; }
.m:hover { background: #323232; }
.m.nonick .mn, .m.ign .mn { color: #777; }
.wav { width: calc(22px * var(--rfs, 1)); height: calc(22px * var(--rfs, 1)); border-radius: 50%; flex: none;
       font-size: calc(9px * var(--rfs, 1)); font-weight: 700; display: flex; align-items: center; justify-content: center; }
.mn { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; min-width: 0; }
small { color: #777; font-size: calc(10px * var(--rfs, 1)); margin-left: auto; white-space: nowrap; }
</style>
