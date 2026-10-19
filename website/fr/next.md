---
title: La suite
---

# Tester le nettoyage des dépendances à grande échelle {#test-dependency-cleanup-at-scale}

`prune-deps` teste déjà les retraits par groupes. Sur un projet de test contenant 40 bibliothèques inutiles, un essai et une vérification finale suffisent. L’essai Highway valide un retrait parmi 15 programmes analysés, avec 2 784 tests réussis et les mêmes 23 tests ignorés dans les deux versions. La plupart des dépendances de ses tests utilisent des variables CMake que la recherche ne traite pas encore. Les essais sur les vrais projets n’ont pas démontré de gain global du temps de build.

## Les prochaines étapes {#next-steps}

1. Prendre en charge les listes de bibliothèques et les noms de cibles exprimés avec des variables CMake, puis relancer Highway sur les programmes de test encore ignorés par la recherche.
2. Prendre en charge les fichiers `.rsp`, qui contiennent une liste d’arguments du compilateur. Ils bloquent actuellement la recherche sur MuJoCo.
3. Valider des groupes de dépendances inutiles confirmées sur ces projets, en mesurant le coût de recherche et les reconstructions évitées. Un seul retrait ne démontre pas les performances à grande échelle.

Les [résultats du nettoyage](https://github.com/kennethassogba/buildray/blob/main/docs/unused-dependencies-validation.md) distinguent le projet de test des mesures sur les vrais projets. Ne trouver aucun retrait éligible ne signifie pas que toutes les dépendances sont nécessaires.

## Plus tard {#later}

Expliquer quelle dépendance déclenche chaque reconstruction. Puis étudier des cibles plus petites à partir de l’usage mesuré des symboles. Une grosse bibliothèque statique ne justifie pas à elle seule un découpage : le linker choisit déjà les fichiers objets dont il a besoin.
