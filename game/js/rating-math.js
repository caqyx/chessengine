/**
 * rating-math.js - turns match results into an Elo estimate. Pure functions, no DOM.
 *
 * Model: against an opponent rated `opp`, a player rated R is expected to score
 * 1 / (1 + 10^((opp - R) / 400)) points per game. The estimate is the R whose expected
 * total over all games equals the points actually scored (maximum likelihood).
 */

/** Expected score (0..1) of a player rated `rating` against one rated `opp`. */
export function expectedScore(rating, opp) {
  return 1 / (1 + 10 ** ((opp - rating) / 400));
}

/**
 * @param {{opp: number, score: number}[]} games  score is 1, 0.5 or 0 per game
 * @returns {{rating: number, margin: number, games: number, points: number} | null}
 *   margin is the half-width of a 95% interval.
 */
export function estimateRating(games) {
  if (!games.length) return null;
  const meanOpp = games.reduce((sum, g) => sum + g.opp, 0) / games.length;
  // One imaginary draw against the average opponent keeps the answer finite when every
  // game so far was won (or lost). Its influence fades as real games accumulate.
  const all = [...games, { opp: meanOpp, score: 0.5 }];
  const points = all.reduce((sum, g) => sum + g.score, 0);

  const expectedTotal = (rating) => all.reduce((sum, g) => sum + expectedScore(rating, g.opp), 0);
  let lo = meanOpp - 2000;
  let hi = meanOpp + 2000;
  for (let i = 0; i < 60; i++) {
    const mid = (lo + hi) / 2;
    if (expectedTotal(mid) < points) lo = mid;
    else hi = mid;
  }
  const rating = (lo + hi) / 2;

  // Standard error from the Fisher information of the logistic model.
  const information = all.reduce((sum, g) => {
    const p = expectedScore(rating, g.opp);
    return sum + p * (1 - p);
  }, 0);
  const standardError = 400 / (Math.LN10 * Math.sqrt(Math.max(information, 1e-6)));

  return {
    rating,
    margin: 1.96 * standardError,
    games: games.length,
    points: games.reduce((sum, g) => sum + g.score, 0),
  };
}

/** The Elo cap to give the opponent next: the current estimate, in steps of 50, kept in range. */
export function nextOpponentElo(games, { start, min, max }) {
  const estimate = estimateRating(games);
  const target = estimate ? Math.round(estimate.rating / 50) * 50 : start;
  return Math.min(max, Math.max(min, target));
}
