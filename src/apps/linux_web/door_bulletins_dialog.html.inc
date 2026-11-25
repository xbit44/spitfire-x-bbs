<dialog id="door-bulletins-dialog" class="door-bulletins-dialog" aria-labelledby="door-bulletins-title">
  <div class="form-actions"><h2 id="door-bulletins-title">Doors &gt; Door Bulletins</h2><button id="dbul-close" type="button">Close</button></div>
  <p class="field-help">Enabled bulletins appear in the global caller index. Permanent numbers stay fixed; gaps are allowed. Source files are never changed here.</p>
  <div class="form-actions"><button id="dbul-new" type="button">Add New</button><button id="dbul-reload" type="button">Reload Saved Settings</button></div>
  <div class="dbul-layout">
    <div class="dbul-list-scroll"><table><thead><tr><th>Number</th><th>Title</th><th>Source</th><th>Enabled</th><th>Last check</th></tr></thead><tbody id="dbul-list"></tbody></table></div>
    <form id="dbul-form">
      <div class="dbul-fields">
        <label>Permanent number (1-999)<input id="dbul-number" type="number" min="1" max="999" step="1" required></label>
        <label>Title<input id="dbul-title" maxlength="64" required></label>
        <label class="checkbox-label"><input id="dbul-enabled" type="checkbox" checked>Enabled</label>
        <label>Source<select id="dbul-source"><option value="0">Display file (DBnnn.ANS / .ASC)</option><option value="1">Door-generated file</option></select></label>
        <label>Door that writes the file<select id="dbul-door"></select></label>
        <label>Relative file<input id="dbul-path" maxlength="240" autocomplete="off"></label>
        <label>Content format<select id="dbul-format"><option value="0">ASCII</option><option value="1">ANSI</option></select></label>
      </div>
      <p id="dbul-source-help" class="field-help"></p>
      <div class="form-actions"><button id="dbul-save" class="primary" type="submit">Save Changes</button><button id="dbul-remove" class="danger" type="button">Remove Definition...</button></div>
      <div class="form-actions"><button id="dbul-validate" type="button">Validate / Refresh</button><button id="dbul-preview" type="button">Preview</button><label class="checkbox-label"><input id="dbul-plain" type="checkbox">Plain-text preview</label></div>
    </form>
  </div>
  <section id="dbul-confirm" class="card" hidden aria-labelledby="dbul-confirm-title"><h3 id="dbul-confirm-title"></h3><p id="dbul-confirm-detail"></p><div class="form-actions"><button id="dbul-confirm-yes" type="button"></button><button id="dbul-confirm-no" type="button">Keep Editing</button></div></section>
  <p id="dbul-result" role="status" aria-live="polite"></p>
  <section id="dbul-preview-section" hidden><h3>Source Preview - sample caller, security level 999</h3><p>Preview shows the last screen of long bulletins. Callers can read the whole bulletin.</p><div class="menu-preview-scroll"><pre id="dbul-preview-screen" class="bbs-preview" aria-label="80 column by 25 row Door Bulletin preview"></pre></div></section>
</dialog>
