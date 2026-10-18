---
title: La suite
---

# Tester le nettoyage des dépendances à grande échelle {#test-dependency-cleanup-at-scale}

`prune-deps` teste déjà les retraits par groupes. Sur un projet de test contenant 40 bibliothèques inutiles, un essai et une vérification finale suffisent. Nous l’avons aussi exécuté sur XGrammar, SentencePiece et MuJoCo, sans démontrer de gain de temps lié à ces retraits.

## Les prochaines étapes {#next-steps}

1. Tester des groupes de 100 et 1 000 dépendances, dont certaines doivent rester. Mesurer la durée de recherche, le nombre d’essais et la mémoire utilisée.
2. Prendre en charge les fichiers `.rsp`, qui contiennent une liste d’arguments du compilateur. Ils bloquent actuellement la recherche sur MuJoCo.
3. Valider plusieurs retraits ensemble sur un vrai projet. Mesurer les builds complets et les reconstructions après modification d’une bibliothèque, notamment les liens évités. Des tests qui passent ne suffisent pas à prouver un gain.

Les [résultats du nettoyage](https://github.com/kennethassogba/buildray/blob/main/docs/unused-dependencies-validation.md) distinguent le projet de test des mesures sur les vrais projets. Ne trouver aucun retrait éligible ne signifie pas que toutes les dépendances sont nécessaires.

## Plus tard {#later}

Expliquer quelle dépendance déclenche chaque reconstruction. Puis étudier des cibles plus petites à partir de l’usage mesuré des symboles. Une grosse bibliothèque statique ne justifie pas à elle seule un découpage : le linker choisit déjà les fichiers objets dont il a besoin.
