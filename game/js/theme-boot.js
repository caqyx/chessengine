/*
 * theme-boot.js - applies an explicitly chosen theme before the first paint.
 *
 * A tiny classic script loaded in <head> (modules run too late and would let a dark-theme
 * user see a flash of the light page). Without a stored choice nothing is set and the CSS
 * follows prefers-color-scheme. The storage key is the one used in storage.js.
 */
(function () {
  try {
    var saved = JSON.parse(window.localStorage.getItem('chess.settings.v1') || 'null');
    if (saved && (saved.theme === 'light' || saved.theme === 'dark')) {
      document.documentElement.dataset.theme = saved.theme;
    }
  } catch (error) {
    // storage blocked or corrupt: keep the system theme
  }
})();
