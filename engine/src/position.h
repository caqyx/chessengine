#pragma once
#include "bitboard.h"
#include "nnue.h"

// Everything that changes with a move and cannot be recomputed cheaply.
// states[n] describes the position after n moves from setup, so unmake is
// just "step the pointer back and undo the piece placement".
struct State {
    u64 key, pawnKey;
    Bitboard checkers;        // enemy pieces giving check to the side to move
    Bitboard blockers[2];     // pieces shielding the king of each color from a slider
    int psq;                  // packed (mg, eg) material + piece-square, white minus black
    int npm[2];               // non-pawn material per color
    int phase;                // 24 = all pieces on board, 0 = bare kings/pawns
    int halfmove;             // fifty-move counter
    int pliesFromNull;
    u8 castling, ep, captured;
    Move move;
};

extern int PSQ[12][64];
extern const int SeeValue[13];
extern const int PhaseWeight[6];

struct Position {
    Bitboard byType[6], byColor[2];
    u8 board[64];
    int stm;
    int fullmove;
    State* st;
    State states[MAX_HIST];
    // Network accumulators, one per entry of `states` (only maintained while a network is active).
    Accumulator accStack[MAX_HIST];

    bool set(const char* fen);
    void fen(char* out) const;

    Bitboard all() const { return byColor[0] | byColor[1]; }
    Bitboard pieces(int c, int pt) const { return byColor[c] & byType[pt]; }
    Bitboard pieces(int pt) const { return byType[pt]; }
    int king_sq(int c) const { return lsb(byColor[c] & byType[KING]); }
    bool in_check() const { return st->checkers != 0; }

    Bitboard attackers_to(int sq, Bitboard occ) const;
    Bitboard slider_blockers(int ksq, int kingColor) const;

    void make(Move m);
    void unmake(Move m);
    void make_null();
    void unmake_null();

    bool pseudo_legal(Move m) const;
    bool legal(Move m) const;        // for moves that are pseudo-legal
    bool gives_check(Move m) const;
    bool see_ge(Move m, int threshold) const;
    bool is_draw(int ply) const;     // fifty-move rule or repetition
    bool has_legal_move() const;
    int moved_piece(Move m) const { return board[from_sq(m)]; }
    // Keep the history array from overflowing during very long games.
    void compact_history();
    // Recompute the accumulator of the current position from scratch.
    void refresh_accumulator();

  private:
    void put(int pc, int sq);
    void remove(int sq);
    void move_piece(int from, int to);
    void update_check_info();
};

void position_init();
u64 perft(Position& pos, int depth);
