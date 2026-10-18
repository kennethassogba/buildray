<script setup>
import { computed } from 'vue'
import { useData } from 'vitepress'
const { lang } = useData()
const english = computed(() => lang.value === 'en')
const stages = computed(() => english.value
  ? [['01', 'Source + headers', '.cc / .h'], ['02', 'Compile', 'Object files .o'], ['03', 'Link', 'Program or library']]
  : [['01', 'Sources + headers', '.cc / .h'], ['02', 'Compiler', 'Fichiers objets .o'], ['03', 'Lier', 'Programme ou bibliothèque']])
</script>

<template>
  <figure class="build-flow" :aria-label="english ? 'The stages of a C++ build' : 'Les étapes d’un build C++'">
    <ol>
      <li v-for="stage in stages" :key="stage[0]">
        <span class="step-number" aria-hidden="true">{{ stage[0] }}</span>
        <strong>{{ stage[1] }}</strong><span>{{ stage[2] }}</span>
      </li>
    </ol>
    <figcaption>{{ english ? 'Buildray times the build through the link step.' : 'Buildray mesure le build jusqu’à la fin du lien.' }}</figcaption>
  </figure>
</template>
