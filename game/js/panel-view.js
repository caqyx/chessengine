/**
 * panel-view.js - everything around the board that is not the move list: the two player bars
 * (name, captured pieces, clock), the status headline, the win-chance indicator, the control
 * buttons, the eval bar and the analysis line. Pure rendering: it shows the model it is given.
 */

import { pieceSvg } from './pieces.js';

function byId(id) {
  return document.getElementById(id);
}

function setText(node, text) {
  if (node.textContent !== text) node.textContent = text;
}

function setHtml(node, html) {
  if (node.dataset.rendered !== html) {
    node.innerHTML = html;
    node.dataset.rendered = html;
  }
}

function playerBar(root) {
  return {
    root,
    side: root.querySelector('.player-side'),
    name: root.querySelector('.player-name'),
    captured: root.querySelector('.player-captured'),
    lead: root.querySelector('.player-lead'),
    clock: root.querySelector('.player-clock'),
  };
}

/** Captured pieces as small icons, one group per piece type. */
function capturedHtml(types, color) {
  let html = '';
  let previous = null;
  for (const type of types) {
    html += `<span class="captured-piece${type === previous ? ' same' : ''}">${pieceSvg(color, type)}</span>`;
    previous = type;
  }
  return html;
}

export function createPanelView() {
  const bars = { top: playerBar(byId('player-top')), bottom: playerBar(byId('player-bottom')) };
  const status = byId('status');
  const statusLine = byId('status-line');
  const statusReview = byId('status-review');
  const statusSub = byId('status-sub');
  const buttons = {
    first: byId('btn-first'),
    prev: byId('btn-prev'),
    next: byId('btn-next'),
    last: byId('btn-last'),
    undo: byId('btn-undo'),
    hint: byId('btn-hint'),
    resign: byId('btn-resign'),
    analysis: byId('btn-analysis'),
  };
  const boardWrap = byId('board-wrap');
  const evalBar = byId('evalbar');
  const evalWhite = byId('evalbar-white');
  const evalLabel = byId('evalbar-label');
  const analysisBox = byId('analysis');
  const analysisScore = byId('analysis-score');
  const analysisDepth = byId('analysis-depth');
  const analysisLine = byId('analysis-line');
  const notice = byId('notice');
  const app = byId('app');
  const winChance = byId('winchance');
  const winChanceBar = byId('winchance-bar');
  const winChanceText = byId('winchance-text');

  function renderClock(bar, clock) {
    bar.clock.hidden = !clock;
    if (!clock) return;
    setText(bar.clock, clock.text);
    bar.clock.classList.toggle('is-running', clock.running);
    bar.clock.classList.toggle('is-low', clock.low);
  }

  function renderPlayer(bar, player) {
    bar.side.dataset.color = player.color;
    setText(bar.name, player.name);
    setHtml(bar.captured, capturedHtml(player.captured, player.color === 'w' ? 'b' : 'w'));
    setText(bar.lead, player.lead > 0 ? `+${player.lead}` : '');
    bar.root.classList.toggle('is-turn', player.toMove);
    renderClock(bar, player.clock);
  }

  function renderStatus(model) {
    setText(statusLine, model.line);
    status.classList.toggle('is-over', model.over);
    status.classList.toggle('is-reviewing', Boolean(model.review));
    statusReview.hidden = !model.review;
    if (model.review) setText(statusSub, model.review);
  }

  function renderControls(model) {
    buttons.first.disabled = !model.canBack;
    buttons.prev.disabled = !model.canBack;
    buttons.next.disabled = !model.canForward;
    buttons.last.disabled = !model.canForward;
    buttons.undo.disabled = !model.canUndo;
    buttons.hint.disabled = !model.canHint;
    buttons.hint.classList.toggle('is-busy', model.hintBusy);
    setText(buttons.hint, model.hintBusy ? 'Thinking' : 'Hint');
    buttons.resign.disabled = !model.canResign;
    buttons.resign.classList.toggle('is-armed', model.resignArmed);
    setText(buttons.resign, model.resignArmed ? 'Confirm' : 'Resign');
    buttons.analysis.setAttribute('aria-pressed', String(model.analysisOn));
    buttons.analysis.setAttribute('aria-disabled', String(model.analysisLocked));
  }

  function renderAnalysis(model) {
    boardWrap.classList.toggle('has-evalbar', model.visible);
    evalBar.hidden = !model.visible;
    analysisBox.hidden = !model.visible;
    if (!model.visible) return;
    evalBar.classList.toggle('is-flipped', model.flipped);
    evalBar.classList.toggle('white-ahead', model.share >= 0.5);
    evalWhite.style.setProperty('--share', `${(model.share * 100).toFixed(1)}%`);
    setText(evalLabel, model.score);
    evalBar.setAttribute('aria-label', `Evaluation ${model.score || 'pending'}`);
    setText(analysisScore, model.score);
    setText(analysisDepth, model.depth ? `Depth ${model.depth}` : '');
    setText(analysisLine, model.line);
  }

  /** Segment widths follow the exact chances; the text (and the label) carry the rounded ones. */
  function renderWinChance(model) {
    app.classList.toggle('has-winchance', model.visible);
    winChance.hidden = !model.visible;
    if (!model.visible) return;
    winChance.classList.toggle('is-empty', !model.chances);
    winChance.classList.toggle('is-stale', model.stale);
    if (model.chances) {
      const { white, draw, black } = model.chances;
      const total = white + draw + black;
      winChanceBar.style.setProperty('--white', `${((white * 100) / total).toFixed(1)}%`);
      winChanceBar.style.setProperty('--draw', `${((draw * 100) / total).toFixed(1)}%`);
    }
    setText(winChanceText, model.text);
    if (winChanceBar.getAttribute('aria-label') !== model.text) winChanceBar.setAttribute('aria-label', model.text);
  }

  function renderNotice(text) {
    notice.hidden = !text;
    setText(notice, text);
  }

  return {
    /**
     * @param {object} model
     *   players  { top, bottom } each { color, name, captured: string[], lead, toMove,
     *              clock: { text, running, low } | null }
     *   status   { line, over, review: string|null }
     *   controls { canBack, canForward, canUndo, canHint, hintBusy, canResign, resignArmed,
     *              analysisOn, analysisLocked }
     *   analysis { visible, share, score, depth, line, flipped }
     *   winChance { visible, chances: { white, draw, black } | null, text, stale }
     *   notice   string
     */
    render(model) {
      renderPlayer(bars.top, model.players.top);
      renderPlayer(bars.bottom, model.players.bottom);
      renderStatus(model.status);
      renderControls(model.controls);
      renderAnalysis(model.analysis);
      renderWinChance(model.winChance);
      renderNotice(model.notice);
    },

    /** Clock-only update, called ten times a second while a clock runs. */
    renderClocks(top, bottom) {
      renderClock(bars.top, top);
      renderClock(bars.bottom, bottom);
    },
  };
}
