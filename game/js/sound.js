/**
 * sound.js - sound effects, synthesised with the Web Audio API (no sound files).
 *
 * Every sound is built from a "wooden knock": a short burst of filtered noise for the click of
 * contact, plus two quickly fading sine tones whose frequencies are in the ratio of a wooden
 * block's lowest modes (1 : 2.76), with a slight pitch drop. Checks, promotions and the start
 * and end of a game add a soft bell-like tone on top so they can be told apart without looking.
 *
 * Browsers only allow audio after the user has interacted with the page, so the audio context
 * is created (or resumed) on the first press or key; sounds requested before that are skipped.
 */

const MASTER_GAIN = 0.55;

/** @returns {{play: (kind: string) => void, playMove: (move: object) => void, setEnabled: (on: boolean) => void}} */
export function createSound({ enabled = true } = {}) {
  let ctx = null;
  let master = null;
  let noise = null;
  let on = enabled;

  function unlock() {
    if (!on) return;
    try {
      if (!ctx) {
        const AudioContextClass = window.AudioContext || window.webkitAudioContext;
        if (!AudioContextClass) return;
        ctx = new AudioContextClass();
        master = ctx.createGain();
        master.gain.value = MASTER_GAIN;
        master.connect(ctx.destination);
        noise = makeNoise(ctx);
      }
      if (ctx.state === 'suspended') ctx.resume();
    } catch {
      ctx = null;   // audio unavailable: the game simply stays silent
    }
  }
  document.addEventListener('pointerdown', unlock, { capture: true });
  document.addEventListener('keydown', unlock, { capture: true });

  function ready() {
    return on && ctx && ctx.state === 'running';
  }

  // -- building blocks -----------------------------------------------------------------

  function envelope(start, peak, decay) {
    const gain = ctx.createGain();
    gain.gain.setValueAtTime(0.0001, start);
    gain.gain.exponentialRampToValueAtTime(peak, start + 0.002);
    gain.gain.exponentialRampToValueAtTime(0.0001, start + decay);
    gain.connect(master);
    return gain;
  }

  function tone(start, freq, peak, decay, { type = 'sine', drop = 1 } = {}) {
    const osc = ctx.createOscillator();
    osc.type = type;
    osc.frequency.setValueAtTime(freq, start);
    if (drop !== 1) osc.frequency.exponentialRampToValueAtTime(freq * drop, start + decay);
    osc.connect(envelope(start, peak, decay));
    osc.start(start);
    osc.stop(start + decay + 0.02);
  }

  function click(start, centre, peak, decay) {
    const source = ctx.createBufferSource();
    source.buffer = noise;
    const filter = ctx.createBiquadFilter();
    filter.type = 'bandpass';
    filter.frequency.value = centre;
    filter.Q.value = 1.4;
    source.connect(filter);
    filter.connect(envelope(start, peak, decay));
    source.start(start);
    source.stop(start + decay + 0.02);
  }

  /** A piece set down on a wooden board. `pitch` scales the size of the "block". */
  function knock(start, { pitch = 1, strength = 1 } = {}) {
    const base = 300 * pitch;
    tone(start, base, 0.55 * strength, 0.09, { drop: 0.86 });
    tone(start, base * 2.76, 0.22 * strength, 0.05, { drop: 0.92 });
    click(start, 2400 * pitch, 0.5 * strength, 0.025);
  }

  function bell(start, freq, peak = 0.12, decay = 0.45) {
    tone(start, freq, peak, decay);
    tone(start, freq * 2.01, peak * 0.3, decay * 0.6);
  }

  // -- the sounds ------------------------------------------------------------------------

  const SOUNDS = {
    move: (t) => knock(t),
    capture: (t) => {
      knock(t, { pitch: 0.82, strength: 1.25 });
      click(t + 0.004, 3600, 0.55, 0.03);   // the sharper clack of piece hitting piece
    },
    castle: (t) => {
      knock(t);
      knock(t + 0.11, { pitch: 0.9, strength: 0.85 });
    },
    check: (t) => {
      knock(t, { strength: 1.1 });
      bell(t + 0.02, 1318, 0.09, 0.35);
    },
    promote: (t) => {
      knock(t);
      bell(t + 0.04, 880, 0.08, 0.3);
      bell(t + 0.12, 1318, 0.08, 0.4);
    },
    start: (t) => {
      bell(t, 659, 0.09, 0.4);
      bell(t + 0.13, 988, 0.09, 0.55);
    },
    end: (t) => {
      bell(t, 784, 0.1, 0.5);
      bell(t + 0.16, 659, 0.1, 0.5);
      bell(t + 0.32, 523, 0.11, 0.9);
    },
    lowtime: (t) => {
      tone(t, 1600, 0.12, 0.05);
      tone(t + 0.14, 1600, 0.12, 0.05);
    },
  };

  function play(kind) {
    if (!ready() || !SOUNDS[kind]) return;
    try {
      SOUNDS[kind](ctx.currentTime + 0.005);
    } catch {
      // a failed sound must never disturb the game
    }
  }

  return {
    play,

    /** The sound for a chess.js move object: check over promotion over castling over capture. */
    playMove(move) {
      if (!move) return;
      const flags = move.flags ?? '';
      if (/[+#]$/.test(move.san ?? '')) play('check');
      else if (flags.includes('p')) play('promote');
      else if (flags.includes('k') || flags.includes('q')) play('castle');
      else if (flags.includes('c')) play('capture');
      else play('move');
    },

    setEnabled(value) {
      on = value;
      if (on) unlock();
    },
  };
}

/** Half a second of white noise, reused by every click. */
function makeNoise(ctx) {
  const length = Math.floor(ctx.sampleRate * 0.5);
  const buffer = ctx.createBuffer(1, length, ctx.sampleRate);
  const data = buffer.getChannelData(0);
  let seed = 12345;
  for (let i = 0; i < length; i++) {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    data[i] = (seed / 0x3fffffff) - 1;
  }
  return buffer;
}
