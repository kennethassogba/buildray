---
title: Première mesure
---

# Faire sa première mesure

Commencez avec un projet qui compile déjà et dont les tests passent. Buildray ne remplace pas la configuration du projet : ses options CMake doivent permettre de construire et de tester ce qui vous intéresse.

## Installer Buildray

Sur Linux, installez Clang/LLVM 21 et leurs bibliothèques de développement, `clang-tidy-21`, `clang-scan-deps`, CMake 3.30 ou plus, Ninja et GNU diff. `libclang-21-dev` est nécessaire en plus des paquets LLVM. Les commandes `deps` et `prune-links` demandent CMake 4.2 ou plus et Ninja 1.13 ou plus.

Les paquets LLVM sont disponibles sur [apt.llvm.org](https://apt.llvm.org/). Les [instructions du dépôt](https://github.com/kennethassogba/buildray#build) précisent les prérequis du compilateur. Python 3.12 ou plus sert aux tests d’intégration et à l’Action.

```bash
git clone https://github.com/kennethassogba/buildray.git
cd buildray
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++-21 \
  -DLLVM_DIR=/usr/lib/llvm-21/lib/cmake/llvm \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$HOME/.local"
```

Ajoutez `$HOME/.local/bin` à votre `PATH` si nécessaire. L’installation place `buildray` et `buildray-header-tool` ensemble ; gardez les deux pour l’analyse des headers. Pour éviter l’installation locale, vous pouvez [utiliser l’Action](./ci.md).

## Mesurer votre projet

```bash
buildray measure --source /chemin/du/projet \
  --compiler clang++-21 --runs 5 --jobs 2 \
  --work-dir /tmp/ma-mesure-buildray \
  -- -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
```

Le répertoire de travail doit être nouveau. Remplacez `BUILD_TESTING` par l’option de tests de votre projet si elle porte un autre nom. Les arguments après `--` sont transmis à CMake.

`--jobs 2` limite à deux le nombre de tâches de build simultanées. Gardez la même valeur pour comparer deux versions. Fermez les autres tâches lourdes pendant la mesure.

## Ouvrir le rapport

Dans le répertoire de travail, `report.json` contient les durées individuelles, la médiane et le contexte de mesure. `logs/` contient les sorties des outils. Les builds sont mesurés séparément des tests et de la configuration.

Un échec de compilation ou de test doit être résolu avant d’interpréter les temps. Si le projet ne déclare aucun test, Buildray refuse normalement la validation. `--allow-no-tests` permet une mesure de compilation seule, avec cette limite indiquée dans le rapport.

Vous pouvez ensuite [essayer une optimisation](./optimize.md) ou [comparer deux versions en CI](./ci.md).
