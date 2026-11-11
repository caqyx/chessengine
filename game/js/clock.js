/**
 * clock.js - a two-sided chess clock.
 *
 * The clock only knows remaining times and which side is running. When to start, stop or add
 * an increment is decided by the controller.
 */

const TICK_MS = 100;

/**
 * @param {Object} options
 * @param {(remaining: {w: number, b: number}, running: 'w'|'b'|null) => void} options.onTick
 * @param {(color: 'w'|'b') => void} options.onFlag  called once when the running side hits zero
 * @param {() => number} [options.now]  millisecond time source
 */
export function createClock({ onTick, onFlag, now = () => performance.now() }) {
  const remaining = { w: 0, b: 0 };
  let running = null;
  let since = 0;
  let timer = null;

  /** Moves the time elapsed since the last settlement off the running side. */
  function settle() {
    const time = now();
    if (running) remaining[running] = Math.max(0, remaining[running] - (time - since));
    since = time;
  }

  function stopTimer() {
    clearInterval(timer);
    timer = null;
  }

  function tick() {
    settle();
    onTick({ ...remaining }, running);
    if (running && remaining[running] <= 0) {
      const flagged = running;
      running = null;
      stopTimer();
      onFlag(flagged);
    }
  }

  return {
    /** @returns {{w: number, b: number}} remaining milliseconds right now */
    read() {
      settle();
      return { ...remaining };
    },

    /** @returns {'w'|'b'|null} */
    running() {
      return running;
    },

    /** Replaces both times (the clock keeps running for the same side, if any). */
    set(values) {
      settle();
      remaining.w = Math.max(0, values.w);
      remaining.b = Math.max(0, values.b);
    },

    add(color, ms) {
      settle();
      remaining[color] += ms;
    },

    /** Starts (or switches to) the given side. */
    run(color) {
      settle();
      running = color;
      if (!timer) timer = setInterval(tick, TICK_MS);
    },

    stop() {
      settle();
      running = null;
      stopTimer();
    },
  };
}

/** "5:00", "0:42", and tenths below ten seconds: "0:07.3". */
export function formatClock(ms) {
  const clamped = Math.max(0, ms);
  if (clamped < 10_000) {
    const tenths = Math.floor(clamped / 100);
    return `0:0${Math.floor(tenths / 10)}.${tenths % 10}`;
  }
  const seconds = Math.ceil(clamped / 1000);
  const minutes = Math.floor(seconds / 60);
  return `${minutes}:${String(seconds % 60).padStart(2, '0')}`;
}
