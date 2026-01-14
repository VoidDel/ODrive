import Vue from 'vue'
import App from './App.vue'
import store from './store'
import Tooltip from 'vue-directive-tooltip';
import 'vue-directive-tooltip/dist/vueDirectiveTooltip.css';
import 'typeface-roboto/index.css';
import VueI18n from 'vue-i18n'
import messages from './locales'

Vue.config.productionTip = false
Vue.use(Tooltip);
Vue.use(VueI18n)

const i18n = new VueI18n({
  locale: 'en',
  fallbackLocale: 'en',
  messages
})

new Vue({
  store,
  i18n,
  render: h => h(App)
}).$mount('#app')
