#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <array>
#include <algorithm>

using namespace std;

enum {
    EMPTY=0,
    WP=1, WN=2, WB=3, WR=4, WQ=5, WK=6,
    BP=-1, BN=-2, BB=-3, BR=-4, BQ=-5, BK=-6
};

struct Move {
    int from, to;
};

struct Board {
    array<int,64> b{};
    bool white=true;

    void startpos() {
        b.fill(EMPTY);

        int back[8]={WR,WN,WB,WQ,WK,WB,WN,WR};

        for(int i=0;i<8;i++) {
            b[i]=back[i];
            b[8+i]=WP;
            b[48+i]=BP;
            b[56+i]=-back[i];
        }

        white=true;
    }
};

int value(int p) {
    switch(abs(p)) {
        case WP: return 100;
        case WN: return 320;
        case WB: return 330;
        case WR: return 500;
        case WQ: return 900;
        case WK: return 20000;
    }
    return 0;
}

bool own(int p,bool white) {
    return white ? p>0 : p<0;
}

bool enemy(int p,bool white) {
    return white ? p<0 : p>0;
}

int file(int s) {
    return s&7;
}

int rank_(int s) {
    return s>>3;
}

bool inside(int f,int r) {
    return f>=0 && f<8 && r>=0 && r<8;
}

void addSlide(
    const Board& b,
    vector<Move>& moves,
    int from,
    int df,
    int dr
) {
    int f=file(from);
    int r=rank_(from);

    while(true) {
        f+=df;
        r+=dr;

        if(!inside(f,r))
            break;

        int to=r*8+f;

        if(b.b[to]==EMPTY) {
            moves.push_back({from,to});
        } else {
            if(enemy(b.b[to],b.white))
                moves.push_back({from,to});
            break;
        }
    }
}

vector<Move> generateMoves(const Board& b) {
    vector<Move> moves;

    for(int s=0;s<64;s++) {
        int p=b.b[s];

        if(!own(p,b.white))
            continue;

        int f=file(s);
        int r=rank_(s);

        switch(abs(p)) {

        case WP: {
            int d=b.white?1:-1;
            int nr=r+d;

            if(inside(f,nr)) {
                int to=nr*8+f;

                if(b.b[to]==EMPTY)
                    moves.push_back({s,to});

                for(int df:{-1,1}) {
                    int nf=f+df;

                    if(!inside(nf,nr))
                        continue;

                    to=nr*8+nf;

                    if(enemy(b.b[to],b.white))
                        moves.push_back({s,to});
                }
            }
            break;
        }

        case WN: {
            static int d[8][2]={
                {1,2},{2,1},{2,-1},{1,-2},
                {-1,-2},{-2,-1},{-2,1},{-1,2}
            };

            for(auto &x:d) {
                int nf=f+x[0];
                int nr=r+x[1];

                if(!inside(nf,nr))
                    continue;

                int to=nr*8+nf;

                if(!own(b.b[to],b.white))
                    moves.push_back({s,to});
            }

            break;
        }

        case WB:
            addSlide(b,moves,s,1,1);
            addSlide(b,moves,s,1,-1);
            addSlide(b,moves,s,-1,1);
            addSlide(b,moves,s,-1,-1);
            break;

        case WR:
            addSlide(b,moves,s,1,0);
            addSlide(b,moves,s,-1,0);
            addSlide(b,moves,s,0,1);
            addSlide(b,moves,s,0,-1);
            break;

        case WQ:
            addSlide(b,moves,s,1,1);
            addSlide(b,moves,s,1,-1);
            addSlide(b,moves,s,-1,1);
            addSlide(b,moves,s,-1,-1);
            addSlide(b,moves,s,1,0);
            addSlide(b,moves,s,-1,0);
            addSlide(b,moves,s,0,1);
            addSlide(b,moves,s,0,-1);
            break;

        case WK:
            for(int df=-1;df<=1;df++) {
                for(int dr=-1;dr<=1;dr++) {
                    if(df==0 && dr==0)
                        continue;

                    int nf=f+df;
                    int nr=r+dr;

                    if(!inside(nf,nr))
                        continue;

                    int to=nr*8+nf;

                    if(!own(b.b[to],b.white))
                        moves.push_back({s,to});
                }
            }
            break;
        }
    }

    return moves;
}

Board makeMove(const Board& b,const Move& m) {
    Board n=b;

    n.b[m.to]=n.b[m.from];
    n.b[m.from]=EMPTY;

    n.white=!b.white;

    return n;
}

int evaluate(const Board& b) {
    int score=0;

    for(int p:b.b) {
        if(p>0)
            score+=value(p);
        else
            score-=value(p);
    }

    return b.white ? score : -score;
}

int search(
    const Board& b,
    int depth,
    int alpha,
    int beta
) {
    if(depth==0)
        return evaluate(b);

    vector<Move> moves=generateMoves(b);

    if(moves.empty())
        return evaluate(b);

    int best=-1000000;

    for(const Move& m:moves) {
        Board n=makeMove(b,m);

        int score=-search(
            n,
            depth-1,
            -beta,
            -alpha
        );

        best=max(best,score);
        alpha=max(alpha,score);

        if(alpha>=beta)
            break;
    }

    return best;
}

string squareName(int s) {
    string r="a1";

    r[0]='a'+file(s);
    r[1]='1'+rank_(s);

    return r;
}

string moveName(const Move& m) {
    return squareName(m.from)+squareName(m.to);
}

Move bestMove(const Board& b,int depth) {
    vector<Move> moves=generateMoves(b);

    Move best=moves[0];
    int bestScore=-1000000;

    for(const Move& m:moves) {
        Board n=makeMove(b,m);

        int score=-search(
            n,
            depth-1,
            -1000000,
            1000000
        );

        if(score>bestScore) {
            bestScore=score;
            best=m;
        }
    }

    return best;
}

void positionStart(Board& b) {
    b.startpos();
}

int main() {

    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    Board board;
    board.startpos();

    string line;

    while(getline(cin,line)) {

        if(line=="uci") {

            cout<<"id name DannyNNUE 0.1\n";
            cout<<"id author Danny\n";
            cout<<"uciok\n";
            cout.flush();

        } else if(line=="isready") {

            cout<<"readyok\n";
            cout.flush();

        } else if(line=="ucinewgame") {

            board.startpos();

        } else if(line=="quit") {

            break;

        } else if(line=="stop") {

        } else if(line.rfind("position startpos",0)==0) {

            board.startpos();

            size_t pos=line.find("moves");

            if(pos!=string::npos) {

                string moves=line.substr(pos+6);
                stringstream ss(moves);
                string mv;

                while(ss>>mv) {

                    vector<Move> legal=
                        generateMoves(board);

                    for(const Move& m:legal) {

                        if(moveName(m)==mv) {

                            board=makeMove(board,m);
                            break;
                        }
                    }
                }
            }

        } else if(line.rfind("go",0)==0) {

            Move m=bestMove(board,4);

            cout<<"bestmove "
                <<moveName(m)
                <<"\n";

            cout.flush();
        }
    }

    return 0;
}
