---
title: La suite
---

# Élargir le nettoyage des dépendances {#test-dependency-cleanup-at-scale}

`prune-deps` traite les noms de cibles et les listes de bibliothèques stockés dans des variables CMake, y compris dans des boucles et des fonctions. Sur un projet de test contenant 40 bibliothèques inutiles, un essai et une vérification finale suffisent toujours. Les définitions des variables partagées restent intactes.

Sur XGrammar, retirer une dépendance inutilisée évite une étape de lien d’environ 32 secondes après une modification du code de GoogleMock. Modifier le parseur ne donne aucun gain confirmé. Le gain concerne cette reconstruction précise. Le nettoyage des dépendances n’a pas encore montré de gain sur un build complet de ces projets.

## Les prochaines étapes {#next-steps}

1. Réduire les rejets répétés sur Highway. La recherche valide 35 retraits mais atteint sa limite de 64 essais, surtout sur des copies répétées de `libhwy.a` dans les commandes de lien. Distinguer une copie redondante d’une bibliothèque retirée entièrement, en gardant les mêmes contrôles sur les binaires.
2. Prendre en charge les fichiers `.rsp`, qui contiennent une liste d’arguments du compilateur. Ils bloquent la recherche sur MuJoCo, où 236 liens sont désormais éligibles.
3. Résoudre les alias CMake, qui excluent encore de la recherche les liens des programmes SentencePiece.

Les [résultats du nettoyage](https://github.com/kennethassogba/buildray/blob/main/docs/unused-dependencies-validation.md) distinguent le projet de test des mesures sur les vrais projets. Ne trouver aucun retrait éligible ne signifie pas que toutes les dépendances sont nécessaires.

## Plus tard {#later}

Expliquer quelle dépendance déclenche chaque reconstruction. Puis étudier des cibles plus petites à partir de l’usage mesuré des symboles. Une grosse bibliothèque statique ne justifie pas à elle seule un découpage : le linker choisit déjà les fichiers objets dont il a besoin.
