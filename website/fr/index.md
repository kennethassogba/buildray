---
title: Le guide
outline: false
---

# Comprendre où part le temps de build.

<p class="lead">Buildray mesure la construction d’un projet C++, essaie une modification dans une copie, puis vérifie si elle fait gagner du temps.</p>

Votre code reste intact. Vous récupérez un patch à relire, les résultats des tests et une comparaison avant/après.

<BuildFlow />

## Commencer par une mesure

Sur un projet CMake avec des tests CTest :

```bash
buildray measure --source /chemin/du/projet --runs 5 --jobs 2
```

Cela mesure plusieurs builds complets, après nettoyage des fichiers compilés. Pour installer Buildray et activer les tests de votre projet, suivez [la première mesure](./start.md).

## Lire le guide à votre rythme

- [Comprendre le build](./concepts.md) explique les headers, le compilateur et le lien, sans supposer que vous les connaissez.
- [Choisir une optimisation](./optimize.md) aide à décider entre nettoyer les includes, précompiler un header ou regrouper des fichiers.
- [Lire les mesures](./measure.md) montre comment distinguer un gain mesuré d’une simple variation.
- [Résultats et limites](./results.md) présente ce qui a marché, ralenti ou peu changé sur nos projets de test.

## Ce que Buildray sait faire

Il mesure des builds complets et, pour les expériences PCH et Unity, des rebuilds après une modification. Il peut proposer des suppressions d’includes, des déclarations anticipées et certains nettoyages CMake. La CLI et la [GitHub Action](./ci.md) conservent les rapports et les patches.

Le support actuel couvre Linux, Clang, CMake et Ninja. Buildray vérifie la configuration et les tests que vous lui donnez. Il ne prouve pas que le code se comporte de façon identique dans toutes les configurations, et il ne garantit pas un gain.

Les [notes techniques et preuves de validation](https://github.com/kennethassogba/buildray/tree/main/docs) restent disponibles pour approfondir.
