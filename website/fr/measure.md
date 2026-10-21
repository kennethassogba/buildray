---
title: Lire les mesures
---

# Savoir si le gain est mesuré {#tell-whether-a-gain-is-measured}

Une compilation peut varier d’un passage à l’autre. Buildray répète les builds et alterne l’ordre entre la version de référence et la version modifiée, sur la même machine.

## Ce que le temps contient {#what-the-timer-includes}

Le temps principal va du lancement du build à sa fin, lien compris. La configuration CMake, l’analyse du code, le nettoyage et les tests sont hors du chronomètre. Les caches de compilation sont désactivés ; le cache de fichiers du système reste chaud.

La **médiane** est la durée au milieu des durées triées. Elle représente mieux un passage habituel qu’une moyenne déformée par un passage exceptionnellement lent. La **MAD** décrit la dispersion autour de cette médiane. Plus elle est élevée, plus la mesure varie.

Pour recommander une optimisation, Buildray demande au moins trois paires de builds et un gain supérieur au plus grand de ces deux seuils : 2 % de la médiane de référence, ou trois fois la plus grande MAD. C’est une règle pratique, pas une preuve de significativité statistique.

## Comprendre le résultat {#read-the-verdict}

| Résultat | Ce que vous pouvez en déduire |
| --- | --- |
| `improved` | Le gain dépasse le seuil de variation de ce run. |
| `regressed` | Le ralentissement dépasse le seuil de ce run. |
| `inconclusive` | Les mesures ne permettent pas de conclure. |
| `insufficient_samples` | Il manque des passages pour recommander le patch. |
| `no_changes` | Aucune modification éligible n’a été produite. |
| `not_measured` | Le temps concerné n’a pas été mesuré. |

`status: passed` signifie que l’exécution s’est terminée correctement. `patch_validated` indique que le candidat a passé les validations prévues. `patch_recommended` ajoute l’exigence d’un gain mesuré. Ces informations répondent à des questions différentes.

## Mesurer le travail quotidien {#measure-everyday-edits}

```bash
buildray unity --source /chemin/du/projet \
  --target ma_bibliotheque --batch-size 4 \
  --rebuild-file src/model.cc --rebuild-file include/model.h \
  --incremental-only --runs 3 --jobs 2
```

Buildray prépare les deux builds, vérifie qu’ils n’ont plus rien à faire, puis ajoute un commentaire dans un fichier et mesure la reconstruction. Il répète ce travail pour chaque fichier demandé, exécute CTest après chaque build mesuré et restaure le contenu original des copies.

Le commentaire révèle quels fichiers une modification oblige à reconstruire. Il ne simule pas tous les changements de code possibles. Le rapport conserve les compilations déclenchées et la création éventuelle du PCH. La mémoire indiquée est le pic d’un compilateur individuel, pas la mémoire totale du build ou du linker.

Le nettoyage des dépendances mesure aussi le nombre de liens et la durée des commandes qui lancent le linker, démarrage du pilote compris. Des liens parallèles peuvent se recouvrir : leur durée cumulée n’est pas le temps total du build. Une dépendance inutile peut coûter peu dans un build complet mais provoquer des liens évitables après une modification.

`--incremental-only` évite de répéter les builds complets pour les chronométrer ; les builds et tests de validation restent nécessaires. Il ne produit pas de recommandation automatique sur le gain d’un build complet. Un ralentissement incrémental mesuré bloque cette recommandation lorsque les deux types de mesures sont activés.

## Comparer deux révisions {#compare-revisions}

```bash
buildray compare --baseline-source /chemin/version-de-reference \
  --source /chemin/version-actuelle --runs 5 --jobs 2 \
  --max-regression-percent 5
```

Le code de sortie `3` indique un ralentissement supérieur au budget et au seuil de bruit. Un résultat incertain peut passer ce contrôle : relancez avec davantage de passages sur une machine plus calme avant de conclure.

Pour un protocole détaillé, consultez les [mesures incrémentales](https://github.com/kennethassogba/buildray/blob/main/docs/incremental.md) et les [comparaisons CI](https://github.com/kennethassogba/buildray/blob/main/docs/ci.md).
