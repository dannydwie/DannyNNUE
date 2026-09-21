#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std;

enum Piece : int {
    EMPTY=0,
    WP=1, WN=2, WB=3, WR=4, WQ=5, WK=6,
    BP=-1, BN=-2, BB=-3, BR=-4, BQ=-5, BK=-6
};

enum Flag : uint8_t {
    QUIET=0,
    CAPTURE=1,
    DOUBLE_PUSH=2,
    EP_CAPTURE=4,
    KING_CASTLE=8,
    QUEEN_CASTLE=16,
    PROMOTION=32
};

struct Move {
    uint8_t from=0,to=0;
    int8_t promotion=0;
    uint8_t flags=QUIET;
};

static uint64_t zobrist[13][64];
static uint64_t zobSide,zobCastle[16],zobEp[64];

static uint64_t rng64() {
    static uint64_t x=0x9e3779b97f4a7c15ULL;
    x^=x>>12;
    x^=x<<25;
    x^=x>>27;
    return x*0x2545F4914F6CDD1DULL;
}

static void initZobrist() {
    for(auto &a:zobrist)
        for(auto &x:a)
            x=rng64();

    zobSide=rng64();

    for(auto &x:zobCastle)
        x=rng64();

    for(auto &x:zobEp)
        x=rng64();
}

struct Board {
    array<int,64> b{};
    bool white=true;
    uint8_t castle=15;
    int ep=-1;
    int halfmove=0;
    int fullmove=1;

    void startpos() {
        b.fill(EMPTY);

        const int back[8]={
            WR,WN,WB,WQ,WK,WB,WN,WR
        };

        for(int i=0;i<8;i++) {
            b[i]=back[i];
            b[8+i]=WP;
            b[48+i]=BP;
            b[56+i]=-back[i];
        }

        white=true;
        castle=15;
        ep=-1;
        halfmove=0;
        fullmove=1;
    }

    uint64_t key() const {
        uint64_t k=0;

        for(int s=0;s<64;s++) {
            if(!b[s])
                continue;

            int idx=
                b[s]>0
                ? b[s]-1
                : 6+(-b[s]-1);

            k^=zobrist[idx][s];
        }

        if(white)
            k^=zobSide;

        k^=zobCastle[castle];

        if(ep>=0)
            k^=zobEp[ep];

        return k;
    }
};

static atomic<bool> stopSearch(false);
static chrono::steady_clock::time_point deadline;
static uint64_t nodes=0;

static inline int fileOf(int s) {
    return s&7;
}

static inline int rankOf(int s) {
    return s>>3;
}

static inline bool inside(int f,int r) {
    return f>=0&&f<8&&r>=0&&r<8;
}

static inline bool own(int p,bool w) {
    return w?p>0:p<0;
}

static inline bool enemy(int p,bool w) {
    return w?p<0:p>0;
}

static int pieceValue(int p) {
    switch(abs(p)) {
        case WP:return 100;
        case WN:return 320;
        case WB:return 330;
        case WR:return 500;
        case WQ:return 900;
        case WK:return 20000;
    }

    return 0;
}

static string sq(int s) {
    string r="a1";

    r[0]=char('a'+fileOf(s));
    r[1]=char('1'+rankOf(s));

    return r;
}

static string uciMove(const Move&m) {
    if(m.from>=64||m.to>=64)
        return "0000";

    string r=sq(m.from)+sq(m.to);

    if(m.promotion) {
        switch(abs((int)m.promotion)) {
            case WQ:r+='q';break;
            case WR:r+='r';break;
            case WB:r+='b';break;
            case WN:r+='n';break;
        }
    }

    return r;
}

static bool attacked(
    const Board&x,
    int s,
    bool byWhite
) {
    int f=fileOf(s);
    int r=rankOf(s);

    int pr=byWhite?r-1:r+1;

    if(pr>=0&&pr<8) {
        for(int df:{-1,1}) {
            int nf=f+df;

            if(inside(nf,pr)) {
                int p=x.b[pr*8+nf];

                if(p==(byWhite?WP:BP))
                    return true;
            }
        }
    }

    static const int kn[8][2]={
        {1,2},{2,1},{2,-1},{1,-2},
        {-1,-2},{-2,-1},{-2,1},{-1,2}
    };

    for(auto d:kn) {
        int nf=f+d[0];
        int nr=r+d[1];

        if(
            inside(nf,nr)&&
            x.b[nr*8+nf]==(byWhite?WN:BN)
        )
            return true;
    }

    static const int diag[4][2]={
        {1,1},{1,-1},{-1,1},{-1,-1}
    };

    for(auto d:diag) {
        int nf=f;
        int nr=r;

        while(true) {
            nf+=d[0];
            nr+=d[1];

            if(!inside(nf,nr))
                break;

            int p=x.b[nr*8+nf];

            if(p) {
                if(
                    p==(byWhite?WB:BB)||
                    p==(byWhite?WQ:BQ)
                )
                    return true;

                break;
            }
        }
    }

    static const int ortho[4][2]={
        {1,0},{-1,0},{0,1},{0,-1}
    };

    for(auto d:ortho) {
        int nf=f;
        int nr=r;

        while(true) {
            nf+=d[0];
            nr+=d[1];

            if(!inside(nf,nr))
                break;

            int p=x.b[nr*8+nf];

            if(p) {
                if(
                    p==(byWhite?WR:BR)||
                    p==(byWhite?WQ:BQ)
                )
                    return true;

                break;
            }
        }
    }

    for(int df=-1;df<=1;df++) {
        for(int dr=-1;dr<=1;dr++) {
            if(!df&&!dr)
                continue;

            int nf=f+df;
            int nr=r+dr;

            if(
                inside(nf,nr)&&
                x.b[nr*8+nf]==(byWhite?WK:BK)
            )
                return true;
        }
    }

    return false;
}

static int kingSquare(
    const Board&x,
    bool w
) {
    int k=w?WK:BK;

    for(int s=0;s<64;s++) {
        if(x.b[s]==k)
            return s;
    }

    return -1;
}

static bool inCheck(
    const Board&x,
    bool w
) {
    int k=kingSquare(x,w);

    return k>=0&&attacked(x,k,!w);
}

static void add(
    vector<Move>&m,
    int f,
    int t,
    uint8_t flags=QUIET,
    int promo=0
) {
    m.push_back(
        Move{
            (uint8_t)f,
            (uint8_t)t,
            (int8_t)promo,
            flags
        }
    );
}

static void pseudo(
    const Board&x,
    vector<Move>&m
) {
    m.clear();

    bool w=x.white;

    for(int s=0;s<64;s++) {
        int p=x.b[s];

        if(!own(p,w))
            continue;

        int f=fileOf(s);
        int r=rankOf(s);
        int a=abs(p);

        if(a==WP) {
            int d=w?1:-1;
            int nr=r+d;

            if(inside(f,nr)) {
                int t=nr*8+f;

                if(x.b[t]==EMPTY) {
                    if(nr==0||nr==7) {
                        for(int q:{
                            w?WQ:BQ,
                            w?WR:BR,
                            w?WB:BB,
                            w?WN:BN
                        })
                            add(
                                m,
                                s,
                                t,
                                PROMOTION,
                                q
                            );
                    }
                    else {
                        add(m,s,t);
                    }

                    int sr=w?1:6;
                    int nr2=r+2*d;

                    if(
                        r==sr&&
                        x.b[nr2*8+f]==EMPTY
                    )
                        add(
                            m,
                            s,
                            nr2*8+f,
                            DOUBLE_PUSH
                        );
                }

                for(int df:{-1,1}) {
                    int nf=f+df;

                    if(!inside(nf,nr))
                        continue;

                    t=nr*8+nf;

                    if(
                        enemy(x.b[t],w)&&
                        abs(x.b[t])!=WK
                    ) {
                        if(nr==0||nr==7) {
                            for(int q:{
                                w?WQ:BQ,
                                w?WR:BR,
                                w?WB:BB,
                                w?WN:BN
                            })
                                add(
                                    m,
                                    s,
                                    t,
                                    CAPTURE|PROMOTION,
                                    q
                                );
                        }
                        else {
                            add(
                                m,
                                s,
                                t,
                                CAPTURE
                            );
                        }
                    }

                    if(t==x.ep) {
                        if(nr==0||nr==7) {
                            for(int q:{
                                w?WQ:BQ,
                                w?WR:BR,
                                w?WB:BB,
                                w?WN:BN
                            })
                                add(
                                    m,
                                    s,
                                    t,
                                    EP_CAPTURE|PROMOTION,
                                    q
                                );
                        }
                        else {
                            add(
                                m,
                                s,
                                t,
                                EP_CAPTURE
                            );
                        }
                    }
                }
            }
        }

        else if(a==WN) {
            static const int d[8][2]={
                {1,2},{2,1},{2,-1},{1,-2},
                {-1,-2},{-2,-1},{-2,1},{-1,2}
            };

            for(auto z:d) {
                int nf=f+z[0];
                int nr=r+z[1];

                if(!inside(nf,nr))
                    continue;

                int t=nr*8+nf;

                if(
                    !own(x.b[t],w)&&
                    abs(x.b[t])!=WK
                )
                    add(
                        m,
                        s,
                        t,
                        x.b[t]?CAPTURE:QUIET
                    );
            }
        }

        else if(a==WB||a==WR||a==WQ) {
            static const int dirs[8][2]={
                {1,1},{1,-1},{-1,1},{-1,-1},
                {1,0},{-1,0},{0,1},{0,-1}
            };

            int first=
                a==WB?0:
                a==WR?4:0;

            int last=
                a==WB?4:
                a==WR?8:8;

            for(int di=first;di<last;di++) {
                int nf=f;
                int nr=r;

                while(true) {
                    nf+=dirs[di][0];
                    nr+=dirs[di][1];

                    if(!inside(nf,nr))
                        break;

                    int t=nr*8+nf;

                    if(x.b[t]==EMPTY) {
                        add(m,s,t);
                    }
                    else {
                        if(
                            enemy(x.b[t],w)&&
                            abs(x.b[t])!=WK
                        )
                            add(
                                m,
                                s,
                                t,
                                CAPTURE
                            );

                        break;
                    }
                }
            }
        }

        else if(a==WK) {
            for(int df=-1;df<=1;df++) {
                for(int dr=-1;dr<=1;dr++) {
                    if(!df&&!dr)
                        continue;

                    int nf=f+df;
                    int nr=r+dr;

                    if(!inside(nf,nr))
                        continue;

                    int t=nr*8+nf;

                    if(
                        !own(x.b[t],w)&&
                        abs(x.b[t])!=WK
                    )
                        add(
                            m,
                            s,
                            t,
                            x.b[t]?CAPTURE:QUIET
                        );
                }
            }

            if(w&&s==4&&!inCheck(x,true)) {
                if(
                    (x.castle&1)&&
                    x.b[5]==EMPTY&&
                    x.b[6]==EMPTY&&
                    !attacked(x,5,false)&&
                    !attacked(x,6,false)
                )
                    add(m,4,6,KING_CASTLE);

                if(
                    (x.castle&2)&&
                    x.b[1]==EMPTY&&
                    x.b[2]==EMPTY&&
                    x.b[3]==EMPTY&&
                    !attacked(x,3,false)&&
                    !attacked(x,2,false)
                )
                    add(m,4,2,QUEEN_CASTLE);
            }

            if(!w&&s==60&&!inCheck(x,false)) {
                if(
                    (x.castle&4)&&
                    x.b[61]==EMPTY&&
                    x.b[62]==EMPTY&&
                    !attacked(x,61,true)&&
                    !attacked(x,62,true)
                )
                    add(m,60,62,KING_CASTLE);

                if(
                    (x.castle&8)&&
                    x.b[57]==EMPTY&&
                    x.b[58]==EMPTY&&
                    x.b[59]==EMPTY&&
                    !attacked(x,59,true)&&
                    !attacked(x,58,true)
                )
                    add(m,60,58,QUEEN_CASTLE);
            }
        }
    }
}

static Board makeMove(const Board&x,const Move&m) {
    Board n=x;

    int p=n.b[m.from];
    int captured=n.b[m.to];

    n.ep=-1;
    n.halfmove++;

    if(abs(p)==WP||captured)
        n.halfmove=0;

    n.b[m.to]=p;
    n.b[m.from]=EMPTY;

    if(m.flags&EP_CAPTURE) {
        int cs=m.to+(x.white?-8:8);
        n.b[cs]=EMPTY;
    }

    if(m.flags&KING_CASTLE) {
        if(x.white) {
            n.b[5]=WR;
            n.b[7]=EMPTY;
        }
        else {
            n.b[61]=BR;
            n.b[63]=EMPTY;
        }
    }

    if(m.flags&QUEEN_CASTLE) {
        if(x.white) {
            n.b[3]=WR;
            n.b[0]=EMPTY;
        }
        else {
            n.b[59]=BR;
            n.b[56]=EMPTY;
        }
    }

    if(m.promotion)
        n.b[m.to]=m.promotion;

    if(m.flags&DOUBLE_PUSH)
        n.ep=m.from+(x.white?8:-8);

    if(p==WK)
        n.castle&=~3;

    if(p==BK)
        n.castle&=~12;

    if(m.from==0||m.to==0)
        n.castle&=~2;

    if(m.from==7||m.to==7)
        n.castle&=~1;

    if(m.from==56||m.to==56)
        n.castle&=~8;

    if(m.from==63||m.to==63)
        n.castle&=~4;

    n.white=!x.white;

    if(!n.white)
        n.fullmove++;

    return n;
}

static vector<Move> legalMoves(const Board&x) {
    vector<Move> p,l;
    pseudo(x,p);

    for(const Move&m:p) {
        Board n=makeMove(x,m);

        if(!inCheck(n,x.white))
            l.push_back(m);
    }

    return l;
}

static int evaluate(const Board&x) {
    static const int pawnP[64]={
         0,  0,  0,  0,  0,  0,  0,  0,
         5, 10, 10,-20,-20, 10, 10,  5,
         5, -5,-10,  0,  0,-10, -5,  5,
         0,  0,  0, 20, 20,  0,  0,  0,
         5,  5, 10, 25, 25, 10,  5,  5,
        10, 10, 20, 30, 30, 20, 10, 10,
        50, 50, 50, 50, 50, 50, 50, 50,
         0,  0,  0,  0,  0,  0,  0,  0
    };

    static const int knightP[64]={
        -50,-40,-30,-30,-30,-30,-40,-50,
        -40,-20,  0,  5,  5,  0,-20,-40,
        -30,  5, 10, 15, 15, 10,  5,-30,
        -30,  0, 15, 20, 20, 15,  0,-30,
        -30,  5, 15, 20, 20, 15,  5,-30,
        -30,  0, 10, 15, 15, 10,  0,-30,
        -40,-20,  0,  0,  0,  0,-20,-40,
        -50,-40,-30,-30,-30,-30,-40,-50
    };

    static const int bishopP[64]={
        -20,-10,-10,-10,-10,-10,-10,-20,
        -10,  5,  0,  0,  0,  0,  5,-10,
        -10, 10, 10, 10, 10, 10, 10,-10,
        -10,  0, 10, 10, 10, 10,  0,-10,
        -10,  5,  5, 10, 10,  5,  5,-10,
        -10,  0,  5, 10, 10,  5,  0,-10,
        -10,  0,  0,  0,  0,  0,  0,-10,
        -20,-10,-10,-10,-10,-10,-10,-20
    };

    static const int rookP[64]={
         0,  0,  0,  5,  5,  0,  0,  0,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
         5, 10, 10, 10, 10, 10, 10,  5,
         0,  0,  0,  0,  0,  0,  0,  0
    };

    static const int queenP[64]={
        -20,-10,-10, -5, -5,-10,-10,-20,
        -10,  0,  0,  0,  0,  0,  0,-10,
        -10,  0,  5,  5,  5,  5,  0,-10,
         -5,  0,  5,  5,  5,  5,  0, -5,
          0,  0,  5,  5,  5,  5,  0, -5,
        -10,  5,  5,  5,  5,  5,  0,-10,
        -10,  0,  5,  0,  0,  0,  0,-10,
        -20,-10,-10, -5, -5,-10,-10,-20
    };

    static const int kingP[64]={
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -20,-30,-30,-40,-40,-30,-30,-20,
        -10,-20,-20,-20,-20,-20,-20,-10,
         20, 20,  0,  0,  0,  0, 20, 20,
         20, 30, 10,  0,  0, 10, 30, 20
    };

    int score=0;
    int bishops[2]={0,0};
    int pawns[2][8]={};

    for(int s=0;s<64;s++) {
        int p=x.b[s];

        if(!p)
            continue;

        int side=p>0?0:1;
        int a=abs(p);
        int ps=side==0?s:63-s;
        int v=pieceValue(p);

        if(a==WP) {
            v+=pawnP[ps];
            pawns[side][fileOf(s)]++;
        }
        else if(a==WN) {
            v+=knightP[ps];
        }
        else if(a==WB) {
            v+=bishopP[ps];
            bishops[side]++;
        }
        else if(a==WR) {
            v+=rookP[ps];
        }
        else if(a==WQ) {
            v+=queenP[ps];
        }
        else if(a==WK) {
            v+=kingP[ps];
        }

        score+=p>0?v:-v;
    }

    // Bishop pair
    if(bishops[0]>=2)
        score+=30;

    if(bishops[1]>=2)
        score-=30;

    // Pawn structure
    for(int side=0;side<2;side++) {
        int sign=side==0?1:-1;

        for(int f=0;f<8;f++) {
            if(pawns[side][f]>=2)
                score-=sign*12;

            if(pawns[side][f]>0) {
                bool left=
                    f>0&&
                    pawns[side][f-1]>0;

                bool right=
                    f<7&&
                    pawns[side][f+1]>0;

                if(!left&&!right)
                    score-=sign*10;
            }
        }
    }

    // Rooks on open and semi-open files
    for(int s=0;s<64;s++) {
        int p=x.b[s];

        if(abs(p)!=WR)
            continue;

        int f=fileOf(s);
        int side=p>0?0:1;

        bool ownPawn=
            pawns[side][f]>0;

        bool enemyPawn=
            pawns[1-side][f]>0;

        if(!ownPawn&&!enemyPawn)
            score+=(side==0?1:-1)*18;
        else if(!ownPawn)
            score+=(side==0?1:-1)*10;
    }

    return x.white?score:-score;
}

struct TTEntry {
    int depth;
    int score;
    uint8_t flag;
    Move best;
};

static unordered_map<uint64_t,TTEntry> tt;

// Search heuristics
static Move killerMoves[64][2];
static int historyHeuristic[2][64][64]{};

static bool outOfTime() {
    return
        stopSearch.load()||
        chrono::steady_clock::now()>=deadline;
}

static int moveScore(
    const Board&x,
    const Move&m,
    const Move*ttMove,
    int ply
) {
    if(
        ttMove&&
        m.from==ttMove->from&&
        m.to==ttMove->to&&
        m.promotion==ttMove->promotion
    )
        return 20000000;

    if(m.flags&PROMOTION)
        return 9000000+
               pieceValue(m.promotion);

    if(m.flags&(CAPTURE|EP_CAPTURE)) {
        int victim=
            (m.flags&EP_CAPTURE)
            ? WP
            : x.b[m.to];

        int attacker=x.b[m.from];

        return
            1000000+
            pieceValue(victim)*16-
            pieceValue(attacker);
    }

    if(ply<64) {
        if(
            m.from==killerMoves[ply][0].from&&
            m.to==killerMoves[ply][0].to&&
            m.promotion==killerMoves[ply][0].promotion
        )
            return 800000;

        if(
            m.from==killerMoves[ply][1].from&&
            m.to==killerMoves[ply][1].to&&
            m.promotion==killerMoves[ply][1].promotion
        )
            return 700000;
    }

    int side=x.white?0:1;

    return historyHeuristic[side][m.from][m.to];
}

static void orderMoves(
    const Board&x,
    vector<Move>&moves,
    const Move*ttMove,
    int ply
) {
    stable_sort(
        moves.begin(),
        moves.end(),
        [&](const Move&a,const Move&b) {
            return
                moveScore(x,a,ttMove,ply)>
                moveScore(x,b,ttMove,ply);
        }
    );
}

static int qsearch(
    const Board&x,
    int alpha,
    int beta,
    int qdepth=0
) {
    if(outOfTime())
        return evaluate(x);

    nodes++;

    int stand=evaluate(x);

    if(stand>=beta)
        return beta;

    if(stand>alpha)
        alpha=stand;

    if(qdepth>=5)
        return alpha;

    auto moves=legalMoves(x);

    orderMoves(
        x,
        moves,
        nullptr,
        0
    );

    for(const Move&m:moves) {
        if(!(m.flags&(CAPTURE|EP_CAPTURE|PROMOTION)))
            continue;

        if(outOfTime())
            break;

        Board n=makeMove(x,m);

        int s=-qsearch(
            n,
            -beta,
            -alpha,
            qdepth+1
        );

        if(s>=beta)
            return beta;

        if(s>alpha)
            alpha=s;
    }

    return alpha;
}

static int search(
    const Board&x,
    int depth,
    int alpha,
    int beta,
    int ply,
    Move*pvBest
) {
    if(outOfTime())
        return evaluate(x);

    nodes++;

    bool root=ply==0;

    uint64_t key=x.key();

    auto it=tt.find(key);

    Move ttMove{};

    if(it!=tt.end()) {
        ttMove=it->second.best;

        if(
            !root&&
            it->second.depth>=depth
        ) {
            if(it->second.flag==0)
                return it->second.score;

            if(
                it->second.flag==1&&
                it->second.score<=alpha
            )
                return it->second.score;

            if(
                it->second.flag==2&&
                it->second.score>=beta
            )
                return it->second.score;
        }
    }

    if(depth<=0)
        return qsearch(x,alpha,beta);

    auto moves=legalMoves(x);

    if(moves.empty()) {
        if(inCheck(x,x.white))
            return -100000+ply;

        return 0;
    }

    orderMoves(
        x,
        moves,
        it!=tt.end()?&ttMove:nullptr,
        ply
    );

    int originalAlpha=alpha;
    int bestScore=-1000000;
    Move bestMove=moves[0];

    for(size_t i=0;i<moves.size();i++) {
        const Move&m=moves[i];

        if(outOfTime())
            break;

        Board n=makeMove(x,m);

        int extension=0;

        // Check extension
        if(inCheck(n,n.white))
            extension=1;

        int nextDepth=
            depth-1+extension;

        int score;

        // Principal variation search
        if(i==0) {
            score=-search(
                n,
                nextDepth,
                -beta,
                -alpha,
                ply+1,
                nullptr
            );
        }
        else {
            score=-search(
                n,
                nextDepth,
                -alpha-1,
                -alpha,
                ply+1,
                nullptr
            );

            if(
                score>alpha&&
                score<beta
            ) {
                score=-search(
                    n,
                    nextDepth,
                    -beta,
                    -alpha,
                    ply+1,
                    nullptr
                );
            }
        }

        if(score>bestScore) {
            bestScore=score;
            bestMove=m;
        }

        if(score>alpha)
            alpha=score;

        if(alpha>=beta) {
            // Killer move
            if(!(m.flags&(CAPTURE|EP_CAPTURE))) {
                if(
                    !(m.from==killerMoves[ply][0].from&&
                      m.to==killerMoves[ply][0].to)
                ) {
                    killerMoves[ply][1]=killerMoves[ply][0];
                    killerMoves[ply][0]=m;
                }

                int side=x.white?0:1;

                historyHeuristic
                    [side]
                    [m.from]
                    [m.to]
                    +=depth*depth;

                if(
                    historyHeuristic
                        [side]
                        [m.from]
                        [m.to]>100000
                )
                    historyHeuristic
                        [side]
                        [m.from]
                        [m.to]=100000;
            }

            break;
        }
    }

    if(pvBest)
        *pvBest=bestMove;

    uint8_t flag=
        bestScore<=originalAlpha
        ?1
        :(bestScore>=beta?2:0);

    tt[key]=TTEntry{
        depth,
        bestScore,
        flag,
        bestMove
    };

    return bestScore;
}

static Move think(
    const Board&x,
    int maxDepth
) {
    auto root=legalMoves(x);

    if(root.empty())
        return Move{0,0,0,QUIET};

    Move best=root[0];

    int bestScore=-1000000;

    for(int d=1;d<=maxDepth;d++) {
        if(outOfTime())
            break;

        Move current=best;

        int score=search(
            x,
            d,
            -1000000,
            1000000,
            0,
            &current
        );

        if(outOfTime())
            break;

        best=current;
        bestScore=score;

        cout
            <<"info depth "<<d
            <<" score cp "<<bestScore
            <<" nodes "<<nodes
            <<" pv "<<uciMove(best)
            <<"\n";

        cout.flush();
    }

    return best;
}

struct Go {
    long long wtime=-1;
    long long btime=-1;
    long long winc=0;
    long long binc=0;
    long long movetime=-1;

    int depth=0;
    bool infinite=false;
};

static Go parseGo(
    const string&line
) {
    Go g;

    stringstream ss(line);
    string t;

    ss>>t;

    while(ss>>t) {
        if(t=="wtime")
            ss>>g.wtime;

        else if(t=="btime")
            ss>>g.btime;

        else if(t=="winc")
            ss>>g.winc;

        else if(t=="binc")
            ss>>g.binc;

        else if(t=="movetime")
            ss>>g.movetime;

        else if(t=="depth")
            ss>>g.depth;

        else if(t=="infinite")
            g.infinite=true;
    }

    return g;
}

static long long timeBudget(
    const Board&x,
    const Go&g
) {
    if(g.movetime>=0)
        return max(
            30LL,
            g.movetime-30
        );

    if(g.infinite)
        return 3600000;

    long long time=
        x.white
        ?g.wtime
        :g.btime;

    long long inc=
        x.white
        ?g.winc
        :g.binc;

    if(time<0)
        return 500;

    long long budget=
        time/30+
        inc*8/10;

    budget=max(40LL,budget);

    budget=min(
        budget,
        max(40LL,time/2)
    );

    return min(
        budget,
        5000LL
    );
}

static bool parseUciMove(
    const Board&x,
    const string&s,
    Move&out
) {
    auto moves=legalMoves(x);

    for(const Move&m:moves) {
        if(uciMove(m)==s) {
            out=m;
            return true;
        }
    }

    return false;
}

static void setFEN(
    Board&b,
    const string&fen
) {
    b.b.fill(EMPTY);
    b.castle=0;
    b.ep=-1;
    b.halfmove=0;
    b.fullmove=1;

    stringstream ss(fen);

    string placement;
    string side;
    string cast;
    string ep;
    string hm;
    string fm;

    ss>>placement
      >>side
      >>cast
      >>ep
      >>hm
      >>fm;

    int rank=7;
    int file=0;

    for(char c:placement) {
        if(c=='/') {
            rank--;
            file=0;
            continue;
        }

        if(c>='1'&&c<='8') {
            file+=c-'0';
            continue;
        }

        if(
            rank<0||
            file>7
        )
            continue;

        int p=EMPTY;

        switch(c) {
            case 'P':p=WP;break;
            case 'N':p=WN;break;
            case 'B':p=WB;break;
            case 'R':p=WR;break;
            case 'Q':p=WQ;break;
            case 'K':p=WK;break;

            case 'p':p=BP;break;
            case 'n':p=BN;break;
            case 'b':p=BB;break;
            case 'r':p=BR;break;
            case 'q':p=BQ;break;
            case 'k':p=BK;break;
        }

        if(p)
            b.b[rank*8+file]=p;

        file++;
    }

    b.white=side!="b";

    if(cast.find('K')!=string::npos)
        b.castle|=1;

    if(cast.find('Q')!=string::npos)
        b.castle|=2;

    if(cast.find('k')!=string::npos)
        b.castle|=4;

    if(cast.find('q')!=string::npos)
        b.castle|=8;

    if(
        ep!="-"&&
        ep.size()==2
    )
        b.ep=
            (ep[1]-'1')*8+
            (ep[0]-'a');

    if(!hm.empty())
        b.halfmove=stoi(hm);

    if(!fm.empty())
        b.fullmove=stoi(fm);
}

static void applyPosition(
    Board&b,
    const string&line
) {
    if(
        line.rfind(
            "position startpos",
            0
        )==0
    ) {
        b.startpos();

        size_t m=line.find("moves");

        if(m!=string::npos) {
            stringstream ss(
                line.substr(m+5)
            );

            string u;

            while(ss>>u) {
                Move mv;

                if(parseUciMove(b,u,mv))
                    b=makeMove(b,mv);
            }
        }

        return;
    }

    size_t f=
        line.find("position fen ");

    if(f==string::npos)
        return;

    size_t mp=
        line.find(" moves",f+13);

    string fen=
        mp==string::npos
        ?line.substr(f+13)
        :line.substr(
            f+13,
            mp-(f+13)
        );

    setFEN(b,fen);

    if(mp!=string::npos) {
        stringstream ss(
            line.substr(mp+7)
        );

        string u;

        while(ss>>u) {
            Move mv;

            if(parseUciMove(b,u,mv))
                b=makeMove(b,mv);
        }
    }
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    initZobrist();

    Board board;
    board.startpos();

    string line;

    thread worker;
    bool searching=false;

    auto stopAndJoin=[&]() {
        if(searching) {
            stopSearch=true;

            if(worker.joinable())
                worker.join();

            searching=false;
        }
    };

    while(getline(cin,line)) {

        if(line=="uci") {
            cout
                <<"id name SimpleLogics 0.4 Strength\n";

            cout
                <<"id author Danny\n";

            cout
                <<"uciok\n"
                <<flush;
        }

        else if(line=="isready") {
            cout
                <<"readyok\n"
                <<flush;
        }

        else if(line=="ucinewgame") {
            stopAndJoin();

            tt.clear();

            memset(
                killerMoves,
                0,
                sizeof(killerMoves)
            );

            memset(
                historyHeuristic,
                0,
                sizeof(historyHeuristic)
            );

            board.startpos();
        }

        else if(
            line.rfind(
                "position startpos",
                0
            )==0||
            line.rfind(
                "position fen ",
                0
            )==0
        ) {
            stopAndJoin();

            applyPosition(
                board,
                line
            );
        }

        else if(
            line.rfind("go",0)==0
        ) {
            stopAndJoin();

            Board pos=board;
            Go g=parseGo(line);

            searching=true;
            stopSearch=false;
            nodes=0;

            worker=thread(
                [pos,g]() mutable {
                    auto start=
                        chrono::steady_clock::now();

                    deadline=
                        start+
                        chrono::milliseconds(
                            timeBudget(pos,g)
                        );

                    int maxDepth=
                        g.depth
                        ?g.depth
                        :64;

                    Move bm=
                        think(
                            pos,
                            maxDepth
                        );

                    cout
                        <<"bestmove "
                        <<uciMove(bm)
                        <<"\n"
                        <<flush;
                }
            );
        }

        else if(line=="stop") {
            stopAndJoin();
        }

        else if(line=="quit") {
            stopAndJoin();
            break;
        }
    }

    stopAndJoin();

    return 0;
}
