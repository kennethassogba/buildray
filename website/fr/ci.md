---
title: Dans GitHub Actions
---

# Surveiller le temps de build en CI {#watch-build-time-in-ci}

La GitHub Action compare une pull request à sa révision de base. Elle construit et teste les deux versions, puis mesure leurs builds complets en alternance sur le même runner.

## Ajouter le workflow {#add-the-workflow}

Créez `.github/workflows/build-time.yml` dans votre projet :

```yaml
name: Build time
on: [pull_request]
permissions:
  contents: read
jobs:
  buildray:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v6
        with:
          fetch-depth: 0
      - uses: kennethassogba/buildray@42ad4e2bbf88e162fdfb4604619a48e80267d7cb
        with:
          source: .
          cmake-args: '["-DBUILD_TESTING=ON"]'
          runs: '5'
          jobs: '2'
          max-regression-percent: '5'
```

Cet exemple fixe une révision de Buildray pour rendre les mises à jour explicites. Adaptez l’option de tests CMake à votre projet. L’Action installe ses outils sur Ubuntu 24.04 et compile la CLI ; ce travail de préparation est hors des temps comparés. Elle est utilisable depuis le dépôt, sans publication Marketplace.

## Retrouver les résultats {#find-the-results}

Le résumé du job affiche la comparaison. Ses artefacts conservent les rapports et, pour une expérience d’optimisation, le patch. La rétention est de 30 jours par défaut. Le contrôle échoue si un ralentissement dépasse le budget et le seuil de bruit.

L’Action n’applique pas le patch et n’ouvre pas de PR. Les sources et les tests exécutent le code du projet : utilisez les mêmes règles de confiance que pour votre workflow de build habituel.

## Essayer une optimisation {#try-an-optimization}

Dans l’étape Buildray, ajoutez par exemple ces entrées à `with` :

```yaml
command: optimize
runs: '3'
jobs: '2'
```

Les expériences d’optimisation comparent le projet avec sa propre copie modifiée. Elles ignorent `baseline-ref`, qui sert à la comparaison entre révisions.

Le PCH est désactivé par défaut. Il demande un choix explicite, `command: pch` ; le nettoyage des includes et des dépendances ne l’active pas.

Si le projet ignore volontairement certains tests à l’exécution, `allow-baseline-skips: 'true'` permet de conserver ces mêmes exceptions pendant la validation. Elles sont comptées séparément des tests réussis. Par défaut, tout test ignoré bloque la validation.

La comparaison automatique exporte la base avec `git archive`. Elle ne prépare donc pas les sous-modules Git ni les contenus Git LFS. Si votre projet en dépend, préparez les deux sources et utilisez la CLI `compare` dans votre workflow.

Les [entrées, sorties et limites complètes de l’Action](https://github.com/kennethassogba/buildray/blob/main/action.yml) et le [guide CI technique](https://github.com/kennethassogba/buildray/blob/main/docs/ci.md) couvrent les autres modes.
