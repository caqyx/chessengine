#include "bitboard.h"

Bitboard PawnAttacks[2][64], KnightAttacks[64], KingAttacks[64];
Bitboard RookPseudo[64], BishopPseudo[64];
Bitboard Between[64][64], Line[64][64];
Bitboard FileBB[8], RankBB[8], AdjacentFiles[8];
Bitboard ForwardFile[2][64], PassedMask[2][64];
u8 SquareDistance[64][64];
Magic RookMagics[64], BishopMagics[64];

static Bitboard RookTable[102400], BishopTable[5248];

static const int RookDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
static const int BishopDirs[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

static Bitboard slow_attacks(int sq, Bitboard occ, const int dirs[4][2]) {
    Bitboard a = 0;
    for (int d = 0; d < 4; d++) {
        int f = file_of(sq) + dirs[d][0], r = rank_of(sq) + dirs[d][1];
        while (f >= 0 && f < 8 && r >= 0 && r < 8) {
            a |= bit(make_sq(f, r));
            if (occ & bit(make_sq(f, r))) break;
            f += dirs[d][0]; r += dirs[d][1];
        }
    }
    return a;
}

// Magic multipliers found once by random search (tools/ not needed at runtime).
// Each one maps the relevant blocker set of a square to a unique table index.
static const u64 RookMagicNumbers[64] = {
    0x1080004008801020ULL, 0x0840092002C03000ULL, 0x1900200010400900ULL, 0x0880100008000480ULL,
    0x4200100420080200ULL, 0x8100020100080400ULL, 0x0200040110886200ULL, 0x0200008040220411ULL,
    0x0404800084400220ULL, 0x0000401000402000ULL, 0x0086001081220440ULL, 0x0408800800100280ULL,
    0x000A001201040820ULL, 0x8848800200840080ULL, 0x4001000100040200ULL, 0x0442000102105084ULL,
    0x9080010020804100ULL, 0x0040404000201009ULL, 0x0000808010002009ULL, 0x2200090021D00100ULL,
    0x0008008008040080ULL, 0x0004004002010040ULL, 0x0011040008015042ULL, 0x00000A0001768104ULL,
    0x0000800080204009ULL, 0x2010004140002001ULL, 0x9800200280100080ULL, 0x1000100080080080ULL,
    0x0442000A00049020ULL, 0x2100040080020080ULL, 0x0800120400900148ULL, 0x0010040A00128541ULL,
    0x2800804000800030ULL, 0x1010002000400041ULL, 0x4000200011004100ULL, 0x0610008410800800ULL,
    0x0400802402800800ULL, 0xC100020080800400ULL, 0x0002000802000401ULL, 0x0182085882000401ULL,
    0x0220204000808000ULL, 0x2860100040024022ULL, 0x0001002004110040ULL, 0x99101042000A0020ULL,
    0x0004080004008080ULL, 0x0010040002008080ULL, 0x2012004881020004ULL, 0x8300842444820011ULL,
    0x0088403882010200ULL, 0x0820400080210100ULL, 0x0110910040A00300ULL, 0x0801100280080480ULL,
    0x0242009008200600ULL, 0x1002000489500200ULL, 0x0040800200010080ULL, 0x0091800041000080ULL,
    0x0000209300488001ULL, 0x04C1002414824001ULL, 0x020020000B001041ULL, 0x7000100004200901ULL,
    0x8002002004100802ULL, 0x30010002084C0007ULL, 0x0888221800813004ULL, 0x4000002840840112ULL,
};
static const u64 BishopMagicNumbers[64] = {
    0xA010041108003100ULL, 0x006082020A002900ULL, 0x6810010619200000ULL, 0x08281A0520000408ULL,
    0x0001104001000400ULL, 0x0018901008048400ULL, 0x00040A0210245280ULL, 0x000200210808A402ULL,
    0x9140048410821200ULL, 0x0800091010820041ULL, 0x20504804832202C0ULL, 0x0100091401081000ULL,
    0x8021011140000012ULL, 0x0810020804450400ULL, 0x208B0542109008A2ULL, 0x0080084A08040204ULL,
    0x0040E2A80811244CULL, 0x2505022008008108ULL, 0x0430220100420040ULL, 0x010A040420220040ULL,
    0x1105000290400000ULL, 0x0093001200822120ULL, 0x4000A62048043004ULL, 0x280120048A015004ULL,
    0x006090002A020814ULL, 0x44042000240800D0ULL, 0x01102800040A4400ULL, 0x1004080080220040ULL,
    0x0001001011004024ULL, 0x0010044000805040ULL, 0x0914041200820100ULL, 0x0004821012821480ULL,
    0x0024040500C05021ULL, 0x0088611002080200ULL, 0x0116080A00040020ULL, 0x4000020080080080ULL,
    0x2450450140840040ULL, 0x0000880201484100ULL, 0x0222020404020092ULL, 0x8081110600002E00ULL,
    0x2842101105000801ULL, 0x1100809008001025ULL, 0x00020202221C0400ULL, 0x0422014022009020ULL,
    0x0210046102100C00ULL, 0xC004008082029102ULL, 0x00AA461801101200ULL, 0x0404080080201108ULL,
    0x020542108C205002ULL, 0x0410544804100100ULL, 0x0040910841100000ULL, 0x0400200042021100ULL,
    0x00004204850400C0ULL, 0x0200100410A42102ULL, 0x1040020801210102ULL, 0x0805040410420000ULL,
    0x2884804130100200ULL, 0x800C262201242000ULL, 0x1058000194108800ULL, 0x0014221054420204ULL,
    0x0104000012A02200ULL, 0x0200881003300100ULL, 0x0140400202840100ULL, 0x0402020801010201ULL,
};

static void init_magics(Magic* magics, Bitboard* table, const int dirs[4][2], const u64* numbers) {
    for (int sq = 0; sq < 64; sq++) {
        Bitboard edges = ((RANK_1 | RANK_8) & ~RankBB[rank_of(sq)]) | ((FILE_A | FILE_H) & ~FileBB[file_of(sq)]);
        Magic& m = magics[sq];
        m.mask = slow_attacks(sq, 0, dirs) & ~edges;
        int bits = popcount(m.mask);
        m.shift = 64 - bits;
        m.magic = numbers[sq];
        m.attacks = table;
        table += 1ULL << bits;
        Bitboard b = 0;
        do {  // enumerate all subsets of the mask (carry-rippler)
            m.attacks[(b * m.magic) >> m.shift] = slow_attacks(sq, b, dirs);
            b = (b - m.mask) & m.mask;
        } while (b);
    }
}

void bitboards_init() {
    for (int i = 0; i < 8; i++) { FileBB[i] = FILE_A << i; RankBB[i] = RANK_1 << (8 * i); }
    for (int i = 0; i < 8; i++)
        AdjacentFiles[i] = (i > 0 ? FileBB[i - 1] : 0) | (i < 7 ? FileBB[i + 1] : 0);

    static const int KnightD[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
    for (int s = 0; s < 64; s++) {
        int f = file_of(s), r = rank_of(s);
        Bitboard n = 0, k = 0;
        for (int i = 0; i < 8; i++) {
            int nf = f + KnightD[i][0], nr = r + KnightD[i][1];
            if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8) n |= bit(make_sq(nf, nr));
        }
        for (int df = -1; df <= 1; df++)
            for (int dr = -1; dr <= 1; dr++) {
                if (!df && !dr) continue;
                int nf = f + df, nr = r + dr;
                if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8) k |= bit(make_sq(nf, nr));
            }
        KnightAttacks[s] = n;
        KingAttacks[s] = k;
        PawnAttacks[WHITE][s] = pawn_attacks_bb(bit(s), WHITE);
        PawnAttacks[BLACK][s] = pawn_attacks_bb(bit(s), BLACK);
        for (int t = 0; t < 64; t++) {
            int df = iabs(file_of(t) - f), dr = iabs(rank_of(t) - r);
            SquareDistance[s][t] = (u8)imax(df, dr);
        }
        Bitboard fw = 0, fb = 0;
        for (int rr = r + 1; rr < 8; rr++) fw |= bit(make_sq(f, rr));
        for (int rr = r - 1; rr >= 0; rr--) fb |= bit(make_sq(f, rr));
        ForwardFile[WHITE][s] = fw;
        ForwardFile[BLACK][s] = fb;
    }
    for (int s = 0; s < 64; s++) {
        int f = file_of(s);
        for (int c = 0; c < 2; c++) {
            Bitboard m = ForwardFile[c][s];
            if (f > 0) m |= ForwardFile[c][s - 1];
            if (f < 7) m |= ForwardFile[c][s + 1];
            PassedMask[c][s] = m;
        }
    }

    init_magics(RookMagics, RookTable, RookDirs, RookMagicNumbers);
    init_magics(BishopMagics, BishopTable, BishopDirs, BishopMagicNumbers);

    for (int s = 0; s < 64; s++) {
        RookPseudo[s] = rook_attacks(s, 0);
        BishopPseudo[s] = bishop_attacks(s, 0);
    }
    for (int a = 0; a < 64; a++)
        for (int b = 0; b < 64; b++) {
            Between[a][b] = 0; Line[a][b] = 0;
            if (a == b) continue;
            if (RookPseudo[a] & bit(b)) {
                Line[a][b] = (RookPseudo[a] & RookPseudo[b]) | bit(a) | bit(b);
                Between[a][b] = rook_attacks(a, bit(b)) & rook_attacks(b, bit(a));
            } else if (BishopPseudo[a] & bit(b)) {
                Line[a][b] = (BishopPseudo[a] & BishopPseudo[b]) | bit(a) | bit(b);
                Between[a][b] = bishop_attacks(a, bit(b)) & bishop_attacks(b, bit(a));
            }
        }
}
