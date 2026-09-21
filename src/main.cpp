#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
    QUIET=0, CAPTURE=1, DOUBLE_PUSH=2, EP_CAPTURE=4,
    KING_CASTLE=8, QUEEN_CASTLE=16, PROMOTION=32
};

struct Move {
    uint8_t from=0, to=0;
    int8_t promotion=0;
    uint8_t flags=QUIET;
};

static uint64_t zobrist[13][64];
static uint64_t zobSide, zobCastle[16], zobEp[64];

static uint64_t rng64() {
    static uint64_t x=0x9e3779b97f4a7c15ULL;
    x^=x>>12; x^=x<<25; x^=x>>27;
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
    int halfmove=0, fullmove=1;

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
            if(b[s]) {
                int idx =
                    b[s]>0
                    ? (b[s]-1)
                    : (6+(-b[s]-1));

                k^=zobrist[idx][s];
            }
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
    if(m.from>=64 || m.to>=64)
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
    const Board& x,
    int s,
    bool byWhite
) {
    int f=fileOf(s);
    int r=rankOf(s);

    // Pawns
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

    // Knights
    static const int kn[8][2]={
        {1,2},{2,1},{2,-1},{1,-2},
        {-1,-2},{-2,-1},{-2,1},{-1,2}
    };

    for(auto d:kn) {
        int nf=f+d[0];
        int nr=r+d[1];

        if(inside(nf,nr) &&
           x.b[nr*8+nf]==(byWhite?WN:BN))
            return true;
    }

    // Bishops / Queens
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
                if(p==(byWhite?WB:BB) ||
                   p==(byWhite?WQ:BQ))
                    return true;

                break;
            }
        }
    }

    // Rooks / Queens
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
                if(p==(byWhite?WR:BR) ||
                   p==(byWhite?WQ:BQ))
                    return true;

                break;
            }
        }
    }

    // King
    for(int df=-1;df<=1;df++) {
        for(int dr=-1;dr<=1;dr++) {
            if(!df&&!dr)
                continue;

            int nf=f+df;
            int nr=r+dr;

            if(inside(nf,nr) &&
               x.b[nr*8+nf]==(byWhite?WK:BK))
                return true;
        }
    }

    return false;
}

static int kingSquare(
    const Board& x,
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
    const Board& x,
    bool w
) {
    int k=kingSquare(x,w);

    return k>=0 && attacked(x,k,!w);
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
    const Board& x,
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

        // Pawn
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
                            add(m,s,t,PROMOTION,q);
                    }
                    else {
                        add(m,s,t);
                    }

                    int sr=w?1:6;
                    int nr2=r+2*d;

                    if(r==sr &&
                       x.b[nr2*8+f]==EMPTY)
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

                    if(enemy(x.b[t],w) &&
                       abs(x.b[t])!=WK) {

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

        // Knight
        else if(a==WN) {
            static const int d[8][2]={
                {1,2},{2,1},{2,-1},{1,-2},
                {-1,-2},{-2,-1},{-2,1},{-1,2}
            };

            for(auto z:d) {
                int nf=f+z[0];
                int nr=r+z[1];

                if(inside(nf,nr)) {
                    int t=nr*8+nf;

                    if(!own(x.b[t],w) &&
                       abs(x.b[t])!=WK)
                        add(
                            m,
                            s,
                            t,
                            x.b[t]?CAPTURE:QUIET
                        );
                }
            }
        }

        // Bishop / Rook / Queen
        else if(a==WB||a==WR||a==WQ) {
            static const int dirs[8][2]={
                {1,1},{1,-1},{-1,1},{-1,-1},
                {1,0},{-1,0},{0,1},{0,-1}
            };

            int first=a==WB?0:a==WR?4:0;
            int last=a==WB?4:a==WR?8:8;

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
                        if(enemy(x.b[t],w) &&
                           abs(x.b[t])!=WK)
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

        // King
        else if(a==WK) {
            for(int df=-1;df<=1;df++) {
                for(int dr=-1;dr<=1;dr++) {
                    if(!df&&!dr)
                        continue;

                    int nf=f+df;
                    int nr=r+dr;

                    if(inside(nf,nr)) {
                        int t=nr*8+nf;

                        if(!own(x.b[t],w) &&
                           abs(x.b[t])!=WK)
                            add(
                                m,
                                s,
                                t,
                                x.b[t]?CAPTURE:QUIET
                            );
                    }
                }
            }

            // White castling
            if(w && s==4 && !inCheck(x,true)) {
                if(
                    (x.castle&1) &&
                    x.b[5]==EMPTY &&
                    x.b[6]==EMPTY &&
                    !attacked(x,5,false) &&
                    !attacked(x,6,false)
                )
                    add(
                        m,
                        4,
                        6,
                        KING_CASTLE
                    );

                if(
                    (x.castle&2) &&
                    x.b[1]==EMPTY &&
                    x.b[2]==EMPTY &&
                    x.b[3]==EMPTY &&
                    !attacked(x,3,false) &&
                    !attacked(x,2,false)
                )
                    add(
                        m,
                        4,
                        2,
                        QUEEN_CASTLE
                    );
            }

            // Black castling
            if(!w && s==60 && !inCheck(x,false)) {
                if(
                    (x.castle&4) &&
                    x.b[61]==EMPTY &&
                    x.b[62]==EMPTY &&
                    !attacked(x,61,true) &&
                    !attacked(x,62,true)
                )
                    add(
                        m,
                        60,
                        62,
                        KING_CASTLE
                    );

                if(
                    (x.castle&8) &&
                    x.b[57]==EMPTY &&
                    x.b[58]==EMPTY &&
                    x.b[59]==EMPTY &&
                    !attacked(x,59,true) &&
                    !attacked(x,58,true)
                )
                    add(
                        m,
                        60,
                        58,
                        QUEEN_CASTLE
                    );
            }
        }
    }
}

static Board makeMove(
    const Board&x,
    const Move&m
) {
    Board n=x;

    int p=n.b[m.from];
    int captured=n.b[m.to];

    n.ep=-1;
    n.halfmove++;

    if(abs(p)==WP||captured)
        n.halfmove=0;

    n.b[m.to]=p;
    n.b[m.from]=EMPTY;

    // En passant
    if(m.flags&EP_CAPTURE) {
        int cs=m.to+(x.white?-8:8);
        n.b[cs]=EMPTY;
    }

    // King side castling
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

    // Queen side castling
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

    // Castling rights
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

static vector<Move> legalMoves(
    const Board&x
) {
    vector<Move> p,l;

    pseudo(x,p);

    for(const Move&m:p) {
        Board n=makeMove(x,m);

        if(!inCheck(n,x.white))
            l.push_back(m);
    }

    return l;
}

static int evaluate(
    const Board&x
) {
    static const int pstP[64]={
        0,0,0,0,0,0,0,0,
        5,10,10,-20,-20,10,10,5,
        5,-5,-10,0,0,-10,-5,5,
        0,0,0,20,20,0,0,0,
        5,5,10,25,25,10,5,5,
        10,10,20,30,30,20,10,10,
        50,50,50,50,50,50,50,50,
        0,0,0,0,0,0,0,0
    };

    int score=0;

    for(int s=0;s<64;s++) {
        int p=x.b[s];

        if(!p)
            continue;

        int v=pieceValue(p);

        if(abs(p)==WP)
            v+=x.b[s]>0?pstP[s]:pstP[63-s];

        score+=p>0?v:-v;
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

static bool outOfTime() {
    return
        stopSearch.load() ||
        chrono::steady_clock::now()>=deadline;
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

    if(qdepth>3)
        return evaluate(x);

    int stand=evaluate(x);

    if(stand>=beta)
        return beta;

    if(stand>alpha)
        alpha=stand;

    auto moves=legalMoves(x);

    for(const Move&m:moves) {
        if(!(m.flags&(CAPTURE|EP_CAPTURE|PROMOTION)))
            continue;

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
    Move*pvBest
) {
    if(outOfTime())
        return evaluate(x);

    nodes++;

    uint64_t key=x.key();

    auto it=tt.find(key);

    if(
        it!=tt.end() &&
        it->second.depth>=depth
    ) {
        if(it->second.flag==0)
            return it->second.score;

        if(
            it->second.flag==1 &&
            it->second.score<=alpha
        )
            return it->second.score;

        if(
            it->second.flag==2 &&
            it->second.score>=beta
        )
            return it->second.score;
    }

    if(depth<=0)
        return evaluate(x);

    auto moves=legalMoves(x);

    if(moves.empty())
        return
            inCheck(x,x.white)
            ? -100000+depth
            : 0;

    stable_sort(
        moves.begin(),
        moves.end(),
        [&](const Move&a,const Move&b) {
            int sa=
                (a.flags&(CAPTURE|EP_CAPTURE))
                ? pieceValue(x.b[a.to])+1000
                : 0;

            int sb=
                (b.flags&(CAPTURE|EP_CAPTURE))
                ? pieceValue(x.b[b.to])+1000
                : 0;

            return sa>sb;
        }
    );

    int origAlpha=alpha;
    int best=-1000000;

    Move bestMove=moves[0];

    for(const Move&m:moves) {
        if(outOfTime())
            break;

        Board n=makeMove(x,m);

        int s=-search(
            n,
            depth-1,
            -beta,
            -alpha,
            nullptr
        );

        if(s>best) {
            best=s;
            bestMove=m;
        }

        if(s>alpha)
            alpha=s;

        if(alpha>=beta)
            break;
    }

    if(pvBest)
        *pvBest=bestMove;

    uint8_t flag=
        best<=origAlpha
        ? 1
        : (best>=beta?2:0);

    tt[key]=TTEntry{
        depth,
        best,
        flag,
        bestMove
    };

    return best;
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

        int alpha=-1000000;
        int beta=1000000;

        Move current=best;

        int score=search(
            x,
            d,
            alpha,
            beta,
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
    string s;

    ss>>s;

    while(ss>>s) {
        if(s=="wtime")
            ss>>g.wtime;

        else if(s=="btime")
            ss>>g.btime;

        else if(s=="winc")
            ss>>g.winc;

        else if(s=="binc")
            ss>>g.binc;

        else if(s=="movetime")
            ss>>g.movetime;

        else if(s=="depth")
            ss>>g.depth;

        else if(s=="infinite")
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
            20LL,
            g.movetime-20
        );

    if(g.infinite)
        return 3600000;

    long long t=
        x.white
        ? g.wtime
        : g.btime;

    long long inc=
        x.white
        ? g.winc
        : g.binc;

    if(t<0)
        return 1000;

    long long b=
        t/30+
        inc*8/10;

    b=max(30LL,b);

    b=min(
        b,
        max(30LL,t/2)
    );

    return min(
        b,
        5000LL
    );
}

static bool parseUciMove(
    const Board&x,
    const string&s,
    Move&out
) {
    for(const Move&m:legalMoves(x)) {
        if(uciMove(m)==s) {
            out=m;
            return true;
        }
    }

    return false;
}

static void setFEN(
    Board& b,
    const string& fen
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

        if(rank<0||file>7)
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

    b.white=(side!="b");

    if(cast.find('K')!=string::npos)
        b.castle|=1;

    if(cast.find('Q')!=string::npos)
        b.castle|=2;

    if(cast.find('k')!=string::npos)
        b.castle|=4;

    if(cast.find('q')!=string::npos)
        b.castle|=8;

    if(
        ep!="-" &&
        ep.size()==2 &&
        ep[0]>='a'&&ep[0]<='h' &&
        ep[1]>='1'&&ep[1]<='8'
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

    size_t movesPos=
        line.find(" moves",f+13);

    string fen=
        movesPos==string::npos
        ? line.substr(f+13)
        : line.substr(
            f+13,
            movesPos-(f+13)
        );

    setFEN(b,fen);

    if(movesPos!=string::npos) {
        stringstream ss(
            line.substr(movesPos+7)
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
                <<"id name SimpleLogics 0.3 Stable UCI\n";

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

            board.startpos();
        }

        else if(
            line.rfind(
                "position startpos",
                0
            )==0 ||

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

        else if(line.rfind("go",0)==0) {

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

                    int md=
                        g.depth
                        ? g.depth
                        : 64;

                    Move bm=
                        think(pos,md);

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
