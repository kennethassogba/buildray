---
title: Comprendre le build
---

# Ce qui se passe pendant un build

Un build transforme vos sources en programme ou en bibliothèque. CMake décrit le travail à faire ; Ninja lance les commandes nécessaires.

<BuildFlow />

## Les headers et les includes

Un fichier source, souvent `.cc` ou `.cpp`, contient le code à compiler. Un header, souvent `.h` ou `.hpp`, décrit des types et des fonctions partagés entre plusieurs fichiers.

Quand un source écrit `#include "model.h"`, le compilateur lit ce header et les headers qu’il inclut à son tour. Si vingt sources utilisent le même gros header, une partie de l’analyse est répétée vingt fois. Supprimer un include inutile peut donc économiser du travail.

Une **déclaration anticipée**, ou *forward declaration*, donne seulement le nom d’un type, par exemple `class Model;`. Cela peut suffire pour déclarer un pointeur vers ce type. Pour connaître sa taille, accéder à ses membres ou détruire certains objets, le compilateur aura besoin de sa définition complète. Buildray vérifie les usages avant de proposer ce remplacement.

## Le compilateur et le lien

Le compilateur produit des fichiers objets, généralement `.o`. Le **linker**, ou éditeur de liens, assemble ces objets et les bibliothèques pour produire le résultat final. Cette dernière étape s’appelle le **lien**.

Un header coûte surtout à la compilation. Une bibliothèque dont dépendent beaucoup d’exécutables peut aussi provoquer beaucoup de liens lorsqu’elle change. C’est pourquoi réduire le temps du compilateur ne suffit pas toujours à accélérer le build complet.

Une **cible CMake** est un élément à construire, par exemple une bibliothèque ou un exécutable de tests. Une dépendance entre cibles peut transmettre des bibliothèques, mais aussi des chemins de headers, des macros et des options de compilation.

## Le PCH : analyser un header une fois

Un *precompiled header* conserve une version déjà analysée de headers. Les sources compatibles peuvent la réutiliser au lieu de recommencer cette analyse.

Le PCH convient surtout aux headers coûteux, partagés par beaucoup de sources et rarement modifiés. Sa création prend du temps et de la mémoire. Si un header qu’il contient change, il faut le reconstruire puis recompiler les sources concernées. Un PCH dépend aussi du compilateur et des options utilisés.

Dans Buildray, l’expérience PCH reste privée à une cible. Elle ne cherche pas à imposer un PCH aux utilisateurs de la bibliothèque. [Fonctionnement dans CMake](https://cmake.org/cmake/help/latest/command/target_precompile_headers.html).

## Unity : compiler plusieurs sources ensemble

Un build Unity regroupe plusieurs fichiers source dans une même compilation. Le compilateur peut ainsi éviter de relire certains headers pour chaque fichier.

En contrepartie, modifier un seul de ces fichiers peut obliger à recompiler tout son groupe. Des noms qui coexistaient dans des fichiers séparés peuvent aussi entrer en conflit. Buildray vérifie les groupes et peut isoler les fichiers incompatibles ; il faut mesurer les rebuilds avant de garder le patch.

## Un rebuild incrémental

Après un premier build, le système reconstruit seulement ce qu’une modification rend nécessaire. C’est un **rebuild incrémental**. Une modification de source concerne souvent peu de fichiers. Modifier un header public peut en affecter des dizaines.

Un bon résultat sur un build complet ne garantit donc pas une bonne expérience quand vous changez un fichier. [Buildray mesure les deux](./measure.md).
