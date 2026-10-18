---
title: La suite
---

# Réduire le travail déclenché par un changement

La prochaine priorité proposée est de comprendre et réduire les rebuilds inutiles, avec une attention particulière au lien. Les mesures MuJoCo donnent un cas concret à examiner.

## Les dépendances : ce qui existe déjà

`deps` décrit les relations entre fichiers et cibles, et signale les cycles. `prune-links` peut supprimer une déclaration CMake redondante dans un cas conservateur, puis valider et mesurer le patch.

Ce premier nettoyage ne cherche pas encore à déterminer toutes les bibliothèques dont un programme peut se passer. Une déclaration peut être redondante tout en laissant exactement les mêmes entrées au linker. Sa suppression simplifie alors le CMake sans accélérer le lien.

## Ce qu’il faut mesurer ensuite

Il faut enregistrer séparément les commandes et durées de lien, puis montrer ce qu’un fichier modifié oblige à recompiler et à relier. Le rapport pourra dire quelles cibles prennent du temps et pourquoi elles sont reconstruites. Cette mesure détaillée du linker est à développer.

À partir de là, nous pourrons tester des changements précis : réduire la portée d’une dépendance `PUBLIC`, retirer une bibliothèque effectivement inutile, ou séparer un header très partagé d’une implémentation qui change souvent. Chacun demande de vérifier les exigences transmises aux autres cibles, puis les builds et tests des consommateurs concernés.

## Cela peut-il accélérer le lien ?

Oui, si le changement retire du travail au linker ou évite de le relancer. Une bibliothèque statique modifiée peut obliger à relier les exécutables qui la consomment. Réduire ces liens peut avoir plus d’effet qu’économiser une seconde de compilation.

Mais moins de lignes dans `target_link_libraries` ne signifie pas automatiquement moins de travail. Les bibliothèques peuvent rester présentes par un autre chemin, et une dépendance apparemment inutilisée peut fournir des options, des macros ou un comportement au chargement.

La bonne preuve restera une commande de lien réellement réduite, ou moins de liens déclenchés, avec des tests qui passent et un gain avant/après. La rupture automatique des cycles et l’audit complet des dépendances restent aussi à faire.
