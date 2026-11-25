(function () {
 'use strict';
 var input = document.getElementById('wiki-query');
 var status = document.getElementById('search-status');
 var results = document.getElementById('search-results');
 var entries = window.SFX_WIKI_INDEX;
 var query = '';
 var params = window.location.search.substring(1).split('&');
 for (var p = 0; p < params.length; p++) {
  if (params[p].split('=')[0] === 'q') {
   try { query = decodeURIComponent(params[p].substring(2).replace(/\+/g, ' ')); }
   catch (error) { query = ''; }
   break;
  }
 }
 // Direct/bookmarked URLs can exceed the input's maxlength. Trim after limiting
 // so truncation cannot leave an empty search term and stall highlighting.
 query = query.substring(0, 160).replace(/^\s+|\s+$/g, '');
 input.value = query;
 if (!query) {
  status.textContent = 'Enter a word or phrase in the search box, then press Enter or choose Go.';
  return;
 }
 if (!entries) {
  status.textContent = 'Search data could not be loaded. Keep the assets folder with this wiki, or use the topic links.';
  return;
 }
 var terms = query.toLowerCase().split(/\s+/);
 var matches = [];
 for (var i = 0; i < entries.length; i++) {
  var entry = entries[i];
  var haystack = (entry.title + ' ' + entry.text).toLowerCase();
  var found = true;
  for (var t = 0; t < terms.length; t++) {
   if (haystack.indexOf(terms[t]) === -1) { found = false; break; }
  }
  if (found) { matches.push(entry); }
 }
 status.textContent = matches.length ? matches.length + (matches.length === 1 ? ' article' : ' articles') + ' found for "' + query + '".' : 'No articles found for "' + query + '". Try fewer words or choose a topic from the contents.';
 for (var m = 0; m < matches.length; m++) {
  var item = matches[m];
  var row = document.createElement('li');
  var heading = document.createElement('h2');
  var link = document.createElement('a');
  link.href = item.url;
  link.textContent = item.title;
  heading.appendChild(link);
  row.appendChild(heading);
  var excerpt = document.createElement('p');
  var at = item.text.toLowerCase().indexOf(terms[0]);
  var start = Math.max(0, at - 65);
  var text = (start ? '... ' : '') + item.text.substring(start, start + 245) + (start + 245 < item.text.length ? ' ...' : '');
  // Queries are literal text, never HTML or regular expressions.
  var lower = text.toLowerCase();
  var cursor = 0;
  while (cursor < text.length) {
   var next = -1;
   var length = 0;
   for (var n = 0; n < terms.length; n++) {
    var pos = lower.indexOf(terms[n], cursor);
    if (pos !== -1 && (next === -1 || pos < next)) { next = pos; length = terms[n].length; }
   }
   if (next === -1) { excerpt.appendChild(document.createTextNode(text.substring(cursor))); break; }
   excerpt.appendChild(document.createTextNode(text.substring(cursor, next)));
   var mark = document.createElement('mark');
   mark.textContent = text.substring(next, next + length);
   excerpt.appendChild(mark);
   cursor = next + length;
  }
  row.appendChild(excerpt);
  results.appendChild(row);
 }
}());
