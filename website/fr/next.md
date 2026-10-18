---
title: La suite
---

# Tester le nettoyage des dépendances à grande échelle {#test-dependency-cleanup-at-scale}

`prune-deps` teste déjà les retraits par groupes. Sur un projet de test contenant 40 bibliothèques inutiles, un essai et une vérification finale suffisent. Nous l’avons aussi exécuté sur XGrammar, SentencePiece et MuJoCo, sans démontrer de gain de temps lié à ces retraits.

## Les prochaines étapes {#next-steps}

1. Valider les retraits par groupes sur un vrai projet avec de nombreux consommateurs. Mesurer la durée de recherche, le nombre d’essais, la mémoire utilisée et les reconstructions évitées.
2. Prendre en charge les fichiers `.rsp`, qui contiennent une liste d’arguments du compilateur. Ils bloquent actuellement la recherche sur MuJoCo.
3. Prendre en charge les listes de bibliothèques et les noms de cibles exprimés avec des variables CMake. Ils sont courants et limitent actuellement les liens que Buildray peut tester.

Les [résultats du nettoyage](https://github.com/kennethassogba/buildray/blob/main/docs/unused-dependencies-validation.md) distinguent le projet de test des mesures sur les vrais projets. Ne trouver aucun retrait éligible ne signifie pas que toutes les dépendances sont nécessaires.

## Plus tard {#later}

Expliquer quelle dépendance déclenche chaque reconstruction. Puis étudier des cibles plus petites à partir de l’usage mesuré des symboles. Une grosse bibliothèque statique ne justifie pas à elle seule un découpage : le linker choisit déjà les fichiers objets dont il a besoin.
