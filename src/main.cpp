#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

// ============================================================
// SIMPLELOGICS PUCT ENGINE
// ============================================================

enum Piece : int {
    EMPTY = 0,
    WP = 1, WN = 2, WB = 3, WR = 4, WQ = 5, WK = 6,
    BP = -1, BN = -2, BB = -3, BR = -4, BQ = -5, BK = -6
};

enum Flag : uint8_t {
    QUIET = 0,
    CAPTURE = 1,
    DOUBLE_PUSH = 2,
    EP_CAPTURE = 4,
    KING_CASTLE = 8,
    QUEEN_CASTLE = 16,
    PROMOTION = 32
};

struct Move {
    uint8_t from = 0;
    uint8_t to = 0;
    int8_t promotion = 0;
    uint8_t flags = QUIET;
};

static bool sameMove(const Move& a, const Move& b) {
    return a.from == b.from &&
           a.to == b.to &&
           a.promotion == b.promotion;
}

// ============================================================
// BOARD
// ============================================================

struct Board {
    array<int, 64> board{};
    bool white = true;
    uint8_t castle = 15;
    int ep = -1;
    int halfmove = 0;
    int fullmove = 1;

    static Board startpos() {
        Board b;
        b.board.fill(EMPTY);

        const int back[8] = {
            WR, WN, WB, WQ, WK, WB, WN, WR
        };

        for (int i = 0; i < 8; ++i) {
            b.board[i] = back[i];
            b.board[8 + i] = WP;
            b.board[48 + i] = BP;
            b.board[56 + i] = -back[i];
        }

        return b;
    }
};

static inline int fileOf(int s) {
    return s & 7;
}

static inline int rankOf(int s) {
    return s >> 3;
}

static inline bool inside(int f, int r) {
    return f >= 0 && f < 8 && r >= 0 && r < 8;
}

static inline bool ownPiece(int p, bool white) {
    return white ? p > 0 : p < 0;
}

static inline int pieceValue(int p) {
    switch (abs(p)) {
        case 1: return 100;
        case 2: return 320;
        case 3: return 335;
        case 4: return 500;
        case 5: return 900;
        case 6: return 20000;
        default: return 0;
    }
}

static string squareName(int s) {
    string r = "a1";
    r[0] = char('a' + fileOf(s));
    r[1] = char('1' + rankOf(s));
    return r;
}

static string uciMove(const Move& m) {
    string r =
        squareName(m.from) +
        squareName(m.to);

    if (m.promotion) {
        switch (abs((int)m.promotion)) {
            case 5: r += 'q'; break;
            case 4: r += 'r'; break;
            case 3: r += 'b'; break;
            case 2: r += 'n'; break;
        }
    }

    return r;
}

// ============================================================
// ATTACK DETECTION
// ============================================================

static int kingSquare(
    const Board& b,
    bool white
) {
    int king = white ? WK : BK;

    for (int s = 0; s < 64; ++s) {
        if (b.board[s] == king)
            return s;
    }

    return -1;
}

static bool attacked(
    const Board& b,
    int square,
    bool byWhite
) {
    int f = fileOf(square);
    int r = rankOf(square);

    // Pawns
    int pawnRank =
        byWhite ? r - 1 : r + 1;

    if (pawnRank >= 0 && pawnRank < 8) {
        for (int df : {-1, 1}) {
            int nf = f + df;

            if (!inside(nf, pawnRank))
                continue;

            int p =
                b.board[pawnRank * 8 + nf];

            if (p == (byWhite ? WP : BP))
                return true;
        }
    }

    // Knights
    static const int knight[8][2] = {
        {1,2}, {2,1}, {2,-1}, {1,-2},
        {-1,-2}, {-2,-1}, {-2,1}, {-1,2}
    };

    for (auto& d : knight) {
        int nf = f + d[0];
        int nr = r + d[1];

        if (!inside(nf, nr))
            continue;

        if (b.board[nr * 8 + nf] ==
            (byWhite ? WN : BN))
            return true;
    }

    // Bishops / Queens
    static const int diagonal[4][2] = {
        {1,1}, {1,-1}, {-1,1}, {-1,-1}
    };

    for (auto& d : diagonal) {
        int nf = f;
        int nr = r;

        while (true) {
            nf += d[0];
            nr += d[1];

            if (!inside(nf, nr))
                break;

            int p =
                b.board[nr * 8 + nf];

            if (p) {
                if (p == (byWhite ? WB : BB) ||
                    p == (byWhite ? WQ : BQ))
                    return true;

                break;
            }
        }
    }

    // Rooks / Queens
    static const int straight[4][2] = {
        {1,0}, {-1,0}, {0,1}, {0,-1}
    };

    for (auto& d : straight) {
        int nf = f;
        int nr = r;

        while (true) {
            nf += d[0];
            nr += d[1];

            if (!inside(nf, nr))
                break;

            int p =
                b.board[nr * 8 + nf];

            if (p) {
                if (p == (byWhite ? WR : BR) ||
                    p == (byWhite ? WQ : BQ))
                    return true;

                break;
            }
        }
    }

    // King
    for (int df = -1; df <= 1; ++df) {
        for (int dr = -1; dr <= 1; ++dr) {

            if (!df && !dr)
                continue;

            int nf = f + df;
            int nr = r + dr;

            if (!inside(nf, nr))
                continue;

            if (b.board[nr * 8 + nf] ==
                (byWhite ? WK : BK))
                return true;
        }
    }

    return false;
}

static bool inCheck(
    const Board& b,
    bool white
) {
    int k = kingSquare(b, white);

    if (k < 0)
        return true;

    return attacked(b, k, !white);
}

// ============================================================
// MOVE GENERATION
// ============================================================

static void addMove(
    vector<Move>& moves,
    int from,
    int to,
    uint8_t flags = QUIET,
    int promotion = 0
) {
    moves.push_back(
        Move{
            (uint8_t)from,
            (uint8_t)to,
            (int8_t)promotion,
            flags
        }
    );
}

static void generatePseudo(
    const Board& b,
    vector<Move>& moves
) {
    moves.clear();

    bool white = b.white;

    for (int s = 0; s < 64; ++s) {

        int p = b.board[s];

        if (!ownPiece(p, white))
            continue;

        int f = fileOf(s);
        int r = rankOf(s);
        int type = abs(p);

        // PAWN
        if (type == 1) {

            int dir = white ? 1 : -1;
            int nr = r + dir;

            if (!inside(f, nr))
                continue;

            int to = nr * 8 + f;

            if (b.board[to] == EMPTY) {

                if (nr == 0 || nr == 7) {

                    int promos[4] = {
                        white ? WQ : BQ,
                        white ? WR : BR,
                        white ? WB : BB,
                        white ? WN : BN
                    };

                    for (int q : promos)
                        addMove(
                            moves,
                            s,
                            to,
                            PROMOTION,
                            q
                        );

                } else {
                    addMove(
                        moves,
                        s,
                        to
                    );
                }

                int startRank =
                    white ? 1 : 6;

                int nr2 =
                    r + dir * 2;

                if (
                    r == startRank &&
                    b.board[nr2 * 8 + f] == EMPTY
                ) {
                    addMove(
                        moves,
                        s,
                        nr2 * 8 + f,
                        DOUBLE_PUSH
                    );
                }
            }

            for (int df : {-1, 1}) {

                int nf = f + df;

                if (!inside(nf, nr))
                    continue;

                to = nr * 8 + nf;

                if (
                    b.board[to] != EMPTY &&
                    !ownPiece(b.board[to], white) &&
                    abs(b.board[to]) != 6
                ) {

                    if (nr == 0 || nr == 7) {

                        int promos[4] = {
                            white ? WQ : BQ,
                            white ? WR : BR,
                            white ? WB : BB,
                            white ? WN : BN
                        };

                        for (int q : promos)
                            addMove(
                                moves,
                                s,
                                to,
                                CAPTURE | PROMOTION,
                                q
                            );

                    } else {
                        addMove(
                            moves,
                            s,
                            to,
                            CAPTURE
                        );
                    }
                }

                if (to == b.ep) {
                    addMove(
                        moves,
                        s,
                        to,
                        EP_CAPTURE
                    );
                }
            }
        }

        // KNIGHT
        else if (type == 2) {

            static const int d[8][2] = {
                {1,2}, {2,1}, {2,-1}, {1,-2},
                {-1,-2}, {-2,-1}, {-2,1}, {-1,2}
            };

            for (auto& z : d) {

                int nf = f + z[0];
                int nr = r + z[1];

                if (!inside(nf, nr))
                    continue;

                int to =
                    nr * 8 + nf;

                if (
                    !ownPiece(
                        b.board[to],
                        white
                    ) &&
                    abs(b.board[to]) != 6
                ) {

                    addMove(
                        moves,
                        s,
                        to,
                        b.board[to]
                            ? CAPTURE
                            : QUIET
                    );
                }
            }
        }

        // BISHOP / ROOK / QUEEN
        else if (
            type == 3 ||
            type == 4 ||
            type == 5
        ) {

            static const int dirs[8][2] = {
                {1,1}, {1,-1}, {-1,1}, {-1,-1},
                {1,0}, {-1,0}, {0,1}, {0,-1}
            };

            int first =
                type == 4 ? 4 : 0;

            int last =
                type == 3 ? 4 : 8;

            for (int d = first; d < last; ++d) {

                int nf = f;
                int nr = r;

                while (true) {

                    nf += dirs[d][0];
                    nr += dirs[d][1];

                    if (!inside(nf, nr))
                        break;

                    int to =
                        nr * 8 + nf;

                    if (b.board[to] == EMPTY) {

                        addMove(
                            moves,
                            s,
                            to
                        );

                    } else {

                        if (
                            !ownPiece(
                                b.board[to],
                                white
                            ) &&
                            abs(b.board[to]) != 6
                        ) {
                            addMove(
                                moves,
                                s,
                                to,
                                CAPTURE
                            );
                        }

                        break;
                    }
                }
            }
        }

        // KING
        else if (type == 6) {

            for (int df = -1; df <= 1; ++df) {
                for (int dr = -1; dr <= 1; ++dr) {

                    if (!df && !dr)
                        continue;

                    int nf = f + df;
                    int nr = r + dr;

                    if (!inside(nf, nr))
                        continue;

                    int to =
                        nr * 8 + nf;

                    if (
                        !ownPiece(
                            b.board[to],
                            white
                        ) &&
                        abs(b.board[to]) != 6
                    ) {
                        addMove(
                            moves,
                            s,
                            to,
                            b.board[to]
                                ? CAPTURE
                                : QUIET
                        );
                    }
                }
            }

            // WHITE CASTLING
            if (
                white &&
                s == 4 &&
                !inCheck(b, true)
            ) {

                if (
                    (b.castle & 1) &&
                    b.board[5] == EMPTY &&
                    b.board[6] == EMPTY &&
                    b.board[7] == WR &&
                    !attacked(b, 5, false) &&
                    !attacked(b, 6, false)
                ) {
                    addMove(
                        moves,
                        4,
                        6,
                        KING_CASTLE
                    );
                }

                if (
                    (b.castle & 2) &&
                    b.board[1] == EMPTY &&
                    b.board[2] == EMPTY &&
                    b.board[3] == EMPTY &&
                    b.board[0] == WR &&
                    !attacked(b, 3, false) &&
                    !attacked(b, 2, false)
                ) {
                    addMove(
                        moves,
                        4,
                        2,
                        QUEEN_CASTLE
                    );
                }
            }

            // BLACK CASTLING
            if (
                !white &&
                s == 60 &&
                !inCheck(b, false)
            ) {

                if (
                    (b.castle & 4) &&
                    b.board[61] == EMPTY &&
                    b.board[62] == EMPTY &&
                    b.board[63] == BR &&
                    !attacked(b, 61, true) &&
                    !attacked(b, 62, true)
                ) {
                    addMove(
                        moves,
                        60,
                        62,
                        KING_CASTLE
                    );
                }

                if (
                    (b.castle & 8) &&
                    b.board[57] == EMPTY &&
                    b.board[58] == EMPTY &&
                    b.board[59] == EMPTY &&
                    b.board[56] == BR &&
                    !attacked(b, 59, true) &&
                    !attacked(b, 58, true)
                ) {
                    addMove(
                        moves,
                        60,
                        58,
                        QUEEN_CASTLE
                    );
                }
            }
        }
    }
}

// ============================================================
// MAKE MOVE
// ============================================================

static Board makeMove(
    const Board& b,
    const Move& m
) {
    Board n = b;

    int piece =
        n.board[m.from];

    int captured =
        n.board[m.to];

    n.board[m.from] = EMPTY;
    n.board[m.to] = piece;

    n.ep = -1;
    n.halfmove++;

    if (
        abs(piece) == 1 ||
        captured ||
        (m.flags & EP_CAPTURE)
    ) {
        n.halfmove = 0;
    }

    if (m.flags & EP_CAPTURE) {

        n.board[
            m.to + (b.white ? -8 : 8)
        ] = EMPTY;
    }

    if (m.flags & KING_CASTLE) {

        if (b.white) {
            n.board[5] = WR;
            n.board[7] = EMPTY;
        } else {
            n.board[61] = BR;
            n.board[63] = EMPTY;
        }
    }

    if (m.flags & QUEEN_CASTLE) {

        if (b.white) {
            n.board[3] = WR;
            n.board[0] = EMPTY;
        } else {
            n.board[59] = BR;
            n.board[56] = EMPTY;
        }
    }

    if (m.promotion)
        n.board[m.to] = m.promotion;

    if (m.flags & DOUBLE_PUSH) {
        n.ep =
            m.from +
            (b.white ? 8 : -8);
    }

    if (piece == WK)
        n.castle &= ~3;

    if (piece == BK)
        n.castle &= ~12;

    if (m.from == 0 || m.to == 0)
        n.castle &= ~2;

    if (m.from == 7 || m.to == 7)
        n.castle &= ~1;

    if (m.from == 56 || m.to == 56)
        n.castle &= ~8;

    if (m.from == 63 || m.to == 63)
        n.castle &= ~4;

    n.white = !b.white;

    if (n.white)
        n.fullmove++;

    return n;
}

// ============================================================
// LEGAL MOVES
// ============================================================

static vector<Move> legalMoves(
    const Board& b
) {
    vector<Move> pseudo;
    vector<Move> legal;

    generatePseudo(
        b,
        pseudo
    );

    legal.reserve(
        pseudo.size()
    );

    for (const Move& m : pseudo) {

        Board n =
            makeMove(b, m);

        if (!inCheck(n, b.white))
            legal.push_back(m);
    }

    return legal;
}

// ============================================================
// SIMPLELOGICS EVALUATION
// ============================================================

static int centerBonus(int s) {

    int f = fileOf(s);
    int r = rankOf(s);

    int distance =
        abs(f - 3) +
        abs(r - 3);

    return 14 -
           distance * 3;
}

static int evaluate(
    const Board& b
) {
    int score = 0;

    int whiteMaterial = 0;
    int blackMaterial = 0;

    int whiteBishops = 0;
    int blackBishops = 0;

    for (int s = 0; s < 64; ++s) {

        int p = b.board[s];

        if (!p)
            continue;

        int type = abs(p);
        int value = pieceValue(p);

        if (p > 0)
            whiteMaterial += value;
        else
            blackMaterial += value;

        int bonus = 0;

        // Pawn structure / advancement
        if (type == 1) {

            int advance =
                p > 0
                ? rankOf(s)
                : 7 - rankOf(s);

            bonus =
                advance * 6;
        }

        // Knights / bishops
        else if (
            type == 2 ||
            type == 3
        ) {
            bonus =
                centerBonus(s) / 2;
        }

        // Rook
        else if (type == 4) {

            int rr =
                rankOf(s);

            if (rr == 1 || rr == 6)
                bonus += 10;
        }

        // Queen
        else if (type == 5) {

            bonus =
                centerBonus(s) / 3;
        }

        if (p == WB)
            whiteBishops++;

        if (p == BB)
            blackBishops++;

        if (p > 0)
            score += value + bonus;
        else
            score -= value + bonus;
    }

    // Bishop pair
    if (whiteBishops >= 2)
        score += 30;

    if (blackBishops >= 2)
        score -= 30;

    // Mobility
    Board whiteBoard = b;
    whiteBoard.white = true;

    Board blackBoard = b;
    blackBoard.white = false;

    int whiteMob =
        (int)legalMoves(
            whiteBoard
        ).size();

    int blackMob =
        (int)legalMoves(
            blackBoard
        ).size();

    score +=
        (whiteMob - blackMob) * 4;

    // King activity in endgame
    if (
        whiteMaterial +
        blackMaterial <= 4500
    ) {

        int wk =
            kingSquare(b, true);

        int bk =
            kingSquare(b, false);

        if (wk >= 0)
            score += centerBonus(wk);

        if (bk >= 0)
            score -= centerBonus(bk);
    }

    return b.white
        ? score
        : -score;
}

// ============================================================
// PUCT NODE
// ============================================================

struct Node {

    Board board;

    vector<Move> moves;
    vector<int> children;

    int parent = -1;

    Move move{};

    int visits = 0;

    double value = 0.0;
};

static vector<Node> tree;

static atomic<bool> stopSearch(false);

static chrono::steady_clock::time_point deadline;

static uint64_t iterations = 0;

// ============================================================
// POLICY PRIOR
// ============================================================

static double policyPrior(
    const Board& b,
    const Move& m
) {
    double prior = 1.0;

    if (
        m.flags &
        (CAPTURE | EP_CAPTURE)
    ) {

        int captured =
            m.flags & EP_CAPTURE
            ? 100
            : pieceValue(
                b.board[m.to]
            );

        int attacker =
            pieceValue(
                b.board[m.from]
            );

        prior +=
            2.5 +
            captured / 120.0 -
            attacker / 400.0;
    }

    if (m.promotion)
        prior += 8.0;

    if (
        m.flags &
        (KING_CASTLE |
         QUEEN_CASTLE)
    ) {
        prior += 1.5;
    }

    // Center preference
    int f =
        fileOf(m.to);

    int r =
        rankOf(m.to);

    prior +=
        max(
            0,
            3 - abs(f - 3)
        ) * 0.18;

    prior +=
        max(
            0,
            3 - abs(r - 3)
        ) * 0.18;

    return max(
        0.05,
        prior
    );
}

// ============================================================
// NODE MANAGEMENT
// ============================================================

static int createNode(
    const Board& b,
    int parent,
    const Move& move
) {
    Node n;

    n.board = b;
    n.parent = parent;
    n.move = move;

    tree.push_back(
        std::move(n)
    );

    return (int)tree.size() - 1;
}

static void expandNode(
    int id
) {
    tree[id].moves =
        legalMoves(
            tree[id].board
        );

    tree[id].children.assign(
        tree[id].moves.size(),
        -1
    );
}

// ============================================================
// PUCT SELECTION
// ============================================================

static int selectChild(
    int id
) {
    Node& parent =
        tree[id];

    double bestScore =
        -1e100;

    int bestIndex = -1;

    double parentVisits =
        max(
            1,
            parent.visits
        );

    double sqrtParent =
        sqrt(
            (double)parentVisits
        );

    for (
        int i = 0;
        i < (int)parent.moves.size();
        ++i
    ) {

        int childId =
            parent.children[i];

        // Unvisited move
        if (childId < 0)
            return i;

        Node& child =
            tree[childId];

        double q = 0.0;

        if (child.visits > 0) {
            q =
                -child.value /
                child.visits;
        }

        double prior =
            policyPrior(
                parent.board,
                parent.moves[i]
            );

        double u =
            1.45 *
            prior *
            sqrtParent /
            (1.0 + child.visits);

        double score =
            q + u;

        if (score > bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }

    return bestIndex;
}

// ============================================================
// VALUE FUNCTION
// ============================================================

static double valueNetwork(
    const Board& b
) {
    int score =
        evaluate(b);

    return tanh(
        score / 700.0
    );
}

// ============================================================
// PUCT SEARCH
// ============================================================

static void searchPUCT(
    const Board& root,
    int milliseconds
) {
    tree.clear();

    tree.reserve(
        200000
    );

    int rootId =
        createNode(
            root,
            -1,
            Move{}
        );

    expandNode(
        rootId
    );

    if (
        tree[rootId]
            .moves.empty()
    ) {
        cout
            << "bestmove 0000"
            << endl;

        return;
    }

    iterations = 0;

    deadline =
        chrono::steady_clock::now() +
        chrono::milliseconds(
            max(
                30,
                milliseconds
            )
        );

    while (
        chrono::steady_clock::now() <
        deadline &&
        !stopSearch.load()
    ) {

        int nodeId =
            rootId;

        vector<int> path;

        path.reserve(64);

        path.push_back(
            rootId
        );

        // ----------------------------------------------------
        // Selection / expansion
        // ----------------------------------------------------

        while (true) {

            if (
                tree[nodeId]
                    .moves.empty()
            ) {
                expandNode(
                    nodeId
                );

                break;
            }

            int moveIndex =
                selectChild(
                    nodeId
                );

            if (moveIndex < 0)
                break;

            int childId =
                tree[nodeId]
                    .children[moveIndex];

            // New node
            if (childId < 0) {

                Board next =
                    makeMove(
                        tree[nodeId].board,
                        tree[nodeId]
                            .moves[moveIndex]
                    );

                childId =
                    createNode(
                        next,
                        nodeId,
                        tree[nodeId]
                            .moves[moveIndex]
                    );

                tree[nodeId]
                    .children[moveIndex] =
                    childId;

                nodeId =
                    childId;

                path.push_back(
                    nodeId
                );

                expandNode(
                    nodeId
                );

                break;
            }

            nodeId =
                childId;

            path.push_back(
                nodeId
            );

            if (
                tree[nodeId]
                    .visits == 0
            ) {
                break;
            }
        }

        // ----------------------------------------------------
        // Evaluation
        // ----------------------------------------------------

        double value =
            valueNetwork(
                tree[nodeId]
                    .board
            );

        // ----------------------------------------------------
        // Backpropagation
        // ----------------------------------------------------

        for (
            auto it = path.rbegin();
            it != path.rend();
            ++it
        ) {

            Node& n =
                tree[*it];

            n.visits++;

            n.value +=
                value;

            value = -value;
        }

        iterations++;

        if (
            (iterations & 4095ULL) == 0
        ) {

            if (
                chrono::steady_clock::now() >=
                deadline
            )
                break;
        }

        if (
            tree.size() >=
            190000
        )
            break;
    }

    // --------------------------------------------------------
    // Select best root move
    // --------------------------------------------------------

    int bestIndex = 0;
    int bestVisits = -1;
    double bestValue = -1e100;

    for (
        int i = 0;
        i < (int)tree[rootId]
            .moves.size();
        ++i
    ) {

        int child =
            tree[rootId]
                .children[i];

        if (child < 0)
            continue;

        int visits =
            tree[child]
                .visits;

        double value = 0.0;

        if (
            tree[child]
                .visits > 0
        ) {
            value =
                -tree[child].value /
                tree[child].visits;
        }

        if (
            visits > bestVisits ||
            (
                visits == bestVisits &&
                value > bestValue
            )
        ) {
            bestVisits = visits;
            bestValue = value;
            bestIndex = i;
        }
    }

    Move best =
        tree[rootId]
            .moves[bestIndex];

    cout
        << "info nodes "
        << iterations
        << " score cp "
        << evaluate(root)
        << " pv "
        << uciMove(best)
        << endl;

    cout
        << "bestmove "
        << uciMove(best)
        << endl;
}

// ============================================================
// UCI SQUARE
// ============================================================

static bool parseSquare(
    const string& s,
    int& out
) {
    if (
        s.size() != 2 ||
        s[0] < 'a' ||
        s[0] > 'h' ||
        s[1] < '1' ||
        s[1] > '8'
    )
        return false;

    out =
        (s[1] - '1') * 8 +
        (s[0] - 'a');

    return true;
}

// ============================================================
// INTEGER PARSER
// ============================================================

static int parseNumber(
    const string& s,
    int fallback
) {
    if (s.empty())
        return fallback;

    int sign = 1;
    size_t i = 0;

    if (s[0] == '-') {
        sign = -1;
        i = 1;
    }

    if (i >= s.size())
        return fallback;

    int result = 0;

    for (; i < s.size(); ++i) {

        if (
            s[i] < '0' ||
            s[i] > '9'
        )
            return fallback;

        result =
            result * 10 +
            (s[i] - '0');

        if (result > 100000000)
            return fallback;
    }

    return result * sign;
}

// ============================================================
// APPLY UCI MOVE
// ============================================================

static bool applyUCIMove(
    Board& b,
    const string& uci
) {
    if (uci.size() < 4)
        return false;

    int from;
    int to;

    if (
        !parseSquare(
            uci.substr(0, 2),
            from
        )
    )
        return false;

    if (
        !parseSquare(
            uci.substr(2, 2),
            to
        )
    )
        return false;

    int promotion = EMPTY;

    if (uci.size() >= 5) {

        char c =
            (char)tolower(
                (unsigned char)uci[4]
            );

        if (c == 'q')
            promotion =
                b.white ? WQ : BQ;

        else if (c == 'r')
            promotion =
                b.white ? WR : BR;

        else if (c == 'b')
            promotion =
                b.white ? WB : BB;

        else if (c == 'n')
            promotion =
                b.white ? WN : BN;
    }

    vector<Move> moves =
        legalMoves(b);

    for (const Move& m : moves) {

        if (
            m.from == from &&
            m.to == to &&
            (
                m.promotion ==
                promotion
            )
        ) {

            b =
                makeMove(
                    b,
                    m
                );

            return true;
        }
    }

    return false;
}

// ============================================================
// FEN
// ============================================================

static bool parseFEN(
    const string& fen,
    Board& out
) {
    vector<string> parts;

    stringstream ss(fen);

    string token;

    while (ss >> token)
        parts.push_back(token);

    if (parts.size() < 4)
        return false;

    Board b;

    b.board.fill(
        EMPTY
    );

    int rank = 7;
    int file = 0;

    for (char c : parts[0]) {

        if (c == '/') {
            rank--;
            file = 0;
            continue;
        }

        if (
            c >= '1' &&
            c <= '8'
        ) {
            file +=
                c - '0';

            continue;
        }

        int p = EMPTY;

        switch (c) {

            case 'P': p = WP; break;
            case 'N': p = WN; break;
            case 'B': p = WB; break;
            case 'R': p = WR; break;
            case 'Q': p = WQ; break;
            case 'K': p = WK; break;

            case 'p': p = BP; break;
            case 'n': p = BN; break;
            case 'b': p = BB; break;
            case 'r': p = BR; break;
            case 'q': p = BQ; break;
            case 'k': p = BK; break;

            default:
                return false;
        }

        if (
            rank < 0 ||
            file >= 8
        )
            return false;

        b.board[
            rank * 8 + file
        ] = p;

        file++;
    }

    if (
        rank != 0 ||
        file != 8
    )
        return false;

    b.white =
        parts[1] == "w";

    b.castle = 0;

    if (parts[2] != "-") {

        for (char c : parts[2]) {

            if (c == 'K')
                b.castle |= 1;

            else if (c == 'Q')
                b.castle |= 2;

            else if (c == 'k')
                b.castle |= 4;

            else if (c == 'q')
                b.castle |= 8;
        }
    }

    b.ep = -1;

    if (
        parts[3] != "-" &&
        !parseSquare(
            parts[3],
            b.ep
        )
    )
        return false;

    if (parts.size() >= 5) {

        b.halfmove =
            parseNumber(
                parts[4],
                0
            );
    }

    if (parts.size() >= 6) {

        b.fullmove =
            parseNumber(
                parts[5],
                1
            );
    }

    out = b;

    return true;
}

// ============================================================
// POSITION
// ============================================================

static void setPosition(
    Board& board,
    const string& line
) {
    stringstream ss(line);

    string token;

    ss >> token;

    ss >> token;

    if (token == "startpos") {

        board =
            Board::startpos();

        if (
            ss >> token &&
            token == "moves"
        ) {

            while (ss >> token) {
                applyUCIMove(
                    board,
                    token
                );
            }
        }

        return;
    }

    if (token == "fen") {

        vector<string> fenParts;

        while (
            ss >> token &&
            token != "moves" &&
            fenParts.size() < 6
        ) {
            fenParts.push_back(
                token
            );
        }

        string fen;

        for (
            const string& x :
            fenParts
        ) {

            if (!fen.empty())
                fen += ' ';

            fen += x;
        }

        Board parsed;

        if (
            parseFEN(
                fen,
                parsed
            )
        ) {
            board = parsed;
        }

        if (token == "moves") {

            while (ss >> token) {

                applyUCIMove(
                    board,
                    token
                );
            }
        }
    }
}

// ============================================================
// UCI
// ============================================================

static void printUCI() {

    cout
        << "id name SimpleLogics PUCT"
        << endl;

    cout
        << "id author Danny"
        << endl;

    cout
        << "option name Hash type spin "
        << "default 32 min 1 max 512"
        << endl;

    cout
        << "option name Threads type spin "
        << "default 1 min 1 max 1"
        << endl;

    cout
        << "option name Move Overhead type spin "
        << "default 50 min 0 max 1000"
        << endl;

    cout
        << "uciok"
        << endl;
}

// ============================================================
// TIME CONTROL
// ============================================================

static int calculateTime(
    const Board& b,
    const string& line
) {
    stringstream ss(line);

    string token;

    int moveTime = -1;

    int whiteTime = -1;
    int blackTime = -1;

    int whiteInc = 0;
    int blackInc = 0;

    while (ss >> token) {

        if (token == "movetime") {

            ss >> token;

            moveTime =
                parseNumber(
                    token,
                    -1
                );
        }

        else if (token == "wtime") {

            ss >> token;

            whiteTime =
                parseNumber(
                    token,
                    -1
                );
        }

        else if (token == "btime") {

            ss >> token;

            blackTime =
                parseNumber(
                    token,
                    -1
                );
        }

        else if (token == "winc") {

            ss >> token;

            whiteInc =
                parseNumber(
                    token,
                    0
                );
        }

        else if (token == "binc") {

            ss >> token;

            blackInc =
                parseNumber(
                    token,
                    0
                );
        }
    }

    if (moveTime >= 0)
        return max(
            50,
            moveTime - 50
        );

    int time =
        b.white
        ? whiteTime
        : blackTime;

    int increment =
        b.white
        ? whiteInc
        : blackInc;

    if (time < 0)
        return 1000;

    int budget =
        time / 30 +
        increment * 3 / 4;

    budget =
        max(
            50,
            budget
        );

    budget =
        min(
            budget,
            max(
                50,
                time / 3
            )
        );

    return min(
        budget,
        10000
    );
}

// ============================================================
// MAIN
// ============================================================

int main() {

    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    Board board =
        Board::startpos();

    string line;

    while (
        getline(cin, line)
    ) {

        if (line == "uci") {

            printUCI();
        }

        else if (
            line == "isready"
        ) {

            cout
                << "readyok"
                << endl;
        }

        else if (
            line.rfind(
                "position ",
                0
            ) == 0
        ) {

            setPosition(
                board,
                line
            );
        }

        else if (
            line == "ucinewgame"
        ) {

            board =
                Board::startpos();

            tree.clear();
        }

        else if (
            line.rfind(
                "go",
                0
            ) == 0
        ) {

            int milliseconds =
                calculateTime(
                    board,
                    line
                );

            stopSearch.store(
                false
            );

            searchPUCT(
                board,
                milliseconds
            );
        }

        else if (
            line == "stop"
        ) {

            stopSearch.store(
                true
            );
        }

        else if (
            line == "quit"
        ) {

            stopSearch.store(
                true
            );

            break;
        }
    }

    return 0;
}
