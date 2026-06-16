(function () {
  var el = document.getElementById('js-output');
  if (!el) return;

  var now = new Date();
  var ts  = now.toISOString().replace('T', ' ').slice(0, 19) + ' UTC';

  el.innerHTML =
    '<span class="tag tag-green">loaded</span> ' +
    'app.js executed at <code>' + ts + '</code> &mdash; ' +
    'this file was served by StaticFileHandler with ETag caching enabled.';
})();
