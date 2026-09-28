// Departure picker for the setup page (served as /dep.js, see wifi_provision.c).
// Search for a stop, tick its lines and directions, and the location's
// "dep<N>" field gets the selection as the text entur_client.h reads,
// e.g. "58366=RUT:Line:31/out,RUT:Line:25;6505".
// Runs in the browser, which calls Entur's open APIs directly, so it needs
// internet access (not there on the device's own setup WiFi).
(function () {
  'use strict';

  var CLIENT = 'trondve-multidisplay';
  var GEOCODER = 'https://api.entur.io/geocoder/v1/autocomplete';
  var JP = 'https://api.entur.io/journey-planner/v3/graphql';
  var MAX_STOPS = 4, MAX_LINES = 8, MAX_LEN = 199; /* entur_client.h, APP_CONFIG_DEPARTURES_MAX */
  var MODES = { bus: 'Buss', tram: 'Trikk', metro: 'T-bane', rail: 'Tog', water: 'Båt',
                air: 'Fly', coach: 'Ekspressbuss', lift: 'Heis', funicular: 'Kabelbane' };
  var CATEGORIES = { onstreetBus: 'buss', busStation: 'buss', onstreetTram: 'trikk', tramStation: 'trikk',
                     metroStation: 't-bane', railStation: 'tog', ferryStop: 'ferje', harbourPort: 'båt',
                     airport: 'fly', coachStation: 'buss', liftStation: 'heis' };

  var css =
    '.dp{margin-top:.5rem}.dp .dp-card{border:1px solid #ccc;border-radius:.5rem;padding:.5rem .7rem;margin:.5rem 0;background:#fff}' +
    '.dp header{display:flex;justify-content:space-between;align-items:center;font-weight:600}' +
    '.dp button{width:auto;margin:0;padding:.2rem .6rem;background:transparent;color:#666;font-size:1rem}' +
    '.dp .dp-result{display:flex;width:100%;justify-content:space-between;gap:.6rem;text-align:left;' +
    'border:1px solid #ccc;border-radius:0;margin-top:-1px;background:#fff;color:#222;padding:.5rem}' +
    '.dp .dp-result:disabled{opacity:.5}.dp .dp-kind{color:#666;font-size:.85em;white-space:nowrap}' +
    '.dp label{display:flex;align-items:center;gap:.5rem;margin:.15rem 0;font-weight:400;cursor:pointer}' +
    '.dp label input{width:auto;margin:0;flex:none}' +
    '.dp .dp-name{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#666;font-size:.9rem}' +
    '.dp .dp-dirs{margin:0 0 .3rem 2rem;font-size:.9rem}' +
    '.dp .dp-dirs span{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}' +
    '.dp .badge{flex:none;min-width:2.6em;padding:2px 6px;border-radius:5px;text-align:center;font-weight:700;' +
    'background:#2d5a86;color:#fff}' +
    '.dp .dp-hint{color:#666;font-size:.85rem;margin:.1rem 0 .3rem}.dp .dp-err{color:#c4161c;font-size:.85rem}';
  document.head.appendChild(el('style', {}, css));

  // el('div', { class: 'x' }, child, 'text', ...)
  function el(tag, attrs) {
    var node = document.createElement(tag);
    Object.keys(attrs || {}).forEach(function (k) {
      var v = attrs[k];
      if (v === false || v == null) return;
      if (k === 'class') node.className = v;
      else if (k === 'style') Object.assign(node.style, v);
      else if (typeof v !== 'string') node[k] = v;
      else node.setAttribute(k, v);
    });
    for (var i = 2; i < arguments.length; i++) {
      var c = arguments[i];
      [].concat(c).forEach(function (x) { if (x != null && x !== false) node.append(x); });
    }
    return node;
  }

  function badge(line) {
    var p = line.presentation || {};
    return el('span', { class: 'badge',
      style: p.colour ? { background: '#' + p.colour, color: p.textColour ? '#' + p.textColour : '#fff' } : null },
      line.publicCode || '?');
  }

  // ---- Entur API ----

  function searchStops(text, near, signal) {
    var q = { text: text, layers: 'venue', size: '10', lang: 'no' };
    if (near) { q['focus.point.lat'] = near.lat; q['focus.point.lon'] = near.lon; }
    return fetch(GEOCODER + '?' + new URLSearchParams(q), { headers: { 'ET-Client-Name': CLIENT }, signal: signal })
      .then(function (r) { if (!r.ok) throw new Error('Geocoder: HTTP ' + r.status); return r.json(); })
      .then(function (j) {
        return j.features
          .filter(function (f) { return (f.properties.id || '').indexOf('NSR:StopPlace:') === 0; })
          .map(function (f) {
            return { id: f.properties.id, name: f.properties.name, label: f.properties.label,
                     categories: f.properties.category || [] };
          });
      });
  }

  var LINES_QUERY = 'query($id:String!){stopPlace(id:$id){id name quays{' +
    'lines{id publicCode name transportMode presentation{colour textColour}}' +
    'id journeyPatterns{directionType line{id} quays{id name stopPlace{id}}}}}}';

  // Trains (Vy at least) come with no inbound/outbound. Their directions are
  // told apart by where they go next instead: patterns whose stops after
  // this one overlap go the same way. Each way is then named by a stop all
  // its patterns call at later on ("v" + its number, e.g. "v502"), which is
  // what the display checks each departure for. Up to 3 stops if no single
  // one is common to all of them.
  function viaDirections(patterns) {
    var groups = patterns.map(function (pt, n) { return n; });
    function root(n) { while (groups[n] !== n) n = groups[n] = groups[groups[n]]; return n; }
    var firstWith = {};
    patterns.forEach(function (pt, n) {
      pt.after.forEach(function (stop) {
        if (stop in firstWith) groups[root(n)] = root(firstWith[stop]);
        else firstWith[stop] = n;
      });
    });
    var byRoot = {};
    patterns.forEach(function (pt, n) { (byRoot[root(n)] = byRoot[root(n)] || []).push(pt); });
    return Object.keys(byRoot).map(function (r) {
      var group = byRoot[r], left = group.slice(), refs = [];
      while (left.length && refs.length < 3) {
        var score = {};
        left.forEach(function (pt) {
          pt.after.forEach(function (stop, k) {
            var sc = score[stop] = score[stop] || { n: 0, pos: 0 };
            sc.n++; sc.pos += k;
          });
        });
        var best = Object.keys(score).sort(function (a, b) {
          return score[b].n - score[a].n || score[a].pos / score[a].n - score[b].pos / score[b].n;
        })[0];
        refs.push(best);
        left = left.filter(function (pt) { return pt.after.indexOf(best) < 0; });
      }
      var dests = {};
      group.forEach(function (pt) { if (pt.dest) dests[pt.dest] = true; });
      return { type: 'v' + refs.map(function (id) { return id.replace(/^NSR:StopPlace:/, ''); }).join('+'),
               destinations: Object.keys(dests).sort(function (a, b) { return a.localeCompare(b, 'no'); }) };
    });
  }

  function shortName(name) { return name.replace(/ (stasjon|holdeplass)$/i, ''); }

  function compareLines(a, b) {
    return (a.publicCode || '').localeCompare(b.publicCode || '', 'no', { numeric: true });
  }

  // { name, lines: [{ ...line, directions: [{ type, destinations }] }] }
  function getLines(stopId) {
    return fetch(JP, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'ET-Client-Name': CLIENT },
      body: JSON.stringify({ query: LINES_QUERY, variables: { id: stopId } }),
    }).then(function (r) { if (!r.ok) throw new Error('Journey Planner: HTTP ' + r.status); return r.json(); })
      .then(function (j) {
        if (j.errors && j.errors.length) throw new Error(j.errors[0].message);
        var sp = j.data.stopPlace;
        if (!sp) throw new Error('Fant ikke holdeplass ' + stopId);
        var byId = {}, dirs = {}, unknown = {};
        sp.quays.forEach(function (quay) {
          quay.lines.forEach(function (l) { byId[l.id] = l; });
          quay.journeyPatterns.forEach(function (jp) {
            var dest = jp.quays.length ? shortName(jp.quays[jp.quays.length - 1].name) : null;
            if (jp.directionType !== 'inbound' && jp.directionType !== 'outbound') {
              var at = jp.quays.map(function (q) { return q.id; }).indexOf(quay.id);
              var after = jp.quays.slice(at + 1).map(function (q) { return q.stopPlace.id; });
              if (at >= 0 && after.length) (unknown[jp.line.id] = unknown[jp.line.id] || []).push({ after: after, dest: dest });
              return;
            }
            var d = dirs[jp.line.id] = dirs[jp.line.id] || {};
            d[jp.directionType] = d[jp.directionType] || {};
            if (dest) d[jp.directionType][dest] = true;
          });
        });
        var lines = Object.keys(byId).map(function (id) {
          var d = dirs[id] || {};
          var directions = Object.keys(d).sort().map(function (t) {
            return { type: t, destinations: Object.keys(d[t]).sort(function (a, b) { return a.localeCompare(b, 'no'); }) };
          });
          if (directions.length < 2 && unknown[id]) directions = viaDirections(unknown[id]);
          return Object.assign({}, byId[id], { directions: directions });
        }).sort(compareLines);
        return { name: sp.name, lines: lines };
      });
  }

  var linesCache = {};
  function loadLines(stopId) {
    if (!linesCache[stopId]) {
      linesCache[stopId] = getLines(stopId);
      linesCache[stopId].catch(function () { delete linesCache[stopId]; });
    }
    return linesCache[stopId];
  }

  // ---- The selection text <-> [{ stopId, name, lines: [{ id, directions: [] }] }] ----

  function parse(text) {
    return text.split(';').map(function (s) { return s.trim(); }).filter(Boolean).map(function (s) {
      var eq = s.indexOf('=');
      var id = (eq < 0 ? s : s.slice(0, eq)).trim();
      if (/^\d+$/.test(id)) id = 'NSR:StopPlace:' + id;
      var lines = eq < 0 ? [] : s.slice(eq + 1).split(',').map(function (l) { return l.trim(); }).filter(Boolean)
        .map(function (l) {
          var parts = l.split('/');
          var dir = (parts[1] || '').trim().toLowerCase();
          return { id: parts[0].trim(),
                   directions: dir === 'in' || dir === 'inbound' ? ['inbound']
                             : dir === 'out' || dir === 'outbound' ? ['outbound']
                             : /^v\d+(\+\d+)*$/.test(dir) ? [dir] : [] };
        });
      return { stopId: id, name: null, lines: lines };
    });
  }

  function serialize(sel) {
    return sel.map(function (stop) {
      var id = stop.stopId.replace(/^NSR:StopPlace:/, '');
      var lines = stop.lines.map(function (l) {
        var d = l.directions[0];
        return l.directions.length !== 1 ? l.id
             : l.id + '/' + (d === 'inbound' ? 'in' : d === 'outbound' ? 'out' : d);
      });
      return lines.length ? id + '=' + lines.join(',') : id;
    }).join(';');
  }

  // ---- One picker per location ----

  function picker(field) {
    var i = field.name.slice(3);
    var sel = parse(field.value);
    var search = el('input', { type: 'search', placeholder: 'Søk etter holdeplass, f.eks. Jernbanetorget',
                               autocomplete: 'off', enterkeyhint: 'search' });
    var results = el('div', {});
    var list = el('div', {});
    var warn = el('p', { class: 'dp-err' });
    var root = el('div', { class: 'dp' }, list, search, results, warn);
    field.insertAdjacentElement('afterend', root);

    function commit() {
      field.value = serialize(sel);
      warn.textContent = field.value.length > MAX_LEN
        ? 'For lang (' + field.value.length + ' tegn, maks ' + MAX_LEN + '): velg færre holdeplasser eller linjer.'
        : '';
      search.disabled = sel.length >= MAX_STOPS;
      search.placeholder = search.disabled ? 'Maks ' + MAX_STOPS + ' holdeplasser'
                                           : 'Søk etter holdeplass, f.eks. Jernbanetorget';
    }

    // Stops near this location's coordinates rank first.
    function near() {
      var lat = parseFloat((document.querySelector('[name=lat' + i + ']') || {}).value);
      var lon = parseFloat((document.querySelector('[name=lon' + i + ']') || {}).value);
      return isFinite(lat) && isFinite(lon) ? { lat: lat, lon: lon } : null;
    }

    var timer, controller;
    search.addEventListener('keydown', function (e) { if (e.key === 'Enter') e.preventDefault(); });
    search.addEventListener('input', function () {
      clearTimeout(timer);
      var q = search.value.trim();
      if (q.length < 2) { results.replaceChildren(); return; }
      timer = setTimeout(function () { runSearch(q); }, 300);
    });

    function runSearch(q) {
      if (controller) controller.abort();
      controller = new AbortController();
      searchStops(q, near(), controller.signal).then(function (stops) {
        var chosen = sel.map(function (s) { return s.stopId; });
        results.replaceChildren.apply(results, stops.length ? stops.map(function (s) {
          var kinds = s.categories.map(function (c) { return CATEGORIES[c]; })
            .filter(function (k, n, a) { return k && a.indexOf(k) === n; });
          var added = chosen.indexOf(s.id) >= 0;
          var btn = el('button', { type: 'button', class: 'dp-result', disabled: added },
            el('span', {}, s.label), el('span', { class: 'dp-kind' }, added ? 'lagt til' : kinds.join(', ')));
          btn.addEventListener('click', function () { addStop(s); });
          return btn;
        }) : [el('p', { class: 'dp-hint' }, 'Ingen treff')]);
      }).catch(function (err) {
        if (err.name !== 'AbortError') {
          results.replaceChildren(el('p', { class: 'dp-err' }, err.message +
            ' – søket trenger internett (ikke tilgjengelig på oppsett-WiFi-en).'));
        }
      });
    }

    function addStop(s) {
      if (sel.length >= MAX_STOPS) return;
      sel.push({ stopId: s.id, name: s.name, lines: [] });
      var show = document.querySelector('[name=dp' + i + ']');
      if (show) show.checked = true; /* picking stops means showing the board */
      search.value = '';
      results.replaceChildren();
      commit();
      render();
      search.focus();
    }

    // Direction toggles for a ticked line. None ticked = both directions.
    function renderDirections(chosen, line) {
      var box = el('div', { class: 'dp-dirs' });
      if (line.directions.length < 2) return box;
      line.directions.forEach(function (dir) {
        var d = dir.destinations;
        var label = d.length ? 'mot ' + d.slice(0, 3).join(', ') + (d.length > 3 ? ' …' : '')
                             : (dir.type === 'inbound' ? 'Retning inn' : 'Retning ut');
        var cb = el('input', { type: 'checkbox', checked: chosen.directions.indexOf(dir.type) >= 0 });
        cb.addEventListener('change', function () {
          chosen.directions = chosen.directions.filter(function (t) { return t !== dir.type; });
          if (cb.checked) chosen.directions.push(dir.type);
          commit();
        });
        box.append(el('label', { title: d.join(', ') }, cb, el('span', {}, label)));
      });
      return box;
    }

    function renderStop(stop) {
      var title = el('span', {}, stop.name || stop.stopId);
      var remove = el('button', { type: 'button', 'aria-label': 'Fjern' }, '✕');
      remove.addEventListener('click', function () {
        sel.splice(sel.indexOf(stop), 1);
        commit();
        render();
      });
      var hint = el('p', { class: 'dp-hint' });
      var lineBox = el('div', {}, el('p', { class: 'dp-hint' }, 'Henter linjer …'));
      function updateHint() {
        var n = stop.lines.length;
        hint.textContent = n ? n + ' linje' + (n > 1 ? 'r' : '') + ' valgt' + (n > MAX_LINES ? ' – maks ' + MAX_LINES + ' vises' : '')
                             : 'Ingen valgt – viser alle linjer';
      }
      updateHint();

      loadLines(stop.stopId).then(function (info) {
        stop.name = info.name;
        title.textContent = info.name;
        if (!info.lines.length) {
          lineBox.replaceChildren(el('p', { class: 'dp-hint' }, 'Ingen linjer funnet for denne holdeplassen.'));
          return;
        }
        lineBox.replaceChildren.apply(lineBox, info.lines.map(function (line) {
          var chosen = stop.lines.filter(function (l) { return l.id === line.id; })[0];
          var cb = el('input', { type: 'checkbox', checked: !!chosen });
          var dirSlot = el('div', {}, chosen ? renderDirections(chosen, line) : null);
          cb.addEventListener('change', function () {
            stop.lines = stop.lines.filter(function (l) { return l.id !== line.id; });
            dirSlot.replaceChildren();
            if (cb.checked) {
              var picked = { id: line.id, directions: [] };
              stop.lines.push(picked);
              dirSlot.append(renderDirections(picked, line));
            }
            updateHint();
            commit();
          });
          return el('div', {},
            el('label', { title: (MODES[line.transportMode] || line.transportMode) + ': ' + line.name },
              cb, badge(line), el('span', { class: 'dp-name' }, line.name)),
            dirSlot);
        }));
      }, function (err) {
        lineBox.replaceChildren(el('p', { class: 'dp-err' }, 'Kunne ikke hente linjer: ' + err.message));
      });

      return el('div', { class: 'dp-card' }, el('header', {}, title, remove), hint, lineBox);
    }

    function render() {
      list.replaceChildren.apply(list, sel.map(renderStop));
    }

    // Typing in the text field itself still works: re-read it when it changes.
    field.addEventListener('change', function () {
      sel = parse(field.value);
      commit();
      render();
    });

    commit();
    render();
  }

  document.querySelectorAll('input[name^=dep]').forEach(picker);
})();
