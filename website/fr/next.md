---
title: La suite
---

# Réduire le travail déclenché par un changement {#reduce-the-work-an-edit-triggers}

La prochaine priorité est d’expliquer pourquoi les cibles sont reconstruites, puis d’étendre le nettoyage à davantage de déclarations CMake.

## Les dépendances : ce qui existe déjà {#dependency-cleanup-today}

`deps` décrit les relations entre fichiers et cibles, et signale les cycles. `prune-links` peut supprimer une déclaration CMake redondante dans un cas conservateur, puis valider et mesurer le patch.

`prune-deps` teste maintenant le retrait de bibliothèques statiques privées avec les statistiques d’extraction des archives, un lien rejoué produisant un binaire identique et la validation complète du build et des tests. Il peut conserver les exigences de compilation quand seul le lien est inutile. Son périmètre reste limité aux déclarations littérales dans une configuration.

## Ce qu’il faut mesurer ensuite {#what-to-measure-next}

Le nettoyage des liens enregistre maintenant les commandes et durées individuelles, ainsi que le nombre de liens incrémentaux. La suite consiste à expliquer quel chemin de dépendances déclenche chaque reconstruction, et à prendre en charge les déclarations CMake utilisant des variables sans en changer le sens.

Les interfaces publiques demandent encore des vérifications entre consommateurs. Réduire la portée d’une dépendance `PUBLIC` ou séparer un header partagé d’une implémentation doit préserver les exigences reçues par les autres cibles.

## Cela peut-il accélérer le lien ? {#can-this-speed-up-linking}

Oui, si le changement retire du travail au linker ou évite de le relancer. Une bibliothèque statique modifiée peut obliger à relier les exécutables qui la consomment. Réduire ces liens peut avoir plus d’effet qu’économiser une seconde de compilation.

Mais moins de lignes dans `target_link_libraries` ne signifie pas automatiquement moins de travail. Les bibliothèques peuvent rester présentes par un autre chemin, et une dépendance apparemment inutilisée peut fournir des options, des macros ou un comportement au chargement.

La bonne preuve restera une commande de lien réellement réduite, ou moins de liens déclenchés, avec des tests qui passent et un gain avant/après. La rupture automatique des cycles et l’audit complet des dépendances restent aussi à faire.
