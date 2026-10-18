---
title: Choisir une optimisation
---

# Choisir quoi essayer {#choose-what-to-try}

Cherchez d’abord où le build passe son temps. Faites une expérience à la fois pour comprendre ce qui a produit le résultat.

## Repérer les headers coûteux {#find-expensive-headers}

```bash
buildray profile --source /chemin/du/projet \
  --compiler clang++-21 --jobs 2
```

`headers.md` classe les headers par temps d’analyse, à partir des traces Clang. Les temps de headers imbriqués se recouvrent : ne les additionnez pas pour prédire un gain. Les traces servent au diagnostic, pas aux mesures avant/après.

## Nettoyer les includes {#clean-up-includes}

```bash
buildray optimize --source /chemin/du/projet --runs 3 --jobs 2
buildray headers --source /chemin/du/projet \
  --forward-declarations --header-path src \
  --exclude third_party --runs 3 --jobs 2
```

`optimize` essaie de supprimer les includes inutiles dans les sources C++. `headers` travaille sur les headers observés ; son option `--forward-declarations` essaie aussi des déclarations anticipées. L’analyse utilise les commandes de compilation réelles. Elle ne constitue pas un audit de toutes les règles de style de clang-tidy.

Les chemins exclus restent compilés et testés, mais leur contenu n’est pas modifié. Les fichiers générés ou téléchargés pendant la configuration sont également exclus des modifications.

## Essayer un PCH ou Unity {#try-pch-or-unity}

```bash
buildray pch --auto --source /chemin/du/projet \
  --compiler clang++-21 --runs 3 --jobs 2

buildray unity --source /chemin/du/projet --target ma_bibliotheque \
  --batch-size 4 --runs 3 --jobs 2 \
  --rebuild-file src/model.cc --rebuild-file include/model.h
```

`pch --auto` profile le projet puis choisit un header sur une cible. Il teste une proposition ; le profil ne prouve pas qu’elle fera gagner du temps. Vous pouvez choisir vous-même avec `--target ma_bibliotheque --header 'include/common.h'` à la place de `--auto`.

Unity produit une option CMake désactivée par défaut. Après application du patch, `-DBUILDRAY_UNITY=ON` active les groupes testés. Ajoutez `--rebuild-file` aux deux modes pour mesurer les fichiers que vous modifiez souvent.

## Examiner les dépendances {#examine-dependencies}

```bash
buildray deps --source /chemin/du/projet
buildray prune-links --source /chemin/du/projet --runs 3 --jobs 2
```

`deps` montre les relations entre headers et cibles, ainsi que les cycles. Il ne casse pas les cycles automatiquement : certains sont valides.

`prune-links` teste la suppression d’une seule déclaration CMake redondante dont les exigences restent accessibles par un autre chemin. Il ne recherche pas encore toutes les bibliothèques réellement inutiles. Si la commande finale de lien reste identique, aucun gain de temps n’est attendu de cette seule suppression.

`prune-deps` essaie de retirer une bibliothèque statique privée de la commande de lien réelle. Il vérifie qu’elle ne fournit aucun objet au binaire, conserve les commandes de compilation et compare les binaires obtenus avec les mêmes objets. Le build complet et les tests doivent aussi passer.

Ce mode demande ELF LLD, choisi explicitement pour les deux versions. Si une bibliothèque fournit seulement des headers ou des paramètres de compilation, le patch peut les conserver tout en retirant le lien. Ajoutez `--rebuild-file` pour vérifier si une modification déclenche moins de liens. La [référence de la commande](https://github.com/kennethassogba/buildray/blob/main/docs/unused-dependencies.md) donne la configuration et les limites.

## Garder uniquement un patch vérifié {#keep-a-validated-patch}

Lisez `patch_validated`, `patch_recommended` et `performance_result` dans `report.json`. Un fichier `.patch` peut exister même après un échec ; sa présence ne vaut pas validation.

Pour un patch d’includes validé, depuis votre projet :

```bash
git apply --check /chemin/du/run/includes.patch
git apply /chemin/du/run/includes.patch
```

Relisez les modifications et testez vos autres configurations avant de les intégrer. Les [références techniques](https://github.com/kennethassogba/buildray/tree/main/docs) détaillent les règles de chaque commande.
