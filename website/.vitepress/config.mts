import { defineConfig } from 'vitepress'

const repo = 'https://github.com/kennethassogba/buildray'
const fr = [
  ['Bienvenue', '/fr/'], ['Première mesure', '/fr/start'],
  ['Comprendre le build', '/fr/concepts'], ['Choisir une optimisation', '/fr/optimize'],
  ['Lire les mesures', '/fr/measure'], ['Dans GitHub Actions', '/fr/ci'],
  ['Résultats et limites', '/fr/results'], ['La suite', '/fr/next']
]
const en = [
  ['Welcome', '/'], ['Your first measurement', '/start'],
  ['How a build works', '/concepts'], ['Choose an optimization', '/optimize'],
  ['Read the measurements', '/measure'], ['In GitHub Actions', '/ci'],
  ['Results and limits', '/results'], ['What comes next', '/next']
]

export default defineConfig({
  title: 'Buildray',
  description: 'Measure C++ builds, understand their cost and validate optimizations.',
  base: '/buildray/',
  appearance: 'dark',
  srcExclude: ['README.md'],
  head: [['link', { rel: 'icon', type: 'image/svg+xml', href: '/buildray/mark.svg' }]],
  sitemap: { hostname: 'https://kennethassogba.github.io/buildray/' },
  markdown: { lineNumbers: false },
  locales: {
    fr: {
      label: 'Français', lang: 'fr', link: '/fr/',
      themeConfig: {
        nav: [{ text: 'Guide', link: '/fr/start' }, { text: 'Résultats', link: '/fr/results' }],
        sidebar: [{ text: 'Le guide', items: fr.map(([text, link]) => ({ text, link })) }],
        outline: { label: 'Sur cette page', level: [2, 3] },
        docFooter: { prev: 'Précédent', next: 'Suivant' },
        sidebarMenuLabel: 'Parcourir le guide', returnToTopLabel: 'Retour en haut',
        darkModeSwitchLabel: 'Apparence', darkModeSwitchTitle: 'Passer en mode sombre', lightModeSwitchTitle: 'Passer en mode clair', langMenuLabel: 'Langue',
        skipToContentLabel: 'Aller au contenu',
        editLink: { pattern: `${repo}/edit/main/website/:path`, text: 'Proposer une correction' }
      }
    },
    root: {
      label: 'English', lang: 'en', link: '/',
      themeConfig: {
        nav: [{ text: 'Guide', link: '/start' }, { text: 'Results', link: '/results' }],
        sidebar: [{ text: 'The guide', items: en.map(([text, link]) => ({ text, link })) }],
        editLink: { pattern: `${repo}/edit/main/website/:path`, text: 'Suggest a correction' }
      }
    }
  },
  themeConfig: {
    logo: { light: '/mark.svg', dark: '/mark-dark.svg' },
    socialLinks: [{ icon: 'github', link: repo }],
    search: {
      provider: 'local',
      options: {
        locales: {
          fr: {
            translations: {
              button: { buttonText: 'Rechercher', buttonAriaLabel: 'Rechercher dans le guide' },
              modal: {
                displayDetails: 'Afficher les détails', resetButtonTitle: 'Effacer',
                backButtonTitle: 'Fermer la recherche', noResultsText: 'Aucun résultat pour',
                footer: { selectText: 'ouvrir', selectKeyAriaLabel: 'Entrée', navigateText: 'parcourir', navigateUpKeyAriaLabel: 'Flèche haut', navigateDownKeyAriaLabel: 'Flèche bas', closeText: 'fermer', closeKeyAriaLabel: 'Échap' }
              }
            }
          }
        }
      }
    }
  }
})
