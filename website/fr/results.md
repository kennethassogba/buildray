---
title: Résultats et limites
---

# Ce que montrent les essais {#what-the-experiments-show}

Nous testons Buildray sur de vrais projets, avec des révisions et des réglages fixes. Ces résultats ne prédisent pas les performances sur votre machine.

## Builds complets : Unity sur XGrammar {#full-builds-unity-on-xgrammar}

Chaque ligne utilise trois paires alternées sur le même runner. Les 83 tests C++ passent avant et après.

| Jobs | Avant → Unity | Gain |
| ---: | ---: | ---: |
| 1 | 172,46 → 136,32 s | 21 % |
| 2 | 103,02 → 85,01 s | 17,5 % |
| 4 | 67,08 → 56,36 s | 16 % |
| 8 | 70,46 → 60,85 s | 13,6 % |

Linux, Clang 21, Release, tests natifs activés, bindings Python désactivés. Les niveaux 1/2 et 4/8 utilisent deux runners différents de quatre CPU ; huit jobs ne signifient pas huit CPU. [Rapports et vérification indépendante](https://github.com/kennethassogba/buildray/actions/runs/39129371937).

## Quelques mesures avant/après {#selected-before-after-measurements}

Chaque ligne contient trois paires alternées, avec build complet et tests de validation. Comparez les deux valeurs d’une même ligne : des lignes différentes peuvent venir de runners différents.

| Projet et expérience | Fichier modifié | Avant → après | Résultat |
| --- | --- | --- | --- |
| XGrammar, Unity | `cpp/earley_parser.cc` | 26,245 → 29,676 s | Ralentissement de 13,07 % |
| XGrammar, Unity | `cpp/grammar_impl.h` | 58,397 → 50,094 s | Gain de 14,22 % |
| SentencePiece, PCH | `src/sentencepiece_processor.cc` | 3,933 → 3,257 s | Gain de 17,19 % |
| SentencePiece, PCH | `src/common.h` | 50,036 → 51,494 s | Incertain |
| MuJoCo, PCH | `src/user/user_model.cc` | 227,502 → 227,547 s | Incertain |

Unity peut accélérer la reconstruction après modification d’un header tout en ralentissant celle d’un source. Sur SentencePiece, le gain du PCH dépend de sa réutilisation : changer `common.h` oblige à le régénérer. Sur MuJoCo, ces essais ne démontrent aucun gain global du PCH.

Les [18 scénarios, configurations et rapports bruts](https://github.com/kennethassogba/buildray/blob/main/docs/incremental-validation.md) expliquent chaque mesure. Les références testées sont XGrammar `7e269d5`, SentencePiece v0.2.2 `e0cce7d` et MuJoCo `651bae1`. SentencePiece utilise un PCH de `src/common.h` sur `sentencepiece-static` ; ce n’est pas l’ancien essai sur `spm_test`.

## Pourquoi le lien devient intéressant {#why-linking-deserves-a-closer-look}

Le log d’un rebuild MuJoCo contient 80 étapes de lien. Dans cette mesure, le fichier source prend environ cinq secondes à compiler, mais le build par défaut dure environ 228 secondes. Cela justifie de chercher ce qui déclenche ces liens et lesquels sont nécessaires. Cela ne prouve pas qu’une dépendance particulière est inutile.

Ces mesures utilisent un réglage commun aux deux versions pour contourner des dépendances temporaires produites par gold avec LTO. Sans lui, le build se relançait même sans modification ; Buildray refusait alors de chronométrer le rebuild. Le réglage et le diagnostic sont conservés dans les preuves.

## Le nettoyage des dépendances aujourd’hui {#dependency-cleanup-so-far}

La recherche par groupes retire 40 liens inutiles en un essai sur un projet de test, puis effectue une vérification finale. Sur les vrais projets, les résultats restent limités : retirer le lien inutilisé vers `gmock` dans XGrammar ne donne aucun gain mesurable, SentencePiece n’a aucun candidat pris en charge et aucun retrait n’est confirmé sur MuJoCo. Cela ne valide pas encore les performances à grande échelle. [Résultats et limites](https://github.com/kennethassogba/buildray/blob/main/docs/unused-dependencies-validation.md).

## Les limites à garder en tête {#limits-of-the-evidence}

Les tests couvrent la configuration fournie. Ils ne remplacent pas la validation des autres plateformes et des consommateurs d’une bibliothèque. Trois paires et un seuil de variation restent une méthode de tri, pas une étude statistique.

Un nettoyage correct peut être utile sans gain mesurable. Nous distinguons donc un patch validé d’une optimisation recommandée. Les [prochaines étapes](./next.md) visent le travail réellement évitable.
