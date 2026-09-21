#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
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
    uint8_t from=0, to=0;
    int8_t promotion=0;
    uint8_t flags=QUIET;
};

static inline bool sameMove(const Move&a,const Move&b){
    return a.from==b.from&&a.to==b.to&&a.promotion==b.promotion;
}

static uint64_t zobrist[13][64];
static uint64_t zobSide, zobCastle[16], zobEp[64];

static uint64_t rng64(){
    static uint64_t x=0x9e3779b97f4a7c15ULL;
    x^=x>>12;
    x^=x<<25;
    x^=x>>27;
    return x*0x2545F4914F6CDD1DULL;
}

static void initZobrist(){
    for(auto&a:zobrist)
        for(auto&x:a)
            x=rng64();

    zobSide=rng64();

    for(auto&x:zobCastle)
        x=rng64();

    for(auto&x:zobEp)
        x=rng64();
}

struct Board {
    array<int,64>b{};
    bool white=true;
    uint8_t castle=15;
    int ep=-1;
    int halfmove=0;
    int fullmove=1;

    void startpos(){
        b.fill(EMPTY);

        const int back[8]={
            WR,WN,WB,WQ,WK,WB,WN,WR
        };

        for(int i=0;i<8;i++){
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

    uint64_t key()const{
        uint64_t k=0;

        for(int s=0;s<64;s++){
            if(!b[s])
                continue;

            int idx=
                b[s]>0
                ?(b[s]-1)
                :(6+(-b[s]-1));

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

struct Settings {
    int hashMB=32;
    int skill=20;
    int aggression=50;
    int searchDepth=0;
    int moveOverhead=50;
    int style=0;
};

static Settings cfg;

static atomic<bool> stopSearch(false);

static chrono::steady_clock::time_point deadline;

static uint64_t nodes=0;

static inline int fileOf(int s){
    return s&7;
}

static inline int rankOf(int s){
    return s>>3;
}

static inline bool inside(int f,int r){
    return f>=0&&f<8&&r>=0&&r<8;
}

static inline bool own(int p,bool w){
    return w?p>0:p<0;
}

static inline bool enemy(int p,bool w){
    return w?p<0:p>0;
}

static inline int ptype(int p){
    return abs(p);
}

static int pieceValue(int p){
    switch(abs(p)){
        case WP:return 100;
        case WN:return 320;
        case WB:return 335;
        case WR:return 500;
        case WQ:return 900;
        case WK:return 20000;
    }

    return 0;
}

static string sq(int s){
    string r="a1";

    r[0]=char('a'+fileOf(s));
    r[1]=char('1'+rankOf(s));

    return r;
}

static string uciMove(const Move&m){
    if(m.from>=64||m.to>=64)
        return "0000";

    string r=sq(m.from)+sq(m.to);

    if(m.promotion){
        switch(abs((int)m.promotion)){
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
){
    int f=fileOf(s);
    int r=rankOf(s);

    int pr=byWhite?r-1:r+1;

    if(pr>=0&&pr<8){
        for(int df:{-1,1}){
            int nf=f+df;

            if(
                inside(nf,pr)&&
                x.b[pr*8+nf]==(byWhite?WP:BP)
            )
                return true;
        }
    }

    static const int kn[8][2]={
        {1,2},
        {2,1},
        {2,-1},
        {1,-2},
        {-1,-2},
        {-2,-1},
        {-2,1},
        {-1,2}
    };

    for(auto d:kn){
        int nf=f+d[0];
        int nr=r+d[1];

        if(
            inside(nf,nr)&&
            x.b[nr*8+nf]==(byWhite?WN:BN)
        )
            return true;
    }

    static const int diag[4][2]={
        {1,1},
        {1,-1},
        {-1,1},
        {-1,-1}
    };

    for(auto d:diag){
        int nf=f;
        int nr=r;

        while(true){
            nf+=d[0];
            nr+=d[1];

            if(!inside(nf,nr))
                break;

            int p=x.b[nr*8+nf];

            if(p){
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
        {1,0},
        {-1,0},
        {0,1},
        {0,-1}
    };

    for(auto d:ortho){
        int nf=f;
        int nr=r;

        while(true){
            nf+=d[0];
            nr+=d[1];

            if(!inside(nf,nr))
                break;

            int p=x.b[nr*8+nf];

            if(p){
                if(
                    p==(byWhite?WR:BR)||
                    p==(byWhite?WQ:BQ)
                )
                    return true;

                break;
            }
        }
    }

    for(int df=-1;df<=1;df++){
        for(int dr=-1;dr<=1;dr++){
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
){
    int k=w?WK:BK;

    for(int s=0;s<64;s++){
        if(x.b[s]==k)
            return s;
    }

    return -1;
}

static bool inCheck(
    const Board&x,
    bool w
){
    int k=kingSquare(x,w);

    return k>=0&&attacked(x,k,!w);
}

static void addMove(
    vector<Move>&m,
    int f,
    int t,
    uint8_t flags=QUIET,
    int promo=0
){
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
){
    m.clear();

    bool w=x.white;

    for(int s=0;s<64;s++){
        int p=x.b[s];

        if(!own(p,w))
            continue;

        int f=fileOf(s);
        int r=rankOf(s);
        int a=abs(p);

        if(a==WP){

            int d=w?1:-1;
            int nr=r+d;

            if(inside(f,nr)){

                int t=nr*8+f;

                if(x.b[t]==EMPTY){

                    if(nr==0||nr==7){

                        for(
                            int q:{
                                w?WQ:BQ,
                                w?WR:BR,
                                w?WB:BB,
                                w?WN:BN
                            }
                        )
                            addMove(
                                m,
                                s,
                                t,
                                PROMOTION,
                                q
                            );

                    }else{
                        addMove(m,s,t);
                    }

                    int sr=w?1:6;
                    int nr2=r+2*d;

                    if(
                        r==sr&&
                        x.b[nr2*8+f]==EMPTY
                    )
                        addMove(
                            m,
                            s,
                            nr2*8+f,
                            DOUBLE_PUSH
                        );
                }

                for(int df:{-1,1}){

                    int nf=f+df;

                    if(!inside(nf,nr))
                        continue;

                    t=nr*8+nf;

                    if(
                        enemy(x.b[t],w)&&
                        abs(x.b[t])!=WK
                    ){

                        if(nr==0||nr==7){

                            for(
                                int q:{
                                    w?WQ:BQ,
                                    w?WR:BR,
                                    w?WB:BB,
                                    w?WN:BN
                                }
                            )
                                addMove(
                                    m,
                                    s,
                                    t,
                                    CAPTURE|PROMOTION,
                                    q
                                );

                        }else{

                            addMove(
                                m,
                                s,
                                t,
                                CAPTURE
                            );
                        }
                    }

                    if(t==x.ep){

                        if(nr==0||nr==7){

                            for(
                                int q:{
                                    w?WQ:BQ,
                                    w?WR:BR,
                                    w?WB:BB,
                                    w?WN:BN
                                }
                            )
                                addMove(
                                    m,
                                    s,
                                    t,
                                    EP_CAPTURE|PROMOTION,
                                    q
                                );

                        }else{

                            addMove(
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

        else if(a==WN){

            static const int d[8][2]={
                {1,2},
                {2,1},
                {2,-1},
                {1,-2},
                {-1,-2},
                {-2,-1},
                {-2,1},
                {-1,2}
            };

            for(auto z:d){

                int nf=f+z[0];
                int nr=r+z[1];

                if(!inside(nf,nr))
                    continue;

                int t=nr*8+nf;

                if(
                    !own(x.b[t],w)&&
                    abs(x.b[t])!=WK
                )
                    addMove(
                        m,
                        s,
                        t,
                        x.b[t]?CAPTURE:QUIET
                    );
            }
        }

        else if(
            a==WB||
            a==WR||
            a==WQ
        ){

            static const int dirs[8][2]={
                {1,1},
                {1,-1},
                {-1,1},
                {-1,-1},
                {1,0},
                {-1,0},
                {0,1},
                {0,-1}
            };

            int first=a==WR?4:0;
            int last=a==WB?4:8;

            for(int di=first;di<last;di++){

                int nf=f;
                int nr=r;

                while(true){

                    nf+=dirs[di][0];
                    nr+=dirs[di][1];

                    if(!inside(nf,nr))
                        break;

                    int t=nr*8+nf;

                    if(x.b[t]==EMPTY){

                        addMove(
                            m,
                            s,
                            t
                        );

                    }else{

                        if(
                            enemy(x.b[t],w)&&
                            abs(x.b[t])!=WK
                        )
                            addMove(
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

        else if(a==WK){

            for(int df=-1;df<=1;df++){
                for(int dr=-1;dr<=1;dr++){

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
                        addMove(
                            m,
                            s,
                            t,
                            x.b[t]?CAPTURE:QUIET
                        );
                }
            }

            if(
                w&&
                s==4&&
                !inCheck(x,true)
            ){

                if(
                    (x.castle&1)&&
                    x.b[5]==EMPTY&&
                    x.b[6]==EMPTY&&
                    x.b[7]==WR&&
                    !attacked(x,5,false)&&
                    !attacked(x,6,false)
                )
                    addMove(
                        m,
                        4,
                        6,
                        KING_CASTLE
                    );

                if(
                    (x.castle&2)&&
                    x.b[1]==EMPTY&&
                    x.b[2]==EMPTY&&
                    x.b[3]==EMPTY&&
                    x.b[0]==WR&&
                    !attacked(x,3,false)&&
                    !attacked(x,2,false)
                )
                    addMove(
                        m,
                        4,
                        2,
                        QUEEN_CASTLE
                    );
            }

            if(
                !w&&
                s==60&&
                !inCheck(x,false)
            ){

                if(
                    (x.castle&4)&&
                    x.b[61]==EMPTY&&
                    x.b[62]==EMPTY&&
                    x.b[63]==BR&&
                    !attacked(x,61,true)&&
                    !attacked(x,62,true)
                )
                    addMove(
                        m,
                        60,
                        62,
                        KING_CASTLE
                    );

                if(
                    (x.castle&8)&&
                    x.b[57]==EMPTY&&
                    x.b[58]==EMPTY&&
                    x.b[59]==EMPTY&&
                    x.b[56]==BR&&
                    !attacked(x,59,true)&&
                    !attacked(x,58,true)
                )
                    addMove(
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
){
    Board n=x;

    int p=n.b[m.from];
    int captured=n.b[m.to];

    n.ep=-1;
    n.halfmove++;

    if(
        abs(p)==WP||
        captured||
        (m.flags&EP_CAPTURE)
    )
        n.halfmove=0;

    n.b[m.to]=p;
    n.b[m.from]=EMPTY;

    if(m.flags&EP_CAPTURE){

        int cs=
            m.to+
            (x.white?-8:8);

        n.b[cs]=EMPTY;
    }

    if(m.flags&KING_CASTLE){

        if(x.white){
            n.b[5]=WR;
            n.b[7]=EMPTY;
        }else{
            n.b[61]=BR;
            n.b[63]=EMPTY;
        }
    }

    if(m.flags&QUEEN_CASTLE){

        if(x.white){
            n.b[3]=WR;
            n.b[0]=EMPTY;
        }else{
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

    if(n.white)
        n.fullmove++;

    return n;
}

static Board makeNullMove(
    const Board&x
){
    Board n=x;

    n.white=!x.white;
    n.ep=-1;
    n.halfmove++;

    if(n.white)
        n.fullmove++;

    return n;
}

static vector<Move> legalMoves(
    const Board&x
){
    vector<Move> p,l;

    pseudo(x,p);

    l.reserve(p.size());

    for(const Move&m:p){

        Board n=makeMove(x,m);

        if(!inCheck(n,x.white))
            l.push_back(m);
    }

    return l;
}

static bool insufficientMaterial(
    const Board&x
){
    int pieces=0;
    int bishops=0;
    int knights=0;
    int bishopColor=-1;

    for(int s=0;s<64;s++){

        int p=x.b[s];

        if(!p||abs(p)==WK)
            continue;

        int a=abs(p);

        if(
            a==WP||
            a==WR||
            a==WQ
        )
            return false;

        pieces++;

        if(a==WB){

            bishops++;

            int c=
                (fileOf(s)+rankOf(s))&1;

            if(bishopColor<0)
                bishopColor=c;
            else if(bishopColor!=c)
                return false;

        }else if(a==WN){

            knights++;

        }else{

            return false;
        }
    }

    if(pieces<=1)
        return true;

    if(pieces==2&&bishops==2)
        return true;

    if(pieces==2&&knights==2)
        return true;

    return false;
}

static int evaluate(const Board&x){

    static const int pawn[64]={
         0,  0,  0,  0,  0,  0,  0,  0,
         5, 10, 10,-20,-20, 10, 10,  5,
         5, -5,-10,  0,  0,-10, -5,  5,
         0,  0,  0, 20, 20,  0,  0,  0,
         5,  5, 10, 25, 25, 10,  5,  5,
        10, 10, 20, 30, 30, 20, 10, 10,
        50, 50, 50, 50, 50, 50, 50, 50,
         0,  0,  0,  0,  0,  0,  0,  0
    };

    static const int knight[64]={
        -50,-40,-30,-30,-30,-30,-40,-50,
        -40,-20,  0,  5,  5,  0,-20,-40,
        -30,  5, 10, 15, 15, 10,  5,-30,
        -30,  0, 15, 20, 20, 15,  0,-30,
        -30,  5, 15, 20, 20, 15,  5,-30,
        -30,  0, 10, 15, 15, 10,  0,-30,
        -40,-20,  0,  0,  0,  0,-20,-40,
        -50,-40,-30,-30,-30,-30,-40,-50
    };

    static const int bishop[64]={
        -20,-10,-10,-10,-10,-10,-10,-20,
        -10,  5,  0,  0,  0,  0,  5,-10,
        -10, 10, 10, 10, 10, 10, 10,-10,
        -10,  0, 10, 10, 10, 10,  0,-10,
        -10,  5,  5, 10, 10,  5,  5,-10,
        -10,  0,  5, 10, 10,  5,  0,-10,
        -10,  0,  0,  0,  0,  0,  0,-10,
        -20,-10,-10,-10,-10,-10,-10,-20
    };

    static const int rook[64]={
         0,  0,  0,  5,  5,  0,  0,  0,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
        -5,  0,  0,  0,  0,  0,  0, -5,
         5, 10, 10, 10, 10, 10, 10,  5,
         0,  0,  0,  0,  0,  0,  0,  0
    };

    static const int queen[64]={
        -20,-10,-10, -5, -5,-10,-10,-20,
        -10,  0,  0,  0,  0,  0,  0,-10,
        -10,  0,  5,  5,  5,  5,  0,-10,
         -5,  0,  5,  5,  5,  5,  0, -5,
          0,  0,  5,  5,  5,  5,  0, -5,
        -10,  5,  5,  5,  5,  5,  0,-10,
        -10,  0,  5,  0,  0,  0,  0,-10,
        -20,-10,-10, -5, -5,-10,-10,-20
    };

    static const int kingMid[64]={
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -30,-40,-40,-50,-50,-40,-40,-30,
        -20,-30,-30,-40,-40,-30,-30,-20,
        -10,-20,-20,-20,-20,-20,-20,-10,
         20, 20,  0,  0,  0,  0, 20, 20,
         20, 30, 10,  0,  0, 10, 30, 20
    };

    static const int kingEnd[64]={
        -50,-40,-30,-20,-20,-30,-40,-50,
        -30,-20,-10,  0,  0,-10,-20,-30,
        -30,-10, 20, 30, 30, 20,-10,-30,
        -30,-10, 30, 40, 40, 30,-10,-30,
        -30,-10, 30, 40, 40, 30,-10,-30,
        -30,-10, 20, 30, 30, 20,-10,-30,
        -30,-30,  0,  0,  0,  0,-30,-30,
        -50,-30,-30,-30,-30,-30,-30,-50
    };

    int score=0;
    int totalMaterial=0;
    int bishops[2]={0,0};
    int rooks[2]={0,0};
    int queens[2]={0,0};
    int pawns[2][8]{};
    int pawnRank[2][8]{};

    for(int s=0;s<64;s++){

        int p=x.b[s];

        if(!p)
            continue;

        int side=p>0?0:1;
        int a=abs(p);
        int ps=side==0?s:63-s;

        totalMaterial+=pieceValue(p);

        if(a==WP){
            pawns[side][fileOf(s)]++;
            pawnRank[side][fileOf(s)]=
                max(
                    pawnRank[side][fileOf(s)],
                    side==0?rankOf(s):7-rankOf(s)
                );
        }

        if(a==WB)
            bishops[side]++;

        if(a==WR)
            rooks[side]++;

        if(a==WQ)
            queens[side]++;

        int v=pieceValue(p);

        switch(a){
            case WP:
                v+=pawn[ps];
                break;

            case WN:
                v+=knight[ps];
                break;

            case WB:
                v+=bishop[ps];
                break;

            case WR:
                v+=rook[ps];
                break;

            case WQ:
                v+=queen[ps];
                break;

            case WK:
                v+=kingMid[ps];
                break;
        }

        score+=p>0?v:-v;
    }

    // Bishop pair.
    if(bishops[0]>=2)
        score+=32;

    if(bishops[1]>=2)
        score-=32;

    // Doubled, isolated and advanced pawns.
    for(int side=0;side<2;side++){

        int sign=side==0?1:-1;

        for(int f=0;f<8;f++){

            int count=pawns[side][f];

            if(count>=2)
                score-=sign*12*(count-1);

            if(count>0){

                bool left=
                    f>0&&
                    pawns[side][f-1]>0;

                bool right=
                    f<7&&
                    pawns[side][f+1]>0;

                if(!left&&!right)
                    score-=sign*10;

                score+=sign*
                    pawnRank[side][f]*3;
            }
        }
    }

    // Rooks on open and semi-open files.
    for(int s=0;s<64;s++){

        int p=x.b[s];

        if(abs(p)!=WR)
            continue;

        int side=p>0?0:1;
        int f=fileOf(s);

        bool ownPawn=
            pawns[side][f]>0;

        bool enemyPawn=
            pawns[1-side][f]>0;

        if(!ownPawn&&!enemyPawn)
            score+=(side==0?1:-1)*22;

        else if(!ownPawn)
            score+=(side==0?1:-1)*12;
    }

    // Rooks on the seventh rank.
    for(int s=0;s<64;s++){

        int p=x.b[s];

        if(abs(p)!=WR)
            continue;

        int side=p>0?0:1;
        int r=rankOf(s);

        if(
            (side==0&&r==6)||
            (side==1&&r==1)
        )
            score+=(side==0?1:-1)*18;
    }

    // Mobility.
    {
        Board y=x;
        bool original=x.white;

        vector<Move> whiteMoves;
        vector<Move> blackMoves;

        y.white=true;
        whiteMoves=legalMoves(y);

        y.white=false;
        blackMoves=legalMoves(y);

        int mobility=
            (int)whiteMoves.size()-
            (int)blackMoves.size();

        score+=mobility*3;

        y.white=original;
    }

    // King safety in the middlegame.
    if(totalMaterial>7000){

        for(int side=0;side<2;side++){

            int sign=side==0?1:-1;
            int ks=kingSquare(x,side==0);

            if(ks<0)
                continue;

            int f=fileOf(ks);
            int r=rankOf(ks);

            int danger=0;

            for(int df=-2;df<=2;df++){
                for(int dr=-2;dr<=2;dr++){

                    if(abs(df)+abs(dr)>3)
                        continue;

                    int nf=f+df;
                    int nr=r+dr;

                    if(!inside(nf,nr))
                        continue;

                    int p=x.b[nr*8+nf];

                    if(enemy(p,side==0))
                        danger++;
                }
            }

            score-=sign*danger*4;

            // Castled king bonus.
            if(
                (side==0&&
                 (ks==6||ks==2))||
                (side==1&&
                 (ks==62||ks==58))
            )
                score+=sign*18;
        }
    }

    // Endgame king activity.
    if(totalMaterial<=4500){

        for(int side=0;side<2;side++){

            int sign=side==0?1:-1;
            int ks=kingSquare(x,side==0);

            if(ks>=0){

                int f=fileOf(ks);
                int r=rankOf(ks);

                int center=
                    7-
                    min(
                        7,
                        abs(f-3)+abs(r-3)
                    );

                score+=sign*center*5;

                int ps=side==0?ks:63-ks;

                score+=sign*kingEnd[ps]/2;
            }
        }
    }

    // Aggression setting.
    int aggressive=cfg.aggression-50;

    if(aggressive!=0){

        int openFiles=0;

        for(int f=0;f<8;f++){

            bool wp=pawns[0][f]>0;
            bool bp=pawns[1][f]>0;

            if(!wp&&!bp)
                openFiles++;
        }

        score+=
            aggressive*
            openFiles/
            4;
    }

    return x.white?score:-score;
}

static int evalForSide(
    const Board&x
){
    return evaluate(x);
}

static bool isCapture(
    const Board&x,
    const Move&m
){
    return
        (m.flags&(CAPTURE|EP_CAPTURE))!=0;
}

static int capturedValue(
    const Board&x,
    const Move&m
){
    if(m.flags&EP_CAPTURE)
        return 100;

    if(m.to>=64)
        return 0;

    return pieceValue(x.b[m.to]);
}

struct TTEntry {
    uint64_t key=0;
    int depth=-1;
    int score=0;
    uint8_t flag=0;
    Move best{};
};

static vector<TTEntry> tt;

static void resizeTT(){

    size_t bytes=
        (size_t)max(
            1,
            cfg.hashMB
        )*
        1024ULL*
        1024ULL;

    size_t count=
        max(
            (size_t)1024,
            bytes/
            sizeof(TTEntry)
        );

    count=min(
        count,
        (size_t)4000000
    );

    tt.assign(
        count,
        TTEntry{}
    );
}

static inline TTEntry* probeTT(
    uint64_t key
){
    if(tt.empty())
        return nullptr;

    TTEntry&e=
        tt[
            key%tt.size()
        ];

    if(e.key==key)
        return &e;

    return nullptr;
}

static inline void storeTT(
    uint64_t key,
    int depth,
    int score,
    uint8_t flag,
    const Move&best
){
    if(tt.empty())
        return;

    TTEntry&e=
        tt[
            key%tt.size()
        ];

    if(
        e.key!=key||
        depth>=e.depth
    ){
        e.key=key;
        e.depth=depth;
        e.score=score;
        e.flag=flag;
        e.best=best;
    }
}

static Move killer[128][2];

static int historyTable[2][64][64]{};

static int moveOrder(
    const Board&x,
    const Move&m,
    const Move*ttMove,
    int ply
){
    int score=0;

    if(
        ttMove&&
        sameMove(m,*ttMove)
    )
        score+=20000000;

    if(m.flags&PROMOTION)
        score+=8000000+
               pieceValue(m.promotion);

    if(isCapture(x,m)){

        int victim=
            capturedValue(x,m);

        int attacker=
            pieceValue(
                x.b[m.from]
            );

        score+=
            1000000+
            victim*16-
            attacker;
    }

    if(ply<128){

        if(sameMove(m,killer[ply][0]))
            score+=700000;

        else if(sameMove(m,killer[ply][1]))
            score+=600000;
    }

    int side=x.white?0:1;

    score+=
        historyTable
            [side]
            [m.from]
            [m.to];

    return score;
}

static void orderMoves(
    const Board&x,
    vector<Move>&moves,
    const Move*ttMove,
    int ply
){
    stable_sort(
        moves.begin(),
        moves.end(),
        [&](const Move&a,const Move&b){
            return
                moveOrder(
                    x,
                    a,
                    ttMove,
                    ply
                )>
                moveOrder(
                    x,
                    b,
                    ttMove,
                    ply
                );
        }
    );
}

static bool shouldStop(){
    return
        stopSearch.load()||
        chrono::steady_clock::now()>=deadline;
}

static int quiescence(
    const Board&x,
    int alpha,
    int beta,
    int ply
){
    if(shouldStop())
        return evaluate(x);

    nodes++;

    int stand=evaluate(x);

    if(stand>=beta)
        return beta;

    if(stand>alpha)
        alpha=stand;

    if(ply>=10)
        return alpha;

    vector<Move>moves;

    pseudo(x,moves);

    orderMoves(
        x,
        moves,
        nullptr,
        ply
    );

    for(const Move&m:moves){

        if(
            !isCapture(x,m)&&
            !(m.flags&PROMOTION)
        )
            continue;

        Board n=makeMove(x,m);

        if(inCheck(n,x.white))
            continue;

        int score=
            -quiescence(
                n,
                -beta,
                -alpha,
                ply+1
            );

        if(score>=beta)
            return beta;

        if(score>alpha)
            alpha=score;
    }

    return alpha;
}

static int search(
    const Board&x,
    int depth,
    int alpha,
    int beta,
    int ply,
    bool allowNull,
    Move*pvBest
){
    if(shouldStop())
        return evaluate(x);

    nodes++;

    if(
        x.halfmove>=100||
        insufficientMaterial(x)
    )
        return 0;

    uint64_t key=x.key();

    TTEntry*entry=probeTT(key);

    Move ttMove{};

    if(entry){
        ttMove=entry->best;

        if(
            entry->depth>=depth&&
            ply>0
        ){
            if(entry->flag==0)
                return entry->score;

            if(
                entry->flag==1&&
                entry->score<=alpha
            )
                return entry->score;

            if(
                entry->flag==2&&
                entry->score>=beta
            )
                return entry->score;
        }
    }

    bool check=inCheck(x,x.white);

    if(depth<=0)
        return quiescence(
            x,
            alpha,
            beta,
            ply
        );

    vector<Move>moves=legalMoves(x);

    if(moves.empty()){

        if(check)
            return -1000000+ply;

        return 0;
    }

    if(
        allowNull&&
        depth>=3&&
        !check&&
        ply>0
    ){

        bool hasBigPiece=false;

        for(int p:x.b){

            if(
                own(p,x.white)&&
                abs(p)>=WQ
            ){
                hasBigPiece=true;
                break;
            }
        }

        if(hasBigPiece){

            Board n=makeNullMove(x);

            int reduction=
                depth>=6?3:2;

            int score=
                -search(
                    n,
                    depth-1-reduction,
                    -beta,
                    -beta+1,
                    ply+1,
                    false,
                    nullptr
                );

            if(score>=beta)
                return beta;
        }
    }

    orderMoves(
        x,
        moves,
        entry?&ttMove:nullptr,
        ply
    );

    int originalAlpha=alpha;

    int bestScore=-10000000;

    Move bestMove=moves[0];

    int moveNumber=0;

    for(const Move&m:moves){

        if(shouldStop())
            break;

        moveNumber++;

        Board n=makeMove(x,m);

        bool givesCheck=
            inCheck(n,n.white);

        int extension=
            givesCheck?1:0;

        int nextDepth=
            depth-1+extension;

        int score;

        bool tactical=
            isCapture(x,m)||
            (m.flags&PROMOTION);

        // Late Move Reduction.
        bool reduce=
            moveNumber>=4&&
            depth>=3&&
            !tactical&&
            !check&&
            !givesCheck&&
            cfg.skill>=10;

        if(reduce)
            nextDepth=max(
                1,
                nextDepth-1
            );

        if(moveNumber==1){

            score=
                -search(
                    n,
                    nextDepth,
                    -beta,
                    -alpha,
                    ply+1,
                    true,
                    nullptr
                );

        }else{

            score=
                -search(
                    n,
                    nextDepth,
                    -alpha-1,
                    -alpha,
                    ply+1,
                    true,
                    nullptr
                );

            if(
                score>alpha&&
                score<beta
            ){
                score=
                    -search(
                        n,
                        nextDepth,
                        -beta,
                        -alpha,
                        ply+1,
                        true,
                        nullptr
                    );
            }
        }

        if(score>bestScore){
            bestScore=score;
            bestMove=m;
        }

        if(score>alpha){

            alpha=score;

            if(pvBest)
                *pvBest=m;
        }

        if(alpha>=beta){

            if(!tactical){

                if(
                    !sameMove(
                        m,
                        killer[ply][0]
                    )
                ){
                    killer[ply][1]=
                        killer[ply][0];

                    killer[ply][0]=m;
                }

                int side=
                    x.white?0:1;

                int bonus=
                    depth*depth*4;

                historyTable
                    [side]
                    [m.from]
                    [m.to]
                    =min(
                        100000,
                        historyTable
                            [side]
                            [m.from]
                            [m.to]
                        +bonus
                    );
            }

            break;
        }
    }

    if(shouldStop())
        return bestScore;

    uint8_t flag=
        bestScore<=originalAlpha
        ?1
        :(bestScore>=beta?2:0);

    storeTT(
        key,
        depth,
        bestScore,
        flag,
        bestMove
    );

    return bestScore;
}

static int contemptValue(){
    switch(cfg.style){
        case 1:
            return 8;

        case 2:
            return 12;

        case 3:
            return -4;

        default:
            return 0;
    }
}

static Move think(
    const Board&x,
    int maxDepth
){
    vector<Move>root=
        legalMoves(x);

    if(root.empty())
        return Move{0,0,0,QUIET};

    Move best=root[0];

    int bestScore=-10000000;

    int completedDepth=0;

    for(int depth=1;
        depth<=maxDepth;
        depth++){

        if(shouldStop())
            break;

        Move current=best;

        int score=
            search(
                x,
                depth,
                -10000000,
                10000000,
                0,
                true,
                &current
            );

        if(shouldStop())
            break;

        best=current;
        bestScore=score;
        completedDepth=depth;

        cout
            <<"info depth "
            <<depth
            <<" score cp "
            <<bestScore
            <<" nodes "
            <<nodes
            <<" pv "
            <<uciMove(best)
            <<"\n";

        cout.flush();

        // Very low skill deliberately limits
        // search depth while preserving legal play.
        if(
            cfg.skill<=3&&
            depth>=4
        )
            break;

        if(
            cfg.skill<=7&&
            depth>=6
        )
            break;
    }

    // If the timer expires before depth 1 completes,
    // always return a legal move.
    if(completedDepth==0)
        best=root[0];

    return best;
}

static int parseInt(
    const string&s,
    int fallback
){
    try{
        return stoi(s);
    }catch(...){
        return fallback;
    }
}

static string lowerString(
    string s
){
    for(char&c:s)
        c=(char)tolower(
            (unsigned char)c
        );

    return s;
}

static void setOption(
    const string&name,
    const string&value
){
    string n=lowerString(name);
    string v=lowerString(value);

    if(n=="hash"){

        int h=parseInt(
            value,
            cfg.hashMB
        );

        cfg.hashMB=
            max(
                1,
                min(512,h)
            );

        resizeTT();
    }

    else if(n=="skill level"){

        int s=parseInt(
            value,
            cfg.skill
        );

        cfg.skill=
            max(
                0,
                min(20,s)
            );
    }

    else if(n=="aggression"){

        int a=parseInt(
            value,
            cfg.aggression
        );

        cfg.aggression=
            max(
                0,
                min(100,a)
            );
    }

    else if(n=="search depth"){

        int d=parseInt(
            value,
            cfg.searchDepth
        );

        cfg.searchDepth=
            max(
                0,
                min(64,d)
            );
    }

    else if(n=="move overhead"){

        int o=parseInt(
            value,
            cfg.moveOverhead
        );

        cfg.moveOverhead=
            max(
                0,
                min(1000,o)
            );
    }

    else if(n=="style"){

        if(v=="normal")
            cfg.style=0;

        else if(v=="aggressive")
            cfg.style=1;

        else if(v=="tal")
            cfg.style=2;

        else if(v=="human")
            cfg.style=3;

        else
            cfg.style=0;
    }
}

static Go parseGo(
    const string&line
);

struct Go{
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
){
    Go g;

    stringstream ss(line);
    string t;

    ss>>t;

    while(ss>>t){

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
){
    if(g.movetime>=0){

        return max(
            20LL,
            g.movetime-
            cfg.moveOverhead
        );
    }

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

    // Conservative allocation:
    // preserve enough time for later moves.
    long long budget=
        time/32+
        inc*3/4;

    budget=max(
        30LL,
        budget
    );

    budget=min(
        budget,
        max(
            30LL,
            time/3
        )
    );

    budget=min(
        budget,
        5000LL
    );

    return budget;
}

static bool parseBool(const string& s){
    string v=lowerString(s);

    return v=="true" ||
           v=="1" ||
           v=="yes" ||
           v=="on";
}

static int parseStyle(const string& s){

    string v=lowerString(s);

    if(v=="aggressive")
        return 1;

    if(v=="tal")
        return 2;

    if(v=="human")
        return 3;

    return 0;
}

static void applyStyleDefaults(){

    if(cfg.style==1){
        // Aggressive
        cfg.aggression=max(cfg.aggression,75);
    }

    else if(cfg.style==2){
        // Tal
        cfg.aggression=max(cfg.aggression,90);
    }

    else if(cfg.style==3){
        // Human
        cfg.aggression=min(cfg.aggression,45);
    }
}

static void setOptionLine(const string&line){

    if(line.rfind("setoption name ",0)!=0)
        return;

    string s=line.substr(15);

    size_t pos=s.find(" value ");

    string name;
    string value;

    if(pos==string::npos){

        name=s;
        value="";
    }

    else{

        name=s.substr(0,pos);
        value=s.substr(pos+7);
    }

    while(!name.empty() &&
          isspace((unsigned char)name.back()))
        name.pop_back();

    while(!value.empty() &&
          isspace((unsigned char)value.front()))
        value.erase(value.begin());

    string lname=lowerString(name);

    if(lname=="hash"){

        int mb=parseInt(value,32);

        mb=max(1,min(512,mb));

        cfg.hashMB=mb;

        resizeTT(cfg.hashMB);
    }

    else if(lname=="skill level"){

        cfg.skill=
            max(0,min(20,
                parseInt(value,20)));
    }

    else if(lname=="aggression"){

        cfg.aggression=
            max(0,min(100,
                parseInt(value,50)));
    }

    else if(lname=="search depth"){

        cfg.searchDepth=
            max(0,min(64,
                parseInt(value,0)));
    }

    else if(lname=="move overhead"){

        cfg.moveOverhead=
            max(0,min(1000,
                parseInt(value,50)));
    }

    else if(lname=="style"){

        cfg.style=parseStyle(value);

        applyStyleDefaults();
    }
}

static bool parseSquare(
    const string&s,
    int&sqOut
){

    if(s.size()!=2)
        return false;

    char f=s[0];
    char r=s[1];

    if(f<'a' || f>'h')
        return false;

    if(r<'1' || r>'8')
        return false;

    sqOut=sq(
        f-'a',
        r-'1'
    );

    return true;
}

static bool parseFEN(
    const string&fen,
    Board&b
){

    vector<string> parts;

    string token;

    stringstream ss(fen);

    while(ss>>token)
        parts.push_back(token);

    if(parts.size()<4)
        return false;

    Board n;

    for(int i=0;i<64;i++)
        n.b[i]=EMPTY;

    int rank=7;
    int file=0;

    for(char c:parts[0]){

        if(c=='/'){

            rank--;

            file=0;

            if(rank<0)
                return false;

            continue;
        }

        if(isdigit((unsigned char)c)){

            int count=c-'0';

            if(count<1 || count>8)
                return false;

            file+=count;

            if(file>8)
                return false;

            continue;
        }

        if(file>=8 || rank<0)
            return false;

        int p=EMPTY;

        switch(c){

            case 'P': p=WP; break;
            case 'N': p=WN; break;
            case 'B': p=WB; break;
            case 'R': p=WR; break;
            case 'Q': p=WQ; break;
            case 'K': p=WK; break;

            case 'p': p=BP; break;
            case 'n': p=BN; break;
            case 'b': p=BB; break;
            case 'r': p=BR; break;
            case 'q': p=BQ; break;
            case 'k': p=BK; break;

            default:
                return false;
        }

        n.b[sq(file,rank)]=p;

        file++;
    }

    if(rank!=0 || file!=8)
        return false;

    n.white=(parts[1]=="w");

    n.castle=0;

    if(parts[2]!="-"){

        for(char c:parts[2]){

            if(c=='K')
                n.castle|=1;

            else if(c=='Q')
                n.castle|=2;

            else if(c=='k')
                n.castle|=4;

            else if(c=='q')
                n.castle|=8;
        }
    }

    n.ep=-1;

    if(parts[3]!="-"){

        int epSq;

        if(!parseSquare(
            parts[3],
            epSq
        ))
            return false;

        n.ep=epSq;
    }

    n.halfmove=0;
    n.fullmove=1;

    if(parts.size()>=5){

        n.halfmove=
            max(
                0,
                parseInt(parts[4],0)
            );
    }

    if(parts.size()>=6){

        n.fullmove=
            max(
                1,
                parseInt(parts[5],1)
            );
    }

    b=n;

    return true;
}

static bool parseMoveString(
    Board&b,
    const string&u
){

    if(u.size()<4)
        return false;

    int from;
    int to;

    if(!parseSquare(
        u.substr(0,2),
        from
    ))
        return false;

    if(!parseSquare(
        u.substr(2,2),
        to
    ))
        return false;

    int promotion=EMPTY;

    if(u.size()>=5){

        char c=
            tolower(
                (unsigned char)u[4]
            );

        if(b.white){

            if(c=='q')
                promotion=WQ;

            else if(c=='r')
                promotion=WR;

            else if(c=='b')
                promotion=WB;

            else if(c=='n')
                promotion=WN;
        }

        else{

            if(c=='q')
                promotion=BQ;

            else if(c=='r')
                promotion=BR;

            else if(c=='b')
                promotion=BB;

            else if(c=='n')
                promotion=BN;
        }
    }

    vector<Move> moves=
        legalMoves(b);

    for(const Move&m:moves){

        if(m.from!=from ||
           m.to!=to)
            continue;

        if(promotion!=EMPTY){

            if(m.promotion!=promotion)
                continue;
        }

        else if(m.promotion!=EMPTY){

            continue;
        }

        b=makeMove(b,m);

        return true;
    }

    return false;
}

static void setPosition(
    Board&b,
    const string&line
){

    stringstream ss(line);

    string token;

    ss>>token;

    if(!(ss>>token))
        return;

    if(token=="startpos"){

        b=Board::startpos();

        if(ss>>token){

            if(token=="moves"){

                string mv;

                while(ss>>mv)
                    parseMoveString(b,mv);
            }
        }

        return;
    }

    if(token=="fen"){

        vector<string> fenParts;

        while(ss>>token){

            if(token=="moves")
                break;

            fenParts.push_back(token);

            if(fenParts.size()==6)
                break;
        }

        if(fenParts.size()<4)
            return;

        string fen;

        for(size_t i=0;
            i<fenParts.size();
            i++){

            if(i)
                fen+=' ';

            fen+=fenParts[i];
        }

        Board n=b;

        if(!parseFEN(fen,n))
            return;

        b=n;

        if(token=="moves"){

            string mv;

            while(ss>>mv)
                parseMoveString(b,mv);
        }

        else{

            if(ss>>token &&
               token=="moves"){

                string mv;

                while(ss>>mv)
                    parseMoveString(b,mv);
            }
        }
    }
}

static string bestMoveUCI(
    const Board&b
){

    int maxDepth=64;

    if(cfg.searchDepth>0)
        maxDepth=cfg.searchDepth;

    if(cfg.skill<20){

        int skillDepth;

        if(cfg.skill<=2)
            skillDepth=2;

        else if(cfg.skill<=5)
            skillDepth=3;

        else if(cfg.skill<=8)
            skillDepth=4;

        else if(cfg.skill<=11)
            skillDepth=5;

        else if(cfg.skill<=14)
            skillDepth=6;

        else if(cfg.skill<=17)
            skillDepth=7;

        else
            skillDepth=8;

        maxDepth=
            min(
                maxDepth,
                skillDepth
            );
    }

    if(maxDepth<1)
        maxDepth=1;

    return think(
        b,
        maxDepth
    );
}

static void uciInfo(
    const Board&b
){

    cout
        <<"info string "
        <<"SimpleLogics ready"
        <<endl;
}

static void printUCI(){

    cout
        <<"id name SimpleLogics Final"
        <<endl;

    cout
        <<"id author Danny"
        <<endl;

    cout
        <<"option name Hash type spin "
        <<"default 32 min 1 max 512"
        <<endl;

    cout
        <<"option name Skill Level type spin "
        <<"default 20 min 0 max 20"
        <<endl;

    cout
        <<"option name Aggression type spin "
        <<"default 50 min 0 max 100"
        <<endl;

    cout
        <<"option name Search Depth type spin "
        <<"default 0 min 0 max 64"
        <<endl;

    cout
        <<"option name Move Overhead type spin "
        <<"default 50 min 0 max 1000"
        <<endl;

    cout
        <<"option name Style type combo "
        <<"default Normal "
        <<"var Normal "
        <<"var Aggressive "
        <<"var Tal "
        <<"var Human"
        <<endl;

    cout<<"uciok"<<endl;
}

static void prepareEngine(){

    initZobrist();

    resizeTT(
        cfg.hashMB
    );

    for(auto&row:killer)
        row={};

    for(auto&row:history)
        row.fill(0);

    gStop.store(false);

    gNodes=0;
}

static void startSearchClock(
    int ms
){

    gStop.store(false);

    if(ms<=0){

        gDeadline=
            chrono::steady_clock::time_point::max();

        return;
    }

    gDeadline=
        chrono::steady_clock::now()+
        chrono::milliseconds(ms);
}

static void stopSearch(){

    gStop.store(true);
}

static void handleGo(
    const Board&b,
    const string&line
){

    Go g=parseGo(line);

    int budget=
        timeBudget(
            b,
            g
        );

    startSearchClock(
        budget
    );

    Board root=b;

    string best=
        bestMoveUCI(root);

    if(best.empty()){

        vector<Move> moves=
            legalMoves(root);

        if(!moves.empty())
            best=uciMove(moves[0]);
    }

    stopSearch();

    cout
        <<"bestmove "
        <<best
        <<endl;
}

static void runUCI(){

    prepareEngine();

    Board board=
        Board::startpos();

    string line;

    while(getline(cin,line)){

        if(line.empty())
            continue;

        string cmd;

        {
            stringstream ss(line);

            ss>>cmd;
        }

        if(cmd=="uci"){

            printUCI();
        }

        else if(cmd=="isready"){

            cout<<"readyok"<<endl;
        }

        else if(cmd=="setoption"){

            setOptionLine(line);
        }

        else if(cmd=="ucinewgame"){

            gStop.store(true);

            tt.clear();

            tt.shrink_to_fit();

            resizeTT(
                cfg.hashMB
            );

            for(auto&row:killer)
                row={};

            for(auto&row:history)
                row.fill(0);
        }

        else if(cmd=="position"){

            setPosition(
                board,
                line
            );
        }

        else if(cmd=="go"){

            handleGo(
                board,
                line
            );
        }

        else if(cmd=="stop"){

            stopSearch();
        }

        else if(cmd=="ponderhit"){

            gStop.store(false);
        }

        else if(cmd=="debug"){

            // accepted
        }

        else if(cmd=="quit"){

            stopSearch();

            break;
        }
    }
}

int main(){

    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    /*
        Jalankan UCI engine.
        DroidFish akan berkomunikasi melalui
        stdin/stdout menggunakan protokol UCI.
    */
    runUCI();

    return 0;
}
