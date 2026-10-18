import DefaultTheme from 'vitepress/theme'
import BuildFlow from './BuildFlow.vue'
import './style.css'

export default {
  extends: DefaultTheme,
  enhanceApp({ app }) { app.component('BuildFlow', BuildFlow) }
}
